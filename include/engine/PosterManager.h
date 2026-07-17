#pragma once
#include <QByteArray>
#include <QImage>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QThread;

namespace Mc {

class PosterWorker;

/**
 * PosterManager — downloads TMDB posters for the library on a pool of background threads.
 *
 * Requires a TMDB API key and an .nfo file containing an IMDB ID alongside each
 * media file. Nothing is processed until a key is configured.
 *
 * Results are persisted to poster_cache so restarts resume where they left off.
 * Each file is processed at most once unless refresh() is called.
 */
class PosterManager : public QObject
{
	Q_OBJECT
public:
	static PosterManager& instance();

	// Call once at startup (after profile is loaded).
	void start(const QString& tmdbApiKey);

	// Resize the TMDB worker pool (1–12). Safe to call after start(); takes effect immediately.
	void setParallelWorkers(int count);

	// Stop the background thread gracefully. Call from closeEvent before accept().
	void stop();

	// Update the API key (e.g. when the user saves settings).
	// If the key changes from empty to non-empty, resets 'no_poster' records
	// so they get retried.
	void setTmdbApiKey(const QString& key);

	// Whether background poster/metadata resolution is also allowed to write
	// the resolved IMDb id to a .nfo file next to the media file. Off by default
	// (see UserProfile::writeNfoFiles) — mirrors the user's Settings choice.
	void setWriteNfoFiles(bool enabled);

	// Ordered (ISO 639-2) language preference used to pick which localized TMDB
	// title gets written as <title> in the .nfo — mirrors UserProfile's
	// understood-languages list. <originaltitle> is always TMDB's original_title
	// regardless of this setting.
	void setUnderstoodLanguages(const QStringList& languages);

	// Enqueue a newly-scanned file for poster lookup without resetting existing data.
	// No-op if the file already has a poster or the TMDB key is not set.
	void enqueue(qint64 fileId);

	// Batch enqueue — one cross-thread signal for many IDs (e.g. quick-scan backfill).
	void enqueueBatch(const QList<qint64>& fileIds);

	// Reset poster for a single file and re-queue it immediately.
	// posterPath: TMDB poster_path already known (e.g. from search dialog) — skips the API lookup.
	// imageData:  raw image bytes already downloaded — written directly to cache, no network call.
	void refresh(qint64 fileId, const QString& posterPath = {},
	             const QByteArray& imageData = {}, const QString& imdbId = {},
	             double voteAverage = 0.0, int voteCount = 0,
	             const QString& fanartTmdbPath = {});

	// Refresh many files as one tracked batch — merges into an already-active batch
	// if one is in flight. Drives batchProgressChanged()/batchFinished() so the UI
	// can show a progress bar + cancel affordance for multi-selection refreshes.
	void refreshBatch(const QList<qint64>& fileIds);

	// User-initiated cancel — drops every not-yet-started file in the active batch
	// from the worker queue. A file already mid-download finishes normally (it's a
	// single quick HTTP round-trip); the manager keeps running afterward.
	void cancelBatch();

signals:
	void posterReady(qint64 fileId, QString imagePath);
	void fanartReady(qint64 fileId, QString fanartPath, QImage image);
	// Fired when TMDB metadata (title, year, rating) has been fetched and written
	// to the DB.  The UI should update cards without waiting for an app restart.
	void tmdbDataReady(qint64 fileId, QString title, int year, double rating);
	// Fired synchronously inside refresh() as soon as the imdb_id is written to DB,
	// before any async poster download.  Lets the UI update the IMDb button immediately.
	void imdbIdSaved(qint64 fileId, QString imdbId);

	// Batch refresh progress — driven by refreshBatch()/cancelBatch().
	void batchProgressChanged(int done, int total);
	void batchFinished(int done, int total, bool cancelled);

	// Internal — cross-thread commands to workers.
	void workerTmdbKeyChanged(QString key);
	void workerWriteNfoChanged(bool enabled);
	void workerUnderstoodLanguagesChanged(QStringList languages);
	void workerStop();

private:
	explicit PosterManager(QObject* parent = nullptr);
	void startWorkerPool();
	void stopWorkerPool(bool wait);

	class PosterWorkQueue* m_queue = nullptr;
	struct WorkerSlot {
		QThread*      thread = nullptr;
		PosterWorker* worker = nullptr;
	};
	QList<WorkerSlot>      m_workers;
	int                    m_parallelWorkers = 4;
	QNetworkAccessManager* m_nam           = nullptr;   // main-thread NAM for fast direct fetches
	QString                m_tmdbApiKey;
	bool                   m_writeNfoFiles = false;
	QStringList            m_understoodLanguages;

	QSet<qint64>           m_batchIds;
	int                    m_batchTotal  = 0;
	int                    m_batchDone   = 0;
	bool                   m_batchActive = false;
};

} // namespace Mc
