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

	// Ordered (ISO 639-2) language preference — mirrors UserProfile's
	// understood-languages list. A movie whose original language is on the list
	// is titled with TMDB's original_title; otherwise the top resolvable entry
	// picks the localized title. The same title is written as <title> in the
	// .nfo; <originaltitle> is always TMDB's original_title regardless.
	void setUnderstoodLanguages(const QStringList& languages);

	// Days to wait before retrying a file whose status is 'no_poster' (TMDB had no
	// match, or no API key was configured at the time). Without this, a file that
	// never resolves gets its NFO/local-art directory re-scanned on every single
	// launch forever — expensive and, for NAS-hosted libraries, enough to wake a
	// sleeping drive on every startup. 0 disables the cooldown (always retry).
	// Mirrors UserProfile::subtitleRetryCooldownDays().
	void setRetryCooldownDays(int days);

	// Mirrors UserProfile::showClearLogos(). Turning it on queues a logo lookup
	// for every matched file that hasn't had one yet.
	void setClearLogosEnabled(bool enabled);

	// Downloads the clear logo the user picked in the metadata dialog (TMDB
	// file_path) and pins it for every file matched to tmdbId — automatic
	// lookups never replace it unless the file is re-matched to another movie.
	void setUserLogo(qint64 fileId, int tmdbId, const QString& imdbId, const QString& logoTmdbPath);

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
	             const QString& fanartTmdbPath = {}, int tmdbId = 0);

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
	// Fired when TMDB metadata (title, year, rating, media category) has been
	// fetched and written to the DB.  The UI should update cards without a restart.
	// mediaType is a MediaTypes::* string (movie/tv/documentary/misc/unknown).
	void tmdbDataReady(qint64 fileId, QString title, int year, double rating, QString mediaType);
	// Fired synchronously inside refresh() as soon as the imdb_id is written to DB,
	// before any async poster download.  Lets the UI update the IMDb button immediately.
	void imdbIdSaved(qint64 fileId, QString imdbId);
	// Fired whenever a TMDB numeric id is resolved and persisted (refresh(), or the
	// background matcher's applyTmdbInfo). Lets the UI update the TMDB button immediately.
	void tmdbIdSaved(qint64 fileId, int tmdbId);
	// Fired whenever release dates (premiere/digital/physical) are resolved and
	// persisted. Any of the three may be empty (unknown).
	void releaseDatesReady(qint64 fileId, QString premiereDate, QString digitalDate, QString physicalDate);
	// Fired whenever a TMDB trailer (YouTube key) is resolved and persisted.
	void trailerReady(qint64 fileId, QString trailerKey);
	// Fired after a TMDB collection lookup; collectionId 0 = not in a collection.
	void collectionReady(qint64 fileId, int collectionId, QString collectionName);
	// Fired whenever a clear logo is resolved; empty path = none available.
	void logoReady(qint64 fileId, QString logoPath);

	// Batch refresh progress — driven by refreshBatch()/cancelBatch().
	void batchProgressChanged(int done, int total);
	void batchFinished(int done, int total, bool cancelled);

	// Internal — cross-thread commands to workers.
	void workerTmdbKeyChanged(QString key);
	void workerWriteNfoChanged(bool enabled);
	void workerUnderstoodLanguagesChanged(QStringList languages);
	void workerRetryCooldownChanged(int days);
	void workerStop();

private:
	explicit PosterManager(QObject* parent = nullptr);
	void startWorkerPool();
	void stopWorkerPool(bool wait);
	// Queues every already-titled file whose original language is understood but
	// whose title isn't the original one yet (existing libraries, or a language
	// newly added to the understood list).
	void enqueueOriginalTitleBackfill();

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
	int                    m_retryCooldownDays = 7;
	QString                m_posterTmdbSize = QStringLiteral("w92");   // set in start(), see tmdbPosterSizeForDpr()

	QSet<qint64>           m_batchIds;
	int                    m_batchTotal  = 0;
	int                    m_batchDone   = 0;
	bool                   m_batchActive = false;
};

} // namespace Mc
