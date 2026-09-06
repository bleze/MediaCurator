#pragma once

#include <QAtomicInt>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QThread;
class QUrl;

namespace Mc {

class SceneNfoWorker;

// One srrDB (https://www.srrdb.com) scene-release search hit.
struct SceneRelease {
	QString release;          // e.g. "Movie.Title.2020.1080p.BluRay.x264-GROUP"
	QString imdbId;           // numeric, no "tt" prefix (srrDB's own field format); empty if unknown
	bool    hasNfo    = false;
	qint64  size      = 0;
};

/**
 * SrrdbClient — blocking HTTP client for the public srrDB scene-release API
 * (https://api.srrdb.com/v1). Community-run, not an official/paid service —
 * see UserProfile::downloadSceneNfoEnabled(), off by default.
 *
 * Must run on a QThread (not a plain thread-pool worker) because it owns a
 * QNetworkAccessManager and uses QEventLoop for synchronous-style calls —
 * same shape as OpenSubtitlesClient.
 *
 * Three-step flow, since srrDB never inlines NFO content in its JSON:
 *   1. searchExact()/searchKeywords() — find the release
 *   2. resolveNfoLink()               — GET /v1/nfo/<release> for the download URL
 *   3. downloadBytes()                — GET that URL for the raw file bytes
 */
class SrrdbClient : public QObject
{
	Q_OBJECT
public:
	explicit SrrdbClient(QObject* parent = nullptr);
	~SrrdbClient() override;

	QString lastError()    const { return m_lastError; }
	bool    wasCancelled()  const { return m_cancelled.loadRelaxed() != 0; }

	// Exact-name lookup (srrDB's "r:" search prefix) — true only when a release
	// with precisely this name exists and has an NFO on file.
	bool searchExact(const QString& releaseName, SceneRelease& out);

	// Keyword AND-search (srrDB ANDs one or more path-segment keywords).
	// Returns every hit; caller filters/ranks (hasNfo, imdbId, ...).
	QList<SceneRelease> searchKeywords(const QStringList& keywords);

	// GET /v1/nfo/<releaseName> — resolves the srrdb.com download URL for that
	// release's .nfo file (srrDB never returns the content itself here).
	bool resolveNfoLink(const QString& releaseName, QString& outUrl);

	// Bare GET of an already-resolved srrdb.com download URL — no API headers.
	bool downloadBytes(const QUrl& url, QByteArray& outBytes);

public slots:
	// Abort whatever request is currently in flight (safe to invoke cross-thread
	// via a queued connection — delivered while blocked inside the nested
	// QEventLoop httpGetJson()/httpGetBytes() run).
	void cancel();

private:
	bool httpGetJson(const QUrl& url, QByteArray& out);   // API GET — adds Accept: application/json
	bool httpGetBytes(const QUrl& url, QByteArray& out);  // bare GET — no API headers (file download)
	QList<SceneRelease> parseResults(const QByteArray& json) const;

	QNetworkAccessManager* m_nam   = nullptr;
	QNetworkReply*         m_reply = nullptr;
	QString    m_lastError;
	int        m_lastStatus = 0;
	QAtomicInt m_cancelled{0};

