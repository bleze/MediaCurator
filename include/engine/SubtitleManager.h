#pragma once
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QThread;

namespace Mc {

class SubtitleWorker;

/**
 * SubtitleManager — downloads missing OpenSubtitles subtitles for the library on a
 * background thread, mirroring PosterManager's always-on queue.
 *
 * Requires an OpenSubtitles API key and is off unless auto-download is explicitly
 * enabled (unlike posters, OpenSubtitles has a small daily download quota, so this
 * is opt-in rather than "on whenever credentials are configured").
 *
 * No local cache table is kept — coverage is derived live from the streams table on
 * each enqueue, so a file naturally stops being queued once it has subtitles for
 * every understood language.
 */
class SubtitleManager : public QObject
{
	Q_OBJECT
public:
	static SubtitleManager& instance();

	// Call once at startup (after profile is loaded).
	void start(const QString& apiKey, const QString& username, const QString& password,
	           bool enabled, const QStringList& understoodLanguages);

	// Stop the background thread gracefully. Call from closeEvent before accept().
	void stop();

	// Push updated settings (e.g. when the user saves settings).
	void setCredentials(const QString& apiKey, const QString& username, const QString& password);
	void setEnabled(bool enabled);
	void setUnderstoodLanguages(const QStringList& languages);
	// Gates SubtitleLanguageDetector + on-disk renaming when re-scanning sidecars
	// after a download (UserProfile::detectSidecarSubtitleLanguage()). Off by default.
	void setDetectSubtitleLanguage(bool detect);

	// Enqueue a newly-scanned (or rescanned) file for subtitle lookup.
	// No-op if disabled, no API key configured, or the file is already fully covered.
	void enqueue(qint64 fileId);

	// Batch enqueue — one cross-thread signal for many IDs (e.g. quick-scan backfill).
	void enqueueBatch(const QList<qint64>& fileIds);

	// User-initiated cancel — drop every queued file and abort whatever is currently
	// downloading. Unlike stop(), the manager stays running and will pick up newly
	// enqueued files afterward (mirrors ScanWorker::cancel()/JobQueue::cancel()).
	void cancelAll();

	// Called when a caller outside the background queue (e.g. the manual
	// McSubtitleDownloadDialog) observes an HTTP 429, so the shared quota guard
	// pauses the background queue too instead of only pausing its own dialog.
	void reportQuotaExceeded();

signals:
	// A background download wrote new subtitle file(s) for fileId; streams table
	// has already been updated — listeners only need to refresh their view.
	void subtitlesReady(qint64 fileId, int downloadedCount);
	// OpenSubtitles reported zero downloads remaining for the day (or a request came
	// back HTTP 429); the queue is paused until the app restarts or credentials change.
	void quotaExhausted();
	// The queue transitioned between idle and actively processing files — drives the
	// visibility of a "Cancel Subtitle Downloads" UI affordance.
	void queueActiveChanged(bool active);

	// Internal — cross-thread commands to the worker.
	void workerSetCredentials(QString apiKey, QString username, QString password);
	void workerSetEnabled(bool enabled);
	void workerSetUnderstoodLanguages(QStringList languages);
	void workerSetDetectSubtitleLanguage(bool detect);
	void workerEnqueueFile(qint64 fileId);
	void workerEnqueueBatch(QList<qint64> fileIds);
	void workerCancelAll();
	void workerReportQuotaExceeded();
	void workerStop();

private:
	explicit SubtitleManager(QObject* parent = nullptr);

	QThread*        m_thread = nullptr;
	SubtitleWorker* m_worker = nullptr;
};

} // namespace Mc
