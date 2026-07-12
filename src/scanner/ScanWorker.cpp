#include "scanner/ScanWorker.h"
#include "core/DatabaseManager.h"
#include "scanner/NfoParser.h"
#include "scanner/SubtitleLanguageDetector.h"
#include "engine/ActionEngine.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QRegularExpression>
#include <QSet>

#include <functional>

#ifdef Q_OS_WIN
#include <windows.h>
static FILETIME toFileTime(const QDateTime& dt)
{
	const qint64 ns100 = (dt.toMSecsSinceEpoch() + Q_INT64_C(11644473600000)) * 10000;
	FILETIME ft;
	ft.dwLowDateTime  = static_cast<DWORD>(ns100 & 0xFFFFFFFF);
	ft.dwHighDateTime = static_cast<DWORD>((ns100 >> 32) & 0xFFFFFFFF);
	return ft;
}
// Restore a renamed sidecar subtitle's own creation/modified timestamps. Mirrors
// preserveTimestamps() in RemuxJob.cpp/JobQueue.cpp.
static void preserveTimestamps(const QString& target, const QDateTime& origCreated, const QDateTime& origModified)
{
	HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(target.utf16()),
	                       FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	FILETIME ftCreated  = origCreated.isValid()  ? toFileTime(origCreated)  : FILETIME{};
	FILETIME ftModified = origModified.isValid() ? toFileTime(origModified) : FILETIME{};
	SetFileTime(h,
	            origCreated.isValid()  ? &ftCreated  : nullptr,
	            nullptr,
	            origModified.isValid() ? &ftModified : nullptr);
	CloseHandle(h);
}
// Restore the folder's own timestamps after a sidecar rename touches its directory
// entry, so a language-detection rename doesn't bubble the movie's folder up as
// "newest" in media libraries that sort by Date Modified. Mirrors preserveDirTimestamps()
// in OpenSubtitlesClient.cpp, needs FILE_FLAG_BACKUP_SEMANTICS to open a directory handle.
static void preserveDirTimestamps(const QString& dirPath, const QDateTime& origCreated, const QDateTime& origModified)
{
	HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(dirPath.utf16()),
	                       FILE_WRITE_ATTRIBUTES,
	                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                       nullptr, OPEN_EXISTING,
	                       FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	FILETIME ftCreated  = origCreated.isValid()  ? toFileTime(origCreated)  : FILETIME{};
	FILETIME ftModified = origModified.isValid() ? toFileTime(origModified) : FILETIME{};
	SetFileTime(h,
	            origCreated.isValid()  ? &ftCreated  : nullptr,
	            nullptr,
	            origModified.isValid() ? &ftModified : nullptr);
	CloseHandle(h);
}
#endif

