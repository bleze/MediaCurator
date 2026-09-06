#include "scanner/DvProfileBackfillWorker.h"
#include "scanner/FfprobeScanner.h"
#include "scanner/ScanWorker.h"

namespace Mc {

DvProfileBackfillWorker::DvProfileBackfillWorker(QString ffprobePath, QObject* parent)
	: QObject(parent), m_ffprobePath(std::move(ffprobePath))
{}

void DvProfileBackfillWorker::run()
{
	auto& db = DatabaseManager::instance();
	const auto pending = db.filesNeedingDvCheck();

	const FfprobeScanner scanner(m_ffprobePath);
	int processed = 0;
	const int total = pending.size();

	for (const FileRecord& f : pending) {
		// Checked per file (cheap relative to the ffprobe call itself) rather than
		// only between batches, so a cancel requested mid-run doesn't have to wait
		// for a whole batch to finish first.
		if (m_cancelled.load(std::memory_order_relaxed)) break;

		const auto result = scanner.scanFile(f.path);
		if (result.success) {
			// Combine with sidecar subtitles same as a normal scan (ScanWorker) —
			// insertStreams() replaces every stream row for this file, and ffprobe
			// alone never sees external sidecar files.
			const auto sidecars = ScanWorker::scanSidecarSubtitles(
				f.path, ScanWorker::nextSidecarStreamIndex(result.streams),
				m_detectSidecarSubtitleLanguage);
			auto allStreams = result.streams;
			allStreams.append(sidecars);
			db.insertStreams(f.id, allStreams);
			db.markDvChecked(f.id);

			for (const StreamRecord& s : result.streams) {
				if (s.hdrFormat == QLatin1String("DolbyVision")) {
					emit fileDolbyVisionFound(f.id);
					break;
				}
			}
		}
		// On failure (e.g. a NAS-hosted file temporarily unreachable), leave
		// dv_checked=0 so this file is retried on a later launch instead of being
		// silently skipped forever.
		++processed;
		emit progress(processed, total);
	}

	emit finished(processed);
}

} // namespace Mc
