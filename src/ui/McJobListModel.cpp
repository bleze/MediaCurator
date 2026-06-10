#include "ui/McJobListModel.h"
#include "ui/McFilterPanel.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>

namespace Mc {

McJobListModel::McJobListModel(QObject* parent)
	: QAbstractListModel(parent)
{}

// ── Public API ────────────────────────────────────────────────────────────────

void McJobListModel::reload()
{
	m_allEntries.clear();
	m_allCheckStates.clear();
	m_posterPaths = DatabaseManager::instance().allDonePosterPaths();

	auto& db = DatabaseManager::instance();
	const auto displayJobs = db.allJobsForPanel(m_sortMode);

	const auto allJobRecords = db.allJobs();
	QHash<qint64, QString> argsMap;
	QHash<qint64, QString> origStreamsMap;
	QSet<qint64> liveJobIds;
	for (const JobRecord& j : allJobRecords) {
		argsMap.insert(j.id, j.commandArgsJson);
		origStreamsMap.insert(j.id, j.originalStreamsJson);
		liveJobIds.insert(j.id);
	}

	// Prune stale progress entries (deleted jobs) but keep the running job's value
	// so it survives the reload and the card doesn't flash back to 0 % mid-run.
	for (auto it = m_progress.begin(); it != m_progress.end(); ) {
		it = liveJobIds.contains(it.key()) ? ++it : m_progress.erase(it);
	}

	for (const JobDisplayRecord& djr : displayJobs) {
		JobCardEntry e;
		e.job = djr;
		const QString& origJson = origStreamsMap.value(djr.jobId);
		if (djr.status == "done" && !origJson.isEmpty()) {
			// Use the pre-remux stream snapshot so removed tracks (now gone from DB)
			// still show with strikethrough and indices match the original command args.
			e.allStreams = streamsFromJson(origJson);
		} else {
			e.allStreams = db.streamsForFile(djr.fileId);
		}
		e.keptStreams = computeKeptStreams(e.allStreams, argsMap.value(djr.jobId));
		m_allEntries.append(e);
		m_allCheckStates.append(djr.status == "proposed" ? Qt::Checked : Qt::Unchecked);
	}

	applyFilter();
}

void McJobListModel::reloadPaged(int limit)
{
	// "Running" filter must also show queued jobs — delegate to full reload so both
	// statuses enter m_allEntries (the paged DB query would otherwise drop 'queued').
	if (m_filterStatus == QLatin1String("running")) {
		reload();
		return;
	}

	m_allEntries.clear();
	m_allCheckStates.clear();

	auto& db = DatabaseManager::instance();
	m_posterPaths = db.allDonePosterPaths();
	const auto displayJobs = db.allJobsForPanelPaged(limit, m_filterStatus, m_sortMode);

	// allJobs() is fast (just metadata, no streams) — needed for commandArgs/origStreams
	const auto allJobRecords = db.allJobs();
	QHash<qint64, QString> argsMap;
	QHash<qint64, QString> origStreamsMap;
	QSet<qint64> liveJobIds;
	for (const JobRecord& j : allJobRecords) {
		argsMap.insert(j.id, j.commandArgsJson);
		origStreamsMap.insert(j.id, j.originalStreamsJson);
		liveJobIds.insert(j.id);
	}

	for (auto it = m_progress.begin(); it != m_progress.end(); )
		it = liveJobIds.contains(it.key()) ? ++it : m_progress.erase(it);

	// Single batch query instead of N individual streamsForFile() calls
	QList<qint64> fileIds;
	fileIds.reserve(displayJobs.size());
	for (const JobDisplayRecord& djr : displayJobs) fileIds << djr.fileId;
	const auto streamsMap = db.streamsForFiles(fileIds);

	for (const JobDisplayRecord& djr : displayJobs) {
		JobCardEntry e;
		e.job = djr;
		const QString& origJson = origStreamsMap.value(djr.jobId);
		if (djr.status == QLatin1String("done") && !origJson.isEmpty())
			e.allStreams = streamsFromJson(origJson);
		else
			e.allStreams = streamsMap.value(djr.fileId);
		e.keptStreams = computeKeptStreams(e.allStreams, argsMap.value(djr.jobId));
		m_allEntries.append(e);
		m_allCheckStates.append(djr.status == QLatin1String("proposed") ? Qt::Checked : Qt::Unchecked);
	}

	applyFilter();
}

void McJobListModel::setSortMode(JobSortMode sortMode)
{
	m_sortMode = sortMode;
}

void McJobListModel::updateJob(qint64 jobId, const QString& status, qint64 savedBytes)
{
	// Update master list
	for (auto& e : m_allEntries) {
		if (e.job.jobId != jobId) continue;
		e.job.status = status;
		if (savedBytes >= 0) e.job.savedBytes = savedBytes;
		break;
	}
	// Update filtered view — or reapply if status filter makes entry appear/disappear
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.jobId != jobId) continue;
		m_entries[i].job.status = status;
		if (savedBytes >= 0) m_entries[i].job.savedBytes = savedBytes;
		if (!statusMatchesFilter(status)) {
			applyFilter();   // entry no longer passes status filter
		} else {
			const QModelIndex idx = index(i);
			emit dataChanged(idx, idx, { StatusRole, SavedRole });
		}
		return;
	}
	// Not in filtered view — check if the status change makes it visible now
	if (statusMatchesFilter(status))
		applyFilter();
}

