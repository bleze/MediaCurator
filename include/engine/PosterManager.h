#pragma once
#include <QByteArray>
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

	// Reset poster for a single file and re-queue it immediately.
	// posterPath: TMDB poster_path already known (e.g. from search dialog) — skips the API lookup.
	// imageData:  raw image bytes already downloaded — written directly to cache, no network call.
	void refresh(qint64 fileId, const QString& posterPath = {},
	             const QByteArray& imageData = {}, const QString& imdbId = {},
	             double voteAverage = 0.0, int voteCount = 0);

signals:
	void posterReady(qint64 fileId, QString imagePath);
	// Fired synchronously inside refresh() as soon as the imdb_id is written to DB,
	// before any async poster download.  Lets the UI update the IMDb button immediately.
	void imdbIdSaved(qint64 fileId, QString imdbId);

	// Internal — cross-thread commands to the worker.
	void workerTmdbKeyChanged(QString key);
	void workerEnqueueFile(qint64 fileId);
	void workerStop();

private:
	explicit PosterManager(QObject* parent = nullptr);

	QThread*               m_thread = nullptr;
	PosterWorker*          m_worker = nullptr;
	QNetworkAccessManager* m_nam    = nullptr;   // main-thread NAM for fast direct fetches
	QString                m_tmdbApiKey;
};

} // namespace Mc
