#pragma once
#include "core/DatabaseManager.h"
#include <QObject>
#include <QString>
#include <atomic>

namespace Mc {

// One-time background pass re-running ffprobe on files scanned before Dolby
// Vision profile/compatibility detection existed (FfprobeScanner's dv_profile/
// dv_bl_compat_id/dv_el_present parsing). The normal scan's skip-if-unchanged
// fast path (ScanWorker::processCandidate) never re-runs ffprobe on a file whose
// mtime/size haven't changed, so an ordinary full or quick scan alone never
// backfills this for already-scanned files. Driven entirely by the per-file
// dv_checked DB flag, same resumable pattern as EditionBackfillWorker — it just
// re-queries DatabaseManager::filesNeedingDvCheck() and keeps going where it
// left off.
class DvProfileBackfillWorker : public QObject
{
	Q_OBJECT
public:
	explicit DvProfileBackfillWorker(QString ffprobePath, QObject* parent = nullptr);

	// Shared with the normal scan's sidecar-subtitle detection — see
	// UserProfile::detectSidecarSubtitleLanguage(). Needed because insertStreams()
	// replaces every stream row for a file, so sidecars must be re-detected
	// alongside ffprobe's container streams, not just left out.
	void setDetectSidecarSubtitleLanguage(bool v) { m_detectSidecarSubtitleLanguage = v; }
	void cancel() { m_cancelled.store(true, std::memory_order_relaxed); }

public slots:
	void run();

signals:
	// Emitted only for files that turned out to be Dolby Vision, so callers can
	// refresh just that card instead of the whole library.
	void fileDolbyVisionFound(qint64 fileId);
	// current/total let the UI show real progress — unlike EditionBackfillWorker
	// (pure filename regex, finishes near-instantly), each file here is a real
	// ffprobe call that can take seconds over a NAS, so this can run for a long
	// time with nothing to show for it otherwise.
	void progress(int current, int total);
	void finished(int processed);

private:
	QString           m_ffprobePath;
	bool              m_detectSidecarSubtitleLanguage = false;
	std::atomic<bool> m_cancelled{false};
};

} // namespace Mc