void McJobListModel::setFilterText(const QString& text)
{
	if (m_filterText == text) return;
	m_filterText = text;
	applyFilter();
}

void McJobListModel::setFilterStatus(const QString& status)
{
	if (m_filterStatus == status) return;
	m_filterStatus = status;
	applyFilter();
}

void McJobListModel::setQuickFilters(quint32 flags)
{
	if (m_quickFilters == flags) return;
	m_quickFilters = flags;
	applyFilter();
}

void McJobListModel::setRatingFilter(double minRating, double maxRating)
{
	if (qFuzzyCompare(m_ratingMin, minRating) && qFuzzyCompare(m_ratingMax, maxRating)) return;
	m_ratingMin = minRating;
	m_ratingMax = maxRating;
	applyFilter();
}

void McJobListModel::setRatingForFile(qint64 fileId, double rating)
{
	for (auto& e : m_allEntries)
		if (e.job.fileId == fileId) { e.job.voteAverage = rating; break; }

	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.fileId != fileId) continue;
		m_entries[i].job.voteAverage = rating;
		emit dataChanged(index(i), index(i), { RatingRole });
		break;
	}
}

QHash<QString, int> McJobListModel::countsByStatus() const
{
	QHash<QString, int> counts;
	for (const auto& e : m_allEntries)
		++counts[e.job.status];
	return counts;
}

void McJobListModel::onPosterReady(qint64 fileId, const QString& imagePath)
{
	m_posterPaths.insert(fileId, imagePath);
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.fileId == fileId) {
			const QModelIndex idx = index(i);
			emit dataChanged(idx, idx);
		}
	}
}

void McJobListModel::updateImdbId(qint64 fileId, const QString& imdbId)
{
	for (int i = 0; i < m_allEntries.size(); ++i) {
		if (m_allEntries[i].job.fileId == fileId) {
			m_allEntries[i].job.imdbId = imdbId;
			break;
		}
	}
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.fileId == fileId) {
			m_entries[i].job.imdbId = imdbId;
			const QModelIndex idx = index(i);
			emit dataChanged(idx, idx, { ImdbIdRole });
			return;
		}
	}
}

void McJobListModel::updateProgress(qint64 jobId, int percent)
{
	m_progress.insert(jobId, percent);
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.jobId == jobId) {
			const QModelIndex idx = index(i);
			emit dataChanged(idx, idx, { ProgressRole });
			return;
		}
	}
}

bool McJobListModel::statusMatchesFilter(const QString& status) const
{
	if (m_filterStatus.isEmpty()) return true;
	// "Running" filter intentionally includes queued jobs so the user can see
	// both what is processing now and what is lined up next.
	if (m_filterStatus == QLatin1String("running"))
		return status == QLatin1String("running") || status == QLatin1String("queued");
	return status == m_filterStatus;
}

