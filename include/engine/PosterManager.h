#pragma once
#include <QByteArray>
#include <QImage>
#include <QList>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QThread;

namespace Mc {

class PosterWorker;

/**
 * PosterManager — downloads TMDB posters for the library on a background thread.
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

	// Stop the background thread gracefully. Call from closeEvent before accept().
	void stop();

	// Update the API key (e.g. when the user saves settings).
	// If the key changes from empty to non-empty, resets 'no_poster' records
	// so they get retried.
	void setTmdbApiKey(const QString& key);

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

signals:
	void posterReady(qint64 fileId, QString imagePath);
	void fanartReady(qint64 fileId, QString fanartPath, QImage image);
	// Fired when TMDB metadata (title, year, rating) has been fetched and written
	// to the DB.  The UI should update cards without waiting for an app restart.
	void tmdbDataReady(qint64 fileId, QString title, int year, double rating);
	// Fired synchronously inside refresh() as soon as the imdb_id is written to DB,
	// before any async poster download.  Lets the UI update the IMDb button immediately.
	void imdbIdSaved(qint64 fileId, QString imdbId);

	// Internal — cross-thread commands to the worker.
	void workerTmdbKeyChanged(QString key);
	void workerEnqueueFile(qint64 fileId);
	void workerEnqueueBatch(QList<qint64> fileIds);
	void workerStop();

private:
	explicit PosterManager(QObject* parent = nullptr);

	QThread*               m_thread = nullptr;
	PosterWorker*          m_worker = nullptr;
	QNetworkAccessManager* m_nam    = nullptr;   // main-thread NAM for fast direct fetches
	QString                m_tmdbApiKey;
};

} // namespace Mc