namespace Mc {

const QStringList& ScanWorker::videoExtensions()
{
	static const QStringList exts = {
		"mkv", "mka", "mks",       // Matroska
		"mp4", "m4v", "m4a",       // MP4
		"avi",                      // AVI
		"wmv", "wma", "asf",       // Windows Media
		"m2ts", "ts", "m2t",       // MPEG transport stream
		"vob",                      // DVD
		"ogm", "ogg", "ogv",       // Ogg
		"flv",                      // Flash
		"mov", "qt",               // QuickTime
		"webm",                     // WebM
		"divx",                     // DivX
		"iso",                      // Blu-ray / DVD disc image
	};
	return exts;
}

// Subtitle file extensions → codec name for StreamRecord
static const QHash<QString, QString>& subtitleExtToCodec()
{
	static const QHash<QString, QString> m = {
		{"srt",  "subrip"},
		{"ass",  "ass"},
		{"ssa",  "ass"},
		{"vtt",  "webvtt"},
		{"sub",  "dvd_subtitle"},
	};
	return m;
}

// Language token → ISO 639-2 code (covers ISO 639-1 two-letter codes and common full names)
static const QHash<QString, QString>& subtitleLangMap()
{
	static const QHash<QString, QString> m = {
		// ISO 639-1
		{"en","eng"},{"da","dan"},{"de","deu"},{"fr","fra"},{"es","spa"},
		{"it","ita"},{"nl","nld"},{"sv","swe"},{"nb","nor"},{"nn","nor"},
		{"fi","fin"},{"pl","pol"},{"pt","por"},{"ru","rus"},{"zh","zho"},
		{"ja","jpn"},{"ko","kor"},{"ar","ara"},{"cs","ces"},{"sk","slk"},
		{"hu","hun"},{"ro","ron"},{"el","ell"},{"tr","tur"},{"he","heb"},
		{"uk","ukr"},{"hr","hrv"},{"sr","srp"},{"bg","bul"},{"lt","lit"},
		{"lv","lav"},{"et","est"},{"sl","slv"},{"is","isl"},{"ca","cat"},
		{"hi","hin"},{"th","tha"},{"vi","vie"},{"id","ind"},{"ms","msa"},
		// Full names
		{"english","eng"},{"danish","dan"},{"german","deu"},{"french","fra"},
		{"spanish","spa"},{"italian","ita"},{"dutch","nld"},{"swedish","swe"},
		{"norwegian","nor"},{"finnish","fin"},{"polish","pol"},{"portuguese","por"},
		{"russian","rus"},{"chinese","zho"},{"japanese","jpn"},{"korean","kor"},
		{"arabic","ara"},{"czech","ces"},{"slovak","slk"},{"hungarian","hun"},
		{"romanian","ron"},{"greek","ell"},{"turkish","tur"},{"hebrew","heb"},
		{"ukrainian","ukr"},{"croatian","hrv"},{"serbian","srp"},{"bulgarian","bul"},
	};
	return m;
}

// Reads up to this many raw bytes of a sidecar subtitle file to sample for
// language detection — enough dialogue for CLD2 to be confident without
// reading pathologically large files in full.
static constexpr qint64 kSubtitleSampleReadCap = 200 * 1024;

// Strips subtitle-format markup/timing so only dialogue prose remains, for
// feeding to SubtitleLanguageDetector. Returns empty for formats with no
// text to extract (image-based .sub/VobSub, per subtitleExtToCodec() above)
// or files that fail to open.
//
// Read as UTF-8 unconditionally, matching NfoParser's convention elsewhere —
// older rips in other codepages (e.g. Windows-1252) will feed CLD2 a partially
// garbled sample, but the detector's own confidence threshold is what catches
// that: a corrupted sample just fails to reach kMinConfidencePercent and gets
// skipped rather than misdetected.
static QString extractSubtitleSampleText(const QFileInfo& fi)
{
	const QString ext = fi.suffix().toLower();
	if (ext != QLatin1String("srt") && ext != QLatin1String("ass") &&
	    ext != QLatin1String("ssa") && ext != QLatin1String("vtt"))
		return {};

	QFile f(fi.absoluteFilePath());
	if (!f.open(QIODevice::ReadOnly))
		return {};
	const QString content = QString::fromUtf8(f.read(kSubtitleSampleReadCap));

	static const QRegularExpression tagRe(QStringLiteral("<[^>]*>|\\{[^}]*\\}"));
	static const QRegularExpression digitsOnlyRe(QStringLiteral("^\\d+$"));
	const bool isAss = (ext == QLatin1String("ass") || ext == QLatin1String("ssa"));

	QString sample;
	sample.reserve(4096);
	for (const QString& rawLine : content.split(QLatin1Char('\n'))) {
		QString line = rawLine.trimmed();
		if (line.isEmpty())
			continue;

		if (isAss) {
			if (!line.startsWith(QLatin1String("Dialogue:")))
				continue;
			// "Dialogue: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text"
			int commas = 0, textStart = -1;
			for (int i = 0; i < line.size(); ++i) {
				if (line[i] == QLatin1Char(',') && ++commas == 9) { textStart = i + 1; break; }
			}
			if (textStart < 0)
				continue;
			line = line.mid(textStart);
			line.replace(QLatin1String("\\N"), QLatin1String(" "));
			line.replace(QLatin1String("\\n"), QLatin1String(" "));
		} else {
			if (line.contains(QLatin1String("-->")) || line == QLatin1String("WEBVTT") ||
			    digitsOnlyRe.match(line).hasMatch())
				continue;
		}

		line.remove(tagRe);
		if (line.isEmpty())
			continue;

		sample += line;
		sample += QLatin1Char(' ');
		if (sample.size() >= 4000)
			break; // plenty for a confident read; avoid scanning huge files fully
	}
	return sample;
}

int ScanWorker::nextSidecarStreamIndex(const QList<StreamRecord>& containerStreams)
{
	int maxIdx = -1;
	for (const StreamRecord& s : containerStreams) {
		if (!s.isExternal && s.streamIndex > maxIdx)
			maxIdx = s.streamIndex;
	}
	return maxIdx + 1;
}

static bool externalSidecarsEqual(const QList<StreamRecord>& a, const QList<StreamRecord>& b)
{
	auto fingerprint = [](const QList<StreamRecord>& streams) {
		QStringList parts;
		for (const StreamRecord& s : streams) {
			if (!s.isExternal) continue;
			parts << QStringLiteral("%1|%2|%3")
			             .arg(s.externalPath, s.language)
			             .arg(s.isForced ? 1 : 0);
		}
		parts.sort();
		return parts;
	};
	return fingerprint(a) == fingerprint(b);
}

bool ScanWorker::refreshSidecarStreamsIfChanged(DatabaseManager& db, qint64 fileId,
                                                const QString& videoPath, bool detectLanguage)
{
	const auto existing = db.streamsForFile(fileId);
	QList<StreamRecord> container;
	QList<StreamRecord> oldExternal;
	container.reserve(existing.size());
	for (const StreamRecord& s : existing) {
		if (s.isExternal)
			oldExternal << s;
		else
			container << s;
	}

	const auto newSidecars = scanSidecarSubtitles(videoPath, nextSidecarStreamIndex(container), detectLanguage);
	if (externalSidecarsEqual(oldExternal, newSidecars))
		return false;

	auto allStreams = container;
	allStreams.append(newSidecars);
	return db.insertStreams(fileId, allStreams);
}

QList<StreamRecord> ScanWorker::scanSidecarSubtitles(const QString& videoPath, int startIndex,
                                                      bool detectLanguage)
{
	const QFileInfo videoFi(videoPath);
	const QString dir      = videoFi.absolutePath();
	const QString baseName = videoFi.completeBaseName();  // filename without last extension

	const QStringList nameFilters = {"*.srt","*.ass","*.ssa","*.vtt","*.sub"};
	const QFileInfoList candidates = QDir(dir).entryInfoList(nameFilters, QDir::Files);

	static const QRegularExpression separatorRe(QStringLiteral("[._\\-]"));

	// Captured lazily on the first rename below — a language-detection rename touches
	// the directory entry, which bumps the folder's Date Modified on Windows and makes
	// the movie look "new" in library views that sort by it.
	QDateTime dirOrigCreated, dirOrigModified;
	bool anyRenamed = false;

	QList<StreamRecord> result;
	int idx = startIndex;

	for (const QFileInfo& fi : candidates) {
		const QString subBase = fi.completeBaseName();
		// Must start with the video's complete base name (case-insensitive)
		if (!subBase.startsWith(baseName, Qt::CaseInsensitive)) continue;
		const QString remainder = subBase.mid(baseName.length()); // "" | ".en" | "-en" | ".en.forced" …

		QString lang;
		bool isForced = false;
		bool isSDH    = false;

		if (!remainder.isEmpty()) {
			// Strip leading separator, then split further parts
			QString rest = remainder;
			if (!rest.isEmpty() && (rest[0] == '.' || rest[0] == '-' || rest[0] == '_'))
				rest = rest.mid(1);
			const QStringList parts = rest.split(separatorRe, Qt::SkipEmptyParts);
			for (const QString& part : parts) {
				const QString lower = part.toLower();
				if (lower == QLatin1String("forced"))                         { isForced = true; continue; }
				if (lower == QLatin1String("sdh") || lower == QLatin1String("hi") || lower == QLatin1String("cc")) { isSDH = true; continue; }
				if (lang.isEmpty()) {
					const QString mapped = subtitleLangMap().value(lower);
					if (!mapped.isEmpty())
						lang = mapped;
					else if (lower.length() == 3)
						lang = lower;  // assume it's already ISO 639-2
				}
			}
		}

		QString finalPath = fi.absoluteFilePath();

		// Filename carried no language token — read the dialogue itself and, only on
		// a high-confidence read, rename the file so the language is never re-guessed
		// (subsequent scans will then parse it straight from the filename above).
		if (lang.isEmpty() && detectLanguage) {
			const QString sample = extractSubtitleSampleText(fi);
			if (const auto detected = SubtitleLanguageDetector::detect(sample)) {
				const QString lower  = detected->code.toLower();
				const QString mapped = subtitleLangMap().value(lower);
				const QString code639_2 = !mapped.isEmpty() ? mapped
				                         : (lower.length() == 3 ? lower : QString());
				if (!code639_2.isEmpty()) {
					const QString newPath = ActionEngine::insertLanguageIntoSidecarPath(
						finalPath, baseName, code639_2);
					if (QFile::exists(newPath)) {
						qWarning() << "Detected language for" << fi.fileName()
						           << "but" << QFileInfo(newPath).fileName()
						           << "already exists — leaving unlabeled";
					} else {
#ifdef Q_OS_WIN
						if (!anyRenamed) {
							dirOrigCreated  = QFileInfo(dir).birthTime();
							dirOrigModified = QFileInfo(dir).lastModified();
						}
						const QDateTime fileOrigCreated  = fi.birthTime();
						const QDateTime fileOrigModified = fi.lastModified();
#endif
						if (QFile::rename(finalPath, newPath)) {
							qDebug() << "Detected" << code639_2 << "(" << detected->confidencePercent
							         << "% confidence ) for" << fi.fileName()
							         << "-> renamed to" << QFileInfo(newPath).fileName();
							lang       = code639_2;
							finalPath  = newPath;
							anyRenamed = true;
#ifdef Q_OS_WIN
							preserveTimestamps(newPath, fileOrigCreated, fileOrigModified);
#endif
						}
					}
				}
			}
		}

		StreamRecord s;
		s.streamIndex       = idx++;
		s.codecType         = QStringLiteral("subtitle");
		s.codecName         = subtitleExtToCodec().value(fi.suffix().toLower());
		s.language          = lang;
		s.trackType         = QStringLiteral("main");
		s.isForced          = isForced;
		s.isHearingImpaired = isSDH;
		s.isExternal        = true;
		s.externalPath      = finalPath;
		result.append(s);
	}

#ifdef Q_OS_WIN
	if (anyRenamed)
		preserveDirTimestamps(dir, dirOrigCreated, dirOrigModified);
#endif

	return result;
}

ScanWorker::ScanWorker(const QString& ffprobePath, QObject* parent)
	: QObject(parent)
	, m_ffprobePath(ffprobePath)
{}

void ScanWorker::run()
{
	if (m_rootPath.isEmpty()) {
		emit error("No scan path set");
		emit finished(0, 0, 0, 0, 0, 0, {});
		return;
	}

	auto& db = DatabaseManager::instance();
	const qint64 scanRunId = db.beginScanRun(m_rootPath);

	FfprobeScanner scanner(m_ffprobePath);
	const QStringList& exts = videoExtensions();

	int added = 0, updated = 0, failed = 0, skipped = 0, index = 0;
	QSet<QString> foundPaths;
	QStringList newFiles;

	QElapsedTimer progressTimer;
	progressTimer.start();

	// Progress is throttled to at most one signal per 100 ms so the main thread
	// isn't flooded with cross-thread events on large libraries.

	// Files living directly inside these folder names are skipped — they are bonus
	// content sub-folders, not independent library items.
	static const QStringList kExtrasFolders = {
	    "extras", "featurettes", "behind the scenes", "deleted scenes",
	    "interviews", "scenes", "trailers", "shorts", "specials", "sample"
	};

	// Scans one candidate video file: skips it unchanged if the DB copy still matches
	// mtime/size, otherwise (re-)runs ffprobe and (up)inserts it. Shared by the full
	// walk and the quick-scan walk below.
	auto processCandidate = [&](const QString& path) {
		const QFileInfo fi(path);
		foundPaths.insert(path);
		++index;
		if (progressTimer.elapsed() >= 100) {
			emit progress(index, 0, fi.fileName());
			progressTimer.restart();
		}

		const auto existing = db.fileByPath(path);
		if (existing.has_value()) {
			const qint64 curMtime = fi.lastModified().toMSecsSinceEpoch();
			const qint64 curSize  = fi.size();
			if (existing->mtimeMs == curMtime && existing->sizeBytes == curSize) {
				++skipped;
				if (refreshSidecarStreamsIfChanged(db, existing->id, path, m_detectSubtitleLanguage))
					emit fileProcessed(*existing, db.streamsForFile(existing->id));
				// Enqueue even for unchanged files so PosterManager can backfill
				// any missing display_title or rating without re-running ffprobe.
				emit posterEnqueueRequested(existing->id);
				emit subtitleEnqueueRequested(existing->id);
				return;
			}
		}

		const bool existed = existing.has_value();

		const auto result = scanner.scanFile(path, scanRunId);
		if (!result.success) {
			qWarning() << "ScanWorker: scan failed for" << path << "—" << result.errorMessage;
			++failed;
			return;
		}

		const auto fileId = db.upsertFile(result.file);
		if (!fileId) {
			qWarning() << "ScanWorker: DB upsert failed for" << path;
			++failed;
			return;
		}

		// Combine container streams with any sidecar subtitle files found alongside
		const auto sidecars  = scanSidecarSubtitles(path, nextSidecarStreamIndex(result.streams), m_detectSubtitleLanguage);
		auto allStreams       = result.streams;
		allStreams.append(sidecars);
		if (!db.insertStreams(*fileId, allStreams)) {
			// Rare, but if it happens the file row exists with no streams — the card
			// looks right this session (fileProcessed below carries the in-memory
			// data) yet comes back empty after restart. One immediate retry covers
			// transient contention from concurrent scan/poster/subtitle writers.
			qWarning() << "ScanWorker: insertStreams failed for" << path << "— retrying once";
			if (!db.insertStreams(*fileId, allStreams))
				qCritical() << "ScanWorker: insertStreams failed twice for" << path
				            << "— tracks will be missing until the next rescan";
		}

		if (existed) {
			++updated;
			// File changed on disk — any pending remux job was built against the old
			// stream layout and would cause a track-mismatch error if run now.
			db.deletePendingJobsForFile(*fileId);
		} else {
			++added;
			newFiles << path;
		}

		// Resolve IMDb ID: embedded container tag takes priority, then .nfo sidecar.
		// Store immediately so the poster worker can begin TMDB lookup without delay.
		const QString imdbId = result.file.embeddedImdbId.isEmpty()
		    ? NfoParser::readImdbId(path)
		    : result.file.embeddedImdbId;
		if (!imdbId.isEmpty()) {
			db.updateImdbId(*fileId, imdbId);
			emit imdbIdFound(*fileId, imdbId);
		}

		// Emit the data we already have in memory — no DB round-trip needed on the UI side.
		FileRecord fileWithId = result.file;
		fileWithId.id = *fileId;
		emit fileProcessed(fileWithId, allStreams);

		// Kick off poster lookup in parallel with the next FFprobe call.
		emit posterEnqueueRequested(*fileId);
		emit subtitleEnqueueRequested(*fileId);
	};

	if (m_quickScan) {
		// Skip any folder that already contains at least one known file entirely —
		// don't even list its contents. Only folders with no known files (i.e. newly
		// added movies) get walked, which is what makes this fast: no per-file mtime/
		// size round-trips across the existing library, just directory listings down
		// to the first already-known folder in each branch.
		QSet<QString> knownDirs;
		const auto knownFiles = db.filesUnderPath(m_rootPath);
		for (const auto& f : knownFiles)
			knownDirs.insert(QFileInfo(f.path).absolutePath());

		// Already-known files are never touched by the walk below, so backfill
		// managers here for files that may still need work under this root.
		{
			const QList<qint64> needingPosters = db.fileIdsNeedingPosters();
			const QSet<qint64> needsPoster(needingPosters.begin(), needingPosters.end());
			const QHash<qint64, QString> imdbIds = db.allKnownImdbIds();
			QList<qint64> posterIds, subtitleIds;
			posterIds.reserve(knownFiles.size());
			subtitleIds.reserve(knownFiles.size());
			for (const auto& f : knownFiles) {
				if (needsPoster.contains(f.id))
					posterIds << f.id;
				if (!imdbIds.value(f.id).isEmpty())
					subtitleIds << f.id;
			}
			if (!posterIds.isEmpty())
				emit posterEnqueueBatchRequested(posterIds);
			if (!subtitleIds.isEmpty())
				emit subtitleEnqueueBatchRequested(subtitleIds);
		}

		QSet<QString> visitedDirs;
		const auto dirFilters = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks;
		std::function<void(const QString&)> walkNewFolders = [&](const QString& dir) {
			if (m_cancelled.loadRelaxed()) return;
			const QString normDir = QDir::cleanPath(dir);
			if (visitedDirs.contains(normDir)) return;
			visitedDirs.insert(normDir);
			const QFileInfoList entries = QDir(normDir).entryInfoList(dirFilters);
			for (const QFileInfo& fi : entries) {
				if (m_cancelled.loadRelaxed()) return;
				if (fi.isDir()) {
					if (knownDirs.contains(fi.absoluteFilePath())) continue;
					walkNewFolders(fi.absoluteFilePath());
				} else if (exts.contains(fi.suffix().toLower())
				           && !kExtrasFolders.contains(fi.dir().dirName().toLower())) {
					processCandidate(fi.absoluteFilePath());
				}
			}
		};
		walkNewFolders(m_rootPath);
	} else {
		// Discover and scan in a single pass — no pre-collection phase.
		// FollowSymlinks is intentionally omitted to avoid infinite loops on cyclic junctions.
		QDirIterator it(m_rootPath, QDirIterator::Subdirectories);
		while (it.hasNext()) {
			const QString path = it.next();
			if (m_cancelled.loadRelaxed()) break;

			const QFileInfo fi(path);
			if (!fi.isFile()) continue;
			if (!exts.contains(fi.suffix().toLower())) continue;

			// Skip files that live directly inside a known extras sub-folder.
			if (kExtrasFolders.contains(fi.dir().dirName().toLower())) continue;

			processCandidate(path);
		}
	}

	// Prune DB entries for files that no longer exist under this root.
	// Only run on a complete, non-quick scan: quick scans deliberately skip
	// already-known folders, so anything not in foundPaths there would be wrongly
	// treated as deleted.
	int removed = 0;
	if (!m_cancelled.loadRelaxed() && !m_quickScan) {
		const auto dbFiles = db.filesUnderPath(m_rootPath);
		for (const auto& dbFile : dbFiles) {
			if (!foundPaths.contains(dbFile.path)) {
				db.deleteFile(dbFile.id);
				emit fileRemoved(dbFile.id);
				++removed;
			}
		}
	}

	db.endScanRun(scanRunId, added, updated, removed);
	emit finished(index, added, updated, failed, skipped, removed, newFiles);
}

} // namespace Mc