void McJobListModel::applyFilter()
{
	using QF = McFilterPanel;

	beginResetModel();
	m_entries.clear();
	m_checkStates.clear();

	for (int i = 0; i < m_allEntries.size(); ++i) {
		const JobCardEntry& e = m_allEntries[i];

		// ── Text search ──────────────────────────────────────────────────────────
		if (!m_filterText.isEmpty()) {
			const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
			bool hit = e.job.filename.contains(m_filterText, cs)
			        || e.job.summary.contains(m_filterText, cs);
			if (!hit) {
				const QString folder = QFileInfo(e.job.filePath).dir().dirName();
				hit = folder.contains(m_filterText, cs);
			}
			if (!hit) {
				for (const StreamRecord& s : e.allStreams) {
					if (s.language.contains(m_filterText, cs)
					 || s.title.contains(m_filterText, cs)
					 || s.codecName.contains(m_filterText, cs)
					 || s.codecProfile.contains(m_filterText, cs)
					 || s.hdrFormat.contains(m_filterText, cs)) {
						hit = true;
						break;
					}
				}
			}
			if (!hit) continue;
		}

		// ── Status filter ────────────────────────────────────────────────────────
		if (!statusMatchesFilter(e.job.status))
			continue;

		// ── Quick filters ────────────────────────────────────────────────────────
		if (m_quickFilters != 0) {
			if (m_quickFilters & QF::QF_4K) {
				bool ok = false;
				for (const StreamRecord& s : e.allStreams)
					if (s.codecType == QLatin1String("video") && (s.width >= 3840 || s.height >= 2160))
						{ ok = true; break; }
				if (!ok) continue;
			}
			if (m_quickFilters & QF::QF_DV) {
				bool ok = false;
				for (const StreamRecord& s : e.allStreams) {
					if (s.codecType != QLatin1String("video")) continue;
					if (s.hdrFormat == QLatin1String("DolbyVision")
					 || s.codecProfile.startsWith(QLatin1String("dvhe"), Qt::CaseInsensitive)
					 || s.codecProfile.startsWith(QLatin1String("dvav"), Qt::CaseInsensitive))
						{ ok = true; break; }
				}
				if (!ok) continue;
			}
			if (m_quickFilters & QF::QF_HDR) {
				bool ok = false;
				for (const StreamRecord& s : e.allStreams) {
					if (s.codecType != QLatin1String("video")) continue;
					if (!s.hdrFormat.isEmpty() && s.hdrFormat != QLatin1String("DolbyVision"))
						{ ok = true; break; }
				}
				if (!ok) continue;
			}
			if (m_quickFilters & (QF::QF_Atmos | QF::QF_TrueHD | QF::QF_DtsHD | QF::QF_DtsX)) {
				bool ok = false;
				for (const StreamRecord& s : e.allStreams) {
					if (s.codecType != QLatin1String("audio")) continue;
					const QString cn = s.codecName.toLower();
					const QString cp = s.codecProfile.toLower();
					const QString ct = s.title.toLower();
					if ((m_quickFilters & QF::QF_Atmos) &&
					    ((cn == QLatin1String("truehd") || cn == QLatin1String("eac3")) &&
					     (cp.contains(QLatin1String("atmos")) || ct.contains(QLatin1String("atmos")))))
						{ ok = true; break; }
					if ((m_quickFilters & QF::QF_TrueHD) && cn == QLatin1String("truehd"))
						{ ok = true; break; }
					if ((m_quickFilters & QF::QF_DtsHD) && cn == QLatin1String("dts") &&
					    (cp.contains(QLatin1String("ma")) || ct.contains(QLatin1String("dts-hd")) || ct.contains(QLatin1String("dts:x"))))
						{ ok = true; break; }
					if ((m_quickFilters & QF::QF_DtsX) && cn == QLatin1String("dts") &&
					    (cp.contains(QLatin1String("dts:x")) || cp.contains(QLatin1String("dts-x")) ||
					     ct.contains(QLatin1String("dts:x")) || ct.contains(QLatin1String("dts-x"))))
						{ ok = true; break; }
				}
				if (!ok) continue;
			}
		}

		// ── Rating filter ────────────────────────────────────────────────────────
		if (m_ratingMin > 0.0 || m_ratingMax < 10.0) {
			const double r = e.job.voteAverage;
			if (r <= 0.0 || r < m_ratingMin || r > m_ratingMax) continue;
		}

		m_entries.append(e);
		m_checkStates.append(m_allCheckStates[i]);
	}
	endResetModel();
}

QList<qint64> McJobListModel::checkedJobIds() const
{
	QList<qint64> ids;
	for (int i = 0; i < m_entries.size(); ++i)
		if (m_checkStates[i] == Qt::Checked)
			ids << m_entries[i].job.jobId;
	return ids;
}

QList<qint64> McJobListModel::jobIdsByStatus(const QString& status) const
{
	QList<qint64> ids;
	for (const auto& e : m_entries)
		if (e.job.status == status)
			ids << e.job.jobId;
	return ids;
}

// ── QAbstractListModel interface ──────────────────────────────────────────────

