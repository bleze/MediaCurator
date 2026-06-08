#include "scanner/ScanWorker.h"
#include "core/DatabaseManager.h"
#include "engine/PosterManager.h"
#include "scanner/NfoParser.h"

#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QDebug>
#include <QSet>

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

ScanWorker::ScanWorker(const QString& ffprobePath, QObject* parent)
	: QObject(parent)
	, m_ffprobePath(ffprobePath)
{}

void ScanWorker::run()
{
	if (m_rootPath.isEmpty()) {
		emit error("No scan path set");
		emit finished(0, 0, 0, 0, 0, 0);
		return;
	}

	auto& db = DatabaseManager::instance();
	const qint64 scanRunId = db.beginScanRun(m_rootPath);

	FfprobeScanner scanner(m_ffprobePath);
	const QStringList& exts = videoExtensions();

	int added = 0, updated = 0, failed = 0, skipped = 0, index = 0;
	QSet<QString> foundPaths;

	QElapsedTimer progressTimer;
	progressTimer.start();

	// Discover and scan in a single pass — no pre-collection phase.
	// Progress is throttled to at most one signal per 100 ms so the main thread
	// isn't flooded with cross-thread events on large libraries.
	// FollowSymlinks is intentionally omitted to avoid infinite loops on cyclic junctions.
	QDirIterator it(m_rootPath, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		const QString path = it.next();
		if (m_cancelled.loadRelaxed()) break;

		const QFileInfo fi(path);
		if (!fi.isFile()) continue;
		if (!exts.contains(fi.suffix().toLower())) continue;

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
				continue;
			}
		}

		const bool existed = existing.has_value();

		const auto result = scanner.scanFile(path, scanRunId);
		if (!result.success) {
			qWarning() << "ScanWorker: scan failed for" << path << "—" << result.errorMessage;
			++failed;
			continue;
		}

		const auto fileId = db.upsertFile(result.file);
		if (!fileId) {
			qWarning() << "ScanWorker: DB upsert failed for" << path;
			++failed;
			continue;
		}

		db.insertStreams(*fileId, result.streams);

		if (existed) ++updated;
		else         ++added;

		// Store the IMDb ID from the .nfo sidecar immediately — visible in the UI
		// without waiting for the poster worker to process the file.
		const QString imdbId = NfoParser::readImdbId(path);
		if (!imdbId.isEmpty()) {
			db.updateImdbId(*fileId, imdbId);
			emit imdbIdFound(*fileId, imdbId);
		}

		// Emit the data we already have in memory — no DB round-trip needed on the UI side.
		FileRecord fileWithId = result.file;
		fileWithId.id = *fileId;
		emit fileProcessed(fileWithId, result.streams);

		// Kick off poster lookup in parallel with the next FFprobe call.
		PosterManager::instance().enqueue(*fileId);
	}

	// Prune DB entries for files that no longer exist under this root.
	// Only run on a complete (non-cancelled) scan.
	int removed = 0;
	if (!m_cancelled.loadRelaxed()) {
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
	emit finished(index, added, updated, failed, skipped, removed);
}

} // namespace Mc