	static constexpr const char* kBaseUrl = "https://api.srrdb.com/v1";
};

// ── Worker QObject — move to a QThread, drive via queued slot invocations ───
//
// Two-phase, unlike SubtitleDownloadWorker's single run(): search() may need to
// hand candidates back to the UI for a manual pick (renamed files won't match
// their original scene release name) before downloadRelease()/chooseRelease()
// can proceed — see McSceneNfoDownloadDialog.
class SceneNfoDownloadWorker : public QObject
{
	Q_OBJECT
public:
	explicit SceneNfoDownloadWorker(const QString& videoPath, const QString& imdbId,
	                                 QObject* parent = nullptr);

public slots:
	// Phase 1 — resolve candidate release(s). Auto-proceeds to the download
	// phase itself when exactly one candidate remains after IMDb-id filtering
	// (emitting releaseSelected() first); otherwise emits candidatesFound() for
	// the dialog to show a picker, or done(false, ...) when nothing matched.
	void search();
	// Phase 2 — called by the dialog once a candidate is chosen (either
	// automatically by search() or manually from its picker).
	void chooseRelease(QString releaseName);
	// Abort the in-flight request, if any. Safe to call cross-thread.
	void cancel();

signals:
	// Ambiguous match — labels are display strings ("release  [ttNNNNNNN]"),
	// names are the corresponding raw release names chooseRelease() expects.
	// Parallel QStringLists rather than QList<SceneRelease> so this queued
	// cross-thread signal needs no custom-type registration.
	void candidatesFound(QStringList labels, QStringList names);
	void releaseSelected(QString releaseName);
	void done(bool success, QString releaseName, QByteArray rawContent, QString errorMessage);

private:
	void startDownload(const QString& releaseName);

	QString    m_videoPath;
	QString    m_imdbId;
	QAtomicInt m_cancelled{0};
	SrrdbClient* m_client = nullptr; // valid only while a phase is executing
};

/**
 * SceneNfoManager — downloads missing scene-release NFOs for the library on a
 * background thread after scanning, mirroring SubtitleManager's always-on
 * queue but without any quota/backoff subsystem — srrDB has no known daily
 * quota, unlike OpenSubtitles.
 *
 * Only ever attempts a download when exactly one confident release match is
 * found for a file; an ambiguous result is left alone for the existing manual
 * "Download Scene NFO…" action (McSceneNfoDownloadDialog) to resolve
 * interactively — this queue never surfaces a picker.
 */
class SceneNfoManager : public QObject
{
	Q_OBJECT
public:
	static SceneNfoManager& instance();

	// Call once at startup (after profile is loaded). enabled should already be
	// the combination of UserProfile::downloadSceneNfoEnabled() (the feature is
	// on at all) and UserProfile::autoDownloadSceneNfo() (background queue is
	// on) — this manager doesn't know about either profile flag itself.
	void start(bool enabled);

	// Stop the background thread gracefully. Call from closeEvent before accept().
	void stop();

	// Push updated settings (e.g. when the user saves settings).
	void setEnabled(bool enabled);
	// UserProfile::writeNfoFiles() — whether a successful download also writes a
	// real .nfo file to disk, or stays database-only (see NfoParser::writeSceneNfoFile
	// and the "Enable Download Scene NFO" tooltip in McSettingsDialog).
	void setWriteNfoFiles(bool enabled);
	// Shared with poster/subtitle retry cooldown — UserProfile::subtitleRetryCooldownDays().
	void setRetryCooldownDays(int days);

	// Enqueue a newly-scanned (or rescanned) file for scene-NFO lookup.
	// No-op if disabled or the file already has a scene NFO.
	void enqueue(qint64 fileId);

	// Batch enqueue — one cross-thread signal for many IDs (e.g. quick-scan backfill).
	void enqueueBatch(const QList<qint64>& fileIds);

	// User-initiated cancel — drop every queued file and abort whatever is
	// currently downloading. The manager stays running and picks up newly
	// enqueued files afterward (mirrors SubtitleManager::cancelAll()).
	void cancelAll();

signals:
	// A background download wrote a scene NFO for fileId; the DB is already
	// updated — listeners only need to refresh their view.
	void sceneNfoReady(qint64 fileId);
	// The queue transitioned between idle and actively processing files.
	void queueActiveChanged(bool active);

	// Internal — cross-thread commands to the worker.
	void workerSetEnabled(bool enabled);
	void workerSetWriteNfoFiles(bool enabled);
	void workerSetRetryCooldownDays(int days);
	void workerEnqueueFile(qint64 fileId);
	void workerEnqueueBatch(QList<qint64> fileIds);
	void workerCancelAll();
	void workerStop();

private:
	explicit SceneNfoManager(QObject* parent = nullptr);

	QThread*        m_thread = nullptr;
	SceneNfoWorker* m_worker = nullptr;
};

} // namespace Mc
