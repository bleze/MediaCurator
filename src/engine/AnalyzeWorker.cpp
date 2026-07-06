#include "engine/AnalyzeWorker.h"
#include "engine/ActionEngine.h"
#include "engine/RuleEngine.h"
#include "engine/TrackDecision.h"
#include "scanner/OriginalLanguageDetector.h"
#include "core/DatabaseManager.h"
#include "core/UserProfile.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace Mc {

AnalyzeWorker::AnalyzeWorker(QList<FileRecord> files,
                             const UserProfile* profile,
                             const QString& mkvmergePath,
                             QHash<qint64, QSet<int>> forcedRemovals,
                             QObject* parent)
	: QObject(parent)
	, m_files(std::move(files))
	, m_profile(profile)
	, m_mkvmergePath(mkvmergePath)
	, m_forcedRemovals(std::move(forcedRemovals))
{}

void AnalyzeWorker::run()
{
	const int total = m_files.size();
	int created = 0;

	for (int i = 0; i < total; ++i) {
		if (m_cancelled.loadRelaxed()) break;
		const FileRecord& file = m_files.at(i);
		emit progress(i + 1, total, file.filename);
		if (analyzeFile(file))
			++created;
	}

	emit finished(total, created);
}

bool AnalyzeWorker::analyzeFile(const FileRecord& fileIn)
{
	auto& db = DatabaseManager::instance();

	FileRecord file = fileIn;
	const auto streams = db.streamsForFile(file.id);

	if (file.originalLanguage.isEmpty()) {
		const QString lang = OriginalLanguageDetector::detect(file, streams);
		if (!lang.isEmpty()) {
			db.updateFileOriginalLanguage(file.id, lang);
			file.originalLanguage = lang;
		}
	}

	// Snapshot streams before rule evaluation so done-job cards can show
	// removed tracks (the file is rescanned after completion, renumbering indices).
	QJsonArray origArr;
	for (const StreamRecord& s : streams) {
		QJsonObject o;
		o["idx"]     = s.streamIndex;
		o["type"]    = s.codecType;
		o["codec"]   = s.codecName;
		o["profile"] = s.codecProfile;
		o["lang"]    = s.language;
		o["title"]   = s.title;
		o["ch"]      = s.channels;
		o["w"]       = s.width;
		o["h"]       = s.height;
		o["hdr"]     = s.hdrFormat;
		o["default"]    = s.isDefault;
		o["forced"]     = s.isForced;
		o["original"]   = s.isOriginal;
		o["commentary"] = s.isCommentary;
		o["hi"]         = s.isHearingImpaired;
		origArr.append(o);
	}

	RuleEngine   engine(m_profile);
	ActionEngine actions(m_mkvmergePath);

	FileDecision decision = engine.evaluateFile(file, streams);

	// Apply manual badge overrides pre-fetched from the UI model on the main thread
	const QSet<int>& forcedRemovals = m_forcedRemovals.value(file.id);
	for (auto& td : decision.tracks) {
		if (forcedRemovals.contains(td.stream.streamIndex)
				&& td.decision == Decision::Keep) {
			td.decision     = Decision::Remove;
			td.reason       = QStringLiteral("Manually marked for removal");
			td.userOverride = true;
		}
	}

	// A file with no internal tracks to remove may still need a remux just to
	// absorb an external (sidecar) subtitle into the container — mkvpropedit
	// can't add tracks, so this is the only way that ever happens.
	QList<StreamRecord> unmuxedSidecars;
	for (const StreamRecord& s : streams)
		if (s.isExternal && !s.externalPath.isEmpty()) unmuxedSidecars << s;

	if (decision.removalCount() == 0 && unmuxedSidecars.isEmpty()) return false;

	int audioRemoved = 0, subRemoved = 0;
	for (const auto& td : decision.tracks) {
		if (td.decision != Decision::Remove || td.stream.isExternal) continue;
		if (td.stream.codecType == "audio")    ++audioRemoved;
		if (td.stream.codecType == "subtitle") ++subRemoved;
	}

	if (m_profile->skipSubtitleOnlyJobs() && audioRemoved == 0) return false;

	QStringList parts;
	if (audioRemoved > 0) parts << QStringLiteral("%1 audio").arg(audioRemoved);
	if (subRemoved   > 0) parts << QStringLiteral("%1 subtitle").arg(subRemoved);
	const QString summary = parts.isEmpty()
		? (unmuxedSidecars.size() == 1
		       ? QStringLiteral("Mux in external subtitle")
		       : QStringLiteral("Mux in %1 external subtitles").arg(unmuxedSidecars.size()))
		: QStringLiteral("Remove ") + parts.join(QStringLiteral(", "));

	QStringList descLines;
	for (const auto& td : decision.tracks) {
		if (td.decision != Decision::Remove || td.stream.isExternal) continue;
		const StreamRecord& s = td.stream;
		QString line = QStringLiteral("  [%1] %2").arg(s.codecType.toUpper(), s.codecName);
		if (!s.language.isEmpty() && s.language != QLatin1String("und"))
			line += QStringLiteral(" (%1)").arg(s.language);
		if (!s.title.isEmpty())
			line += QStringLiteral(" \"%1\"").arg(s.title);
		if (!td.reason.isEmpty())
			line += QStringLiteral(" — %1").arg(td.reason);
		descLines << line;
	}
	for (const StreamRecord& s : unmuxedSidecars) {
		QString line = QStringLiteral("  [SUBTITLE] %1").arg(s.codecName);
		if (!s.language.isEmpty() && s.language != QLatin1String("und"))
			line += QStringLiteral(" (%1)").arg(s.language);
		line += QStringLiteral(" — external, will be muxed in");
		descLines << line;
	}

	// Non-MKV / ISO files output to a new .mkv file; MKV files overwrite in place.
	const QFileInfo   fi(file.path);
	const QString     ext = fi.suffix().toLower();
	const bool        isMkv = (ext == QLatin1String("mkv") || ext == QLatin1String("mka") || ext == QLatin1String("mks"));
	const QString     outputPath = isMkv
		? file.path + QLatin1String(".tmp")
		: fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QLatin1String(".mkv.tmp");
	const QStringList args       = actions.buildCommand(decision, outputPath);
	QJsonArray        arr;
	for (const QString& a : args) arr.append(a);

	QString inheritedFlagChanges;
	if (const auto existingJob = db.activeJobForFile(file.id)) {
		if (existingJob->jobType != QLatin1String("tag_edit")) return false;
		inheritedFlagChanges = existingJob->flagChangesJson;
		db.deleteJob(existingJob->id);
	}

	const qint64 estimatedSavings = decision.estimatedSavingBytes();

	JobRecord job;
	job.fileId          = file.id;
	job.status          = QStringLiteral("proposed");
	job.jobType         = QStringLiteral("remux");
	job.commandArgsJson = QJsonDocument(arr).toJson(QJsonDocument::Compact);
	job.summary             = summary;
	job.descriptionText     = descLines.join(QLatin1Char('\n'));
	job.originalStreamsJson  = QJsonDocument(origArr).toJson(QJsonDocument::Compact);
	job.savedBytes          = estimatedSavings;
	job.estimatedSavedBytes = estimatedSavings;
	job.streamEstimatesJson = decision.streamEstimatesJson();
	job.flagChangesJson     = inheritedFlagChanges;
	(void)db.insertJob(job);

	emit jobProposed(file.id);
	return true;
}

} // namespace Mc