int McJobListModel::rowCount(const QModelIndex& parent) const
{
	if (parent.isValid()) return 0;
	return m_entries.size();
}

QVariant McJobListModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || index.row() >= m_entries.size())
		return {};

	const JobCardEntry& e = m_entries.at(index.row());

	switch (role) {
	case Qt::DisplayRole:   return e.job.filename;
	case Qt::CheckStateRole: return m_checkStates.at(index.row());
	case JobIdRole:         return e.job.jobId;
	case FileIdRole:        return e.job.fileId;
	case StatusRole:        return e.job.status;
	case SavedRole:         return e.job.savedBytes;
	case AllStreamsRole:     return QVariant::fromValue(e.allStreams);
	case KeptStreamsRole:    return QVariant::fromValue(e.keptStreams);
	case FilenameRole:      return e.job.filename;
	case SummaryRole:       return e.job.summary;
	case ProgressRole:      return m_progress.value(e.job.jobId, 0);
	case PosterRole:        return m_posterPaths.value(e.job.fileId);
	case FileSizeRole:      return e.job.sizeBytes;
	case FilePathRole:      return e.job.filePath;
	case ImdbIdRole:        return e.job.imdbId;
	case DurationRole:      return e.job.durationSec;
	case RatingRole:           return e.job.voteAverage;
	case OriginalLanguageRole: return e.job.originalLanguage;
	default:                   return {};
	}
}

bool McJobListModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
	if (!index.isValid() || index.row() >= m_entries.size())
		return false;

	if (role == Qt::CheckStateRole) {
		m_checkStates[index.row()] = static_cast<Qt::CheckState>(value.toInt());
		emit dataChanged(index, index, { Qt::CheckStateRole });
		return true;
	}
	return false;
}

Qt::ItemFlags McJobListModel::flags(const QModelIndex& index) const
{
	if (!index.isValid()) return Qt::NoItemFlags;
	Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
	// Only proposed jobs are user-checkable
	if (m_entries.at(index.row()).job.status == "proposed")
		f |= Qt::ItemIsUserCheckable;
	return f;
}

// ── Stream toggle ─────────────────────────────────────────────────────────────

void McJobListModel::toggleStream(const QModelIndex& index, int streamIndex)
{
	if (!index.isValid() || index.row() >= m_entries.size()) return;

	JobCardEntry& filt = m_entries[index.row()];
	if (filt.job.status != "proposed") return;

	const qint64 jobId = filt.job.jobId;

	// Toggle: remove from keptStreams if present, otherwise add it back
	bool wasKept = false;
	for (int i = 0; i < filt.keptStreams.size(); ++i) {
		if (filt.keptStreams[i].streamIndex == streamIndex) {
			filt.keptStreams.removeAt(i);
			wasKept = true;
			break;
		}
	}
	if (!wasKept) {
		for (const auto& s : filt.allStreams) {
			if (s.streamIndex == streamIndex) {
				filt.keptStreams.append(s);
				break;
			}
		}
	}

	// Sync the master list so filter re-apply preserves the change
	for (auto& ae : m_allEntries) {
		if (ae.job.jobId == jobId) {
			ae.keptStreams = filt.keptStreams;
			break;
		}
	}

	// Regenerate commandArgsJson and persist
	const auto allJobs = DatabaseManager::instance().allJobs();
	QString existingArgs;
	for (const auto& j : allJobs)
		if (j.id == jobId) { existingArgs = j.commandArgsJson; break; }

	const QString newArgs = rebuildCommandArgs(existingArgs, filt.allStreams, filt.keptStreams);
	DatabaseManager::instance().updateJobCommandArgs(jobId, newArgs);

	emit dataChanged(index, index, { KeptStreamsRole });
}

QString McJobListModel::rebuildCommandArgs(const QString& existingJson,
                                            const QList<StreamRecord>& all,
                                            const QList<StreamRecord>& kept)
{
	const QJsonArray arr = QJsonDocument::fromJson(existingJson.toUtf8()).array();
	if (arr.size() < 3) return existingJson;

	// Structure from ActionEngine::buildCommand:
	//   ["-o", outputPath, <track-selection flags...>, inputPath]
	const QString outputPath = arr[1].toString();
	const QString inputPath  = arr.last().toString();

	bool hasAudio = false, hasSub = false;
	for (const auto& s : all) {
		if (s.codecType == "audio")    hasAudio = true;
		if (s.codecType == "subtitle") hasSub   = true;
	}

	QStringList keepAudio, keepSub;
	for (const auto& s : kept) {
		if (s.codecType == "audio")    keepAudio << QString::number(s.streamIndex);
		if (s.codecType == "subtitle") keepSub   << QString::number(s.streamIndex);
	}

	// Preserve video flags unchanged — toggleStream only applies to audio/subtitle.
	QStringList newArgs = { "-o", outputPath };
	for (int i = 0; i < arr.size(); ++i) {
		const QString a = arr[i].toString();
		if (a == QStringLiteral("--video-tracks") && i + 1 < arr.size()) {
			newArgs << a << arr[i + 1].toString();
			++i;
		} else if (a == QStringLiteral("--no-video")) {
			newArgs << a;
		}
	}
	if (hasAudio) {
		if (!keepAudio.isEmpty())
			newArgs << "--audio-tracks" << keepAudio.join(',');
		else
			newArgs << "--no-audio";
	}
	if (hasSub) {
		if (!keepSub.isEmpty())
			newArgs << "--subtitle-tracks" << keepSub.join(',');
		else
			newArgs << "--no-subtitles";
	}
	newArgs << inputPath;

	QJsonArray result;
	for (const QString& s : newArgs) result << s;
	return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

// ── Static helper ─────────────────────────────────────────────────────────────

QList<StreamRecord> McJobListModel::streamsFromJson(const QString& json)
{
	QList<StreamRecord> result;
	const QJsonArray arr = QJsonDocument::fromJson(json.toUtf8()).array();
	for (const auto& v : arr) {
		const QJsonObject o = v.toObject();
		StreamRecord s;
		s.streamIndex       = o["idx"].toInt();
		s.codecType         = o["type"].toString();
		s.codecName         = o["codec"].toString();
		s.codecProfile      = o["profile"].toString();
		s.language          = o["lang"].toString();
		s.title             = o["title"].toString();
		s.channels          = o["ch"].toInt();
		s.width             = o["w"].toInt();
		s.height            = o["h"].toInt();
		s.hdrFormat         = o["hdr"].toString();
		s.isDefault         = o["default"].toBool();
		s.isForced          = o["forced"].toBool();
		s.isHearingImpaired = o["hi"].toBool();
		result << s;
	}
	return result;
}

QList<StreamRecord> McJobListModel::computeKeptStreams(
	const QList<StreamRecord>& all,
	const QString& commandArgsJson)
{
	if (commandArgsJson.isEmpty())
		return all;

	// mkvmerge args contain --audio-tracks N,M and --subtitle-tracks N,M
	// where N,M are stream indices to *include*.  Video tracks are always kept.
	// If neither flag is present for a type, all tracks of that type are kept.
	const QJsonArray arr = QJsonDocument::fromJson(commandArgsJson.toUtf8()).array();
	const QStringList args = [&]{
		QStringList s;
		for (const auto& v : arr) s << v.toString();
		return s;
	}();

	// Parse include-sets per type
	auto parseSet = [&](const QString& flag) -> std::optional<QSet<int>> {
		for (int i = 0; i < args.size() - 1; ++i) {
			if (args[i] == flag) {
				QSet<int> set;
				for (const QString& tok : args[i + 1].split(',', Qt::SkipEmptyParts))
					set.insert(tok.trimmed().toInt());
				return set;
			}
		}
		return std::nullopt;
	};

	const auto audioInclude    = parseSet("--audio-tracks");
	const auto subtitleInclude = parseSet("--subtitle-tracks");
	const auto videoInclude    = parseSet("--video-tracks");
	const bool noAudio     = args.contains(QStringLiteral("--no-audio"));
	const bool noSubtitles = args.contains(QStringLiteral("--no-subtitles"));
	const bool noVideo     = args.contains(QStringLiteral("--no-video"));

	QList<StreamRecord> kept;
	for (const StreamRecord& s : all) {
		if (s.codecType == "video") {
			if (noVideo) continue;
			// When --video-tracks is absent, keep all video (backward-compatible default).
			if (videoInclude && !videoInclude->contains(s.streamIndex)) continue;
			kept << s;
		} else if (s.codecType == "audio") {
			if (noAudio) continue;
			if (!audioInclude || audioInclude->contains(s.streamIndex))
				kept << s;
		} else if (s.codecType == "subtitle") {
			if (noSubtitles) continue;
			if (!subtitleInclude || subtitleInclude->contains(s.streamIndex))
				kept << s;
		} else {
			kept << s; // data/attachment tracks always kept
		}
	}
	return kept;
}

} // namespace Mc
