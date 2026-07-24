#include "engine/PosterManager.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"
#include "scanner/NfoParser.h"
#include "scanner/ScanWorker.h"
#include "ui/McLanguageFlags.h"

#include <QDateTime>
#include <QUrl>
#include <utility>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QMutex>
#include <QTextStream>
#include <QThread>
#include <QDebug>

namespace {

// Poster cards render at a fixed 100 logical px wide (McCardDelegate::kPosterW)
// regardless of DPI scaling, so the physical pixels actually needed scale with
// devicePixelRatio() alone — e.g. 300 at 300% scaling. Picks the smallest TMDB
// poster tier within 10% of that (w92 already looks fine at 100% scaling even
// though 92 < 100 — no need to bump every default-DPI user to a bigger download
// over a practically invisible shortfall), falling back to "original" if even
// TMDB's largest poster tier undershoots it.
QString tmdbPosterSizeForDpr(qreal dpr)
{
	constexpr int   kCardPosterWidth = 100;
	constexpr float kTolerance       = 0.9f;
	const int       neededPx         = qRound(kCardPosterWidth * dpr * kTolerance);
	for (const auto& [px, tag] : {std::pair{92, "w92"}, {154, "w154"}, {185, "w185"},
	                               {342, "w342"}, {500, "w500"}, {780, "w780"}}) {
		if (neededPx <= px)
			return QLatin1String(tag);
	}
	return QStringLiteral("original");
}
// Temporary instrumentation for the "auto-update finish-page Run checkbox
// doesn't restart the app" regression (2026-07-23) — see the matching helper
// in main.cpp. Gated on MC_DEBUG_LOG so it's silent in shipped installs.
// Remove once the root cause is confirmed.
void logRestartDebug(const QString& line)
{
	if (!qEnvironmentVariableIsSet("MC_DEBUG_LOG")) return;
	static const QString logPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
	                                + QStringLiteral("/MediaCurator-restart_debug.log");
	QFile f(logPath);
	if (f.open(QIODevice::Append | QIODevice::Text)) {
		QTextStream ts(&f);
		ts << QDateTime::currentDateTime().toString(Qt::ISODate) << "  " << line << "\n";
	}
}
} // namespace

#include <optional>

namespace Mc {

// Consecutive failed-resolve attempts (each spaced out by the retry cooldown) a
// 'no_poster' file gets before processFile() gives up permanently and marks it
// 'unresolvable'. Content that will never get an imdb_id (trailers, behind-the-
// scenes clips under an "Extras" folder, etc.) used to retry forever, re-running
// the folder-scan path below every time the cooldown lapsed.
static constexpr int kMaxPosterResolveAttempts = 2;

// Verify cached poster/fanart files still exist; reset DB rows for missing files.
static void resetMissingPosterMediaInDb()
{
	{
		const QHash<qint64, QString> paths = DatabaseManager::instance().allDonePosterPaths();
		QSet<QString> checked;
		for (const QString& path : paths) {
			if (path.isEmpty() || checked.contains(path)) continue;
			checked.insert(path);
			if (!QFile::exists(path))
				DatabaseManager::instance().clearPosterPath(path);
		}
	}
	{
		const QHash<qint64, QString> paths = DatabaseManager::instance().allDoneFanartPaths();
		QSet<QString> checked;
		for (const QString& path : paths) {
			if (path.isEmpty() || checked.contains(path)) continue;
			checked.insert(path);
			if (!QFile::exists(path))
				DatabaseManager::instance().clearFanartPath(path);
		}
	}
}

// ── Shared work queue (all poster workers pull from this) ───────────────────────

class PosterWorkQueue : public QObject
{
	Q_OBJECT
public:
	explicit PosterWorkQueue(QObject* parent = nullptr) : QObject(parent) {}

	void enqueueFile(qint64 fileId)
	{
		if (fileId == -1) {
			loadPendingFromDb();
			emit workAvailable();
			return;
		}
		if (fileId <= 0) return;
		{
			QMutexLocker lock(&m_mutex);
			m_queue.removeOne(fileId);
			m_queue.prepend(fileId);
		}
		emit workAvailable();
	}

	void enqueueBatch(const QList<qint64>& fileIds)
	{
		if (fileIds.isEmpty()) return;
		{
			QMutexLocker lock(&m_mutex);
			for (qint64 id : fileIds) {
				if (id > 0 && !m_queue.contains(id))
					m_queue.append(id);
			}
		}
		emit workAvailable();
	}

	void loadPendingFromDb()
	{
		QMutexLocker lock(&m_mutex);
		for (qint64 id : DatabaseManager::instance().fileIdsNeedingPosters()) {
			if (!m_queue.contains(id))
				m_queue.append(id);
		}
	}

	[[nodiscard]] std::optional<qint64> takeNext()
	{
		QMutexLocker lock(&m_mutex);
		if (m_queue.isEmpty())
			return std::nullopt;
		return m_queue.takeFirst();
	}

	[[nodiscard]] bool isEmpty() const
	{
		QMutexLocker lock(&m_mutex);
		return m_queue.isEmpty();
	}

	// Drops every id still waiting in the queue (a file already taken by a worker
	// is unaffected — it finishes normally).
	void removeIds(const QSet<qint64>& ids)
	{
		if (ids.isEmpty()) return;
		QMutexLocker lock(&m_mutex);
		for (int i = m_queue.size() - 1; i >= 0; --i)
			if (ids.contains(m_queue[i])) m_queue.removeAt(i);
	}

signals:
	void workAvailable();

private:
	mutable QMutex m_mutex;
	QList<qint64>  m_queue;
};

// ── PosterWorker ──────────────────────────────────────────────────────────────

class PosterWorker : public QObject
{
	Q_OBJECT
public:
	explicit PosterWorker(PosterWorkQueue* queue, const QString& tmdbApiKey,
	                      bool writeNfoFiles, const QStringList& understoodLanguages,
	                      int retryCooldownDays, const QString& posterTmdbSize,
	                      QObject* parent = nullptr)
	    : QObject(parent)
	    , m_queue(queue)
	    , m_tmdbApiKey(tmdbApiKey)
	    , m_writeNfoFiles(writeNfoFiles)
	    , m_understoodLanguages(understoodLanguages)
	    , m_retryCooldownDays(retryCooldownDays)
	    , m_posterTmdbSize(posterTmdbSize)
	{
		const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
		m_cacheDir  = dataDir + "/posters";
		m_fanartDir = dataDir + "/fanart";
		QDir().mkpath(m_cacheDir);
		QDir().mkpath(m_fanartDir);
	}

public slots:
	void startProcessing()
	{
		m_nam = new QNetworkAccessManager(this);
		connect(m_queue, &PosterWorkQueue::workAvailable,
		        this, &PosterWorker::tryProcessNext, Qt::QueuedConnection);
		tryProcessNext();
	}

	void stop()
	{
		m_stopping = true;
		if (m_currentReply) {
			// We're currently nested inside fetchHttp()'s QEventLoop (that's the
			// only place m_currentReply is non-null). abort() synchronously emits
			// finished(), which is connected to that loop's quit() — so it unwinds
			// on its own. tryProcessNext() calls thread()->quit() once control is
			// back on the real event loop; doing it here instead would just quit
			// the nested loop again (QThread::quit()/exit() only affects whichever
			// loop is currently innermost), leaving the real loop running forever
			// — the root cause of the old shutdown race.
			m_currentReply->abort();
			m_currentReply = nullptr;
		} else if (!m_processing) {
			// Idle — not nested inside any loop — safe to quit the real event
			// loop directly.
			QThread::currentThread()->quit();
		}
	}

	void setTmdbApiKey(const QString& key) { m_tmdbApiKey = key; }
	void setWriteNfoFiles(bool enabled) { m_writeNfoFiles = enabled; }
	void setUnderstoodLanguages(QStringList languages) { m_understoodLanguages = std::move(languages); }
	void setRetryCooldownDays(int days) { m_retryCooldownDays = qMax(0, days); }

signals:
	void posterReady(qint64 fileId, QString imagePath);
	void fanartReady(qint64 fileId, QString fanartPath, QImage image);
	void tmdbDataReady(qint64 fileId, QString title, int year, double rating, QString mediaType);
	void imdbIdSaved(qint64 fileId, QString imdbId);
	void tmdbIdSaved(qint64 fileId, int tmdbId);
	// Fired once per file after processFile() returns, regardless of outcome —
	// drives PosterManager's batch-refresh progress tracking.
	void fileProcessed(qint64 fileId);

private slots:
	void tryProcessNext()
	{
		// A reentrant call while m_processing is true means we're nested inside
		// fetchHttp()'s event loop for the file currently being processed (e.g. a
		// workAvailable signal for newly-enqueued work delivered while that nested
		// loop is pumping this thread's queue) — never safe to quit from here.
		if (m_processing)
			return;

		if (m_stopping) {
			QThread::currentThread()->quit();
			return;
		}

		const auto fileId = m_queue->takeNext();
		if (!fileId)
			return;

		m_processing = true;
		processFile(*fileId);
		m_processing = false;
		emit fileProcessed(*fileId);

		if (m_stopping)
			QThread::currentThread()->quit();
		else
			QMetaObject::invokeMethod(this, &PosterWorker::tryProcessNext, Qt::QueuedConnection);
	}

private:
	void processFile(qint64 fileId)
	{
		const auto fileOpt = DatabaseManager::instance().fileById(fileId);
		if (!fileOpt) return;

		const QString filePath = fileOpt->path;
		const QString baseStem = QFileInfo(filePath).completeBaseName();

		const auto existing = DatabaseManager::instance().posterForFile(fileId);

		// Helper: persist TMDB data from a TmdbInfo result, writing .nfo for durability.
		// Also emits tmdbDataReady so the UI updates immediately without a DB round-trip.
		auto applyTmdbInfo = [&](const TmdbInfo& info, const QString& resolvedImdbId) {
			// Only auto-fill category when unset — never clobber a user override
			// (Set Category menu). Dialog accepts always write via McMainWindow.
			QString appliedMediaType;
			if (!info.mediaType.isEmpty()
			    && info.mediaType != QLatin1String(MediaTypes::Unknown)
			    && (fileOpt->mediaType.isEmpty()
			        || fileOpt->mediaType == QLatin1String(MediaTypes::Unknown))) {
				DatabaseManager::instance().updateMediaType(fileId, info.mediaType);
				appliedMediaType = info.mediaType;
			}
			if (!info.title.isEmpty()) {
				if (fileOpt->displayTitle.isEmpty())
					DatabaseManager::instance().updateDisplayTitle(fileId, info.title, info.year);
			}
			if (!info.title.isEmpty() || !appliedMediaType.isEmpty()) {
				emit tmdbDataReady(fileId, info.title, info.year, info.voteAverage, appliedMediaType);
			}
			// Write .nfo so imdbId (plus whatever TMDB metadata we have) survives
			// future DB deletions — opt-in, and only for a not-yet-existing .nfo.
			// TV still gets a movie-style NFO for now (IMDb/title tags); full tvshow
			// NFO support can land later.
			if (m_writeNfoFiles && !resolvedImdbId.isEmpty()
			    && !QFile::exists(NfoParser::nfoPathFor(filePath))) {
				NfoMovieMeta meta;
				meta.tmdbId        = info.tmdbId;
				meta.title         = info.title;
				meta.originalTitle = info.originalTitle;
				meta.year          = info.year;
				meta.premiered     = info.releaseDate;
				meta.voteAverage   = info.voteAverage;
				meta.voteCount     = info.voteCount;
				NfoParser::writeMovieNfo(filePath, resolvedImdbId, meta);
			}
			// Newly-discovered imdbId (title-search recovery) needs to reach the live
			// UI models too -- upsertPosterRecord() alone only updates the DB.
			if (!resolvedImdbId.isEmpty())
				emit imdbIdSaved(fileId, resolvedImdbId);
			// tmdbId isn't always set on the PosterRecord the caller upserts (several
			// branches only touch rating/title), so persist it here unconditionally
			// instead of relying on that upsert to carry it.
			if (info.tmdbId > 0) {
				DatabaseManager::instance().updateTmdbId(fileId, info.tmdbId);
				emit tmdbIdSaved(fileId, info.tmdbId);
			}
		};

		// Skip files that are already fully done (poster + rating + title).
		// Checked BEFORE the NFO/local-art directory scans below — a fully-cached
		// file needs neither, and those scans are what make this a per-file NAS
		// stat/listing on every launch instead of a cheap DB read.
		if (existing && existing->status == "done"
		    && !existing->imagePath.isEmpty()
		    && QFile::exists(existing->imagePath)) {
			const bool needsRating    = existing->voteAverage <= 0.0;
			const bool needsTitle     = fileOpt->displayTitle.isEmpty();
			const bool needsMediaType = fileOpt->mediaType.isEmpty()
			    || fileOpt->mediaType == QLatin1String(MediaTypes::Unknown);
			const bool needsNfo       = m_writeNfoFiles && !QFile::exists(NfoParser::nfoPathFor(filePath));
			bool needsFanart          = existing->fanartPath.isEmpty()
			                         || !QFile::exists(existing->fanartPath);

			QString imdbId = existing->imdbId;

			// Fast path: fanart file already on disk — no API call needed.
			if (needsFanart && !imdbId.isEmpty()) {
				const QString outPath = m_fanartDir + "/" + imdbId + ".jpg";
				if (QFile::exists(outPath)) {
					PosterRecord pr = *existing;
					pr.fanartPath = outPath;
					DatabaseManager::instance().upsertPosterRecord(pr);
					QImage img(outPath);
					if (!img.isNull() && img.height() > 300) {
						const int y = (img.height() - 300) / 2;
						img = img.copy(0, y, img.width(), 300);
					}
					emit fanartReady(fileId, outPath, img);
					needsFanart = false;
				}
			}

			if (!needsRating && !needsTitle && !needsMediaType && !needsNfo && !needsFanart) return;

			// Something is still missing — only now fall back to the NFO file
			// for an imdbId the DB doesn't already have.
			if (imdbId.isEmpty())
				imdbId = findNfoImdbId(filePath);
			if (imdbId.isEmpty() || m_tmdbApiKey.isEmpty()) return;
			const TmdbInfo info = fetchTmdbInfo(imdbId);
			if (needsRating && info.voteAverage > 0.0) {
				PosterRecord pr = *existing;
				pr.voteAverage = info.voteAverage;
				pr.voteCount   = info.voteCount;
				DatabaseManager::instance().upsertPosterRecord(pr);
			}
			if (needsFanart) {
				const QString outPath = m_fanartDir + "/" + imdbId + ".jpg";
				QByteArray fanartBytes;
				const bool diskHit   = QFile::exists(outPath);
				const bool dlOk      = !diskHit && !info.backdropPath.isEmpty()
				    && downloadImage(
				           QStringLiteral("https://image.tmdb.org/t/p/original%1").arg(info.backdropPath),
				           outPath, &fanartBytes);
				if (diskHit || dlOk) {
					PosterRecord pr = existing ? *existing : PosterRecord{};
					pr.fileId     = fileId;
					pr.fanartPath = outPath;
					DatabaseManager::instance().upsertPosterRecord(pr);
					QImage img;
					if (dlOk && !fanartBytes.isEmpty())
						img.loadFromData(fanartBytes);
					else
						img = QImage(outPath);
					if (!img.isNull() && img.height() > 300) {
						const int y = (img.height() - 300) / 2;
						img = img.copy(0, y, img.width(), 300);
					}
					emit fanartReady(fileId, outPath, img);
				}
			}
			applyTmdbInfo(info, imdbId);
			return;
		}

		// A file TMDB never matched (or that was processed with no API key set)
		// stays 'no_poster' with an empty display_title forever, which keeps it
		// selected by fileIdsNeedingPosters() on every future launch. Without this
		// cooldown, that means re-running findNfoImdbId()/findLocalArt() below —
		// both directory listings against the file's own folder — for the same
		// permanently-unresolved file on every single startup, which is what was
		// waking sleeping NAS drives and adding real per-launch delay. 0 disables
		// the cooldown (always retry).
		if (existing && existing->status == "no_poster" && m_retryCooldownDays > 0
		    && existing->fetchedAt > 0) {
			// Content that will never resolve (trailers, "Extras" sub-clips, etc.)
			// would otherwise keep cycling through this cooldown forever — one more
			// NAS-waking folder scan every time it lapses. Give up for good instead.
			if (existing->attemptCount >= kMaxPosterResolveAttempts) {
				PosterRecord pr = *existing;
				pr.status = "unresolvable";
				DatabaseManager::instance().upsertPosterRecord(pr);
				return;
			}
			const qint64 cooldownMs = qint64(m_retryCooldownDays) * 24 * 60 * 60 * 1000;
			if (QDateTime::currentMSecsSinceEpoch() - existing->fetchedAt < cooldownMs)
				return;
		}

		// Not fully done — resolve the imdbId (NFO first, DB fallback) and seed
		// the poster/fanart cache from any local sidecar art before the
		// disk-cache-reuse checks below run, so they can skip the network entirely.
		QString imdbId = findNfoImdbId(filePath);
		if (imdbId.isEmpty() && existing && !existing->imdbId.isEmpty())
			imdbId = existing->imdbId;

		// A "<basename>-poster.<ext>" / "<basename>-fanart.<ext>" file next to the
		// video takes priority over TMDB — seed the cache from it before the
		// disk-cache-reuse checks below run, so they pick it up and skip the network
		// entirely. Fanart caching needs a known imdbId (same requirement the rest
		// of this class already has for fanart, e.g. the lazy re-check just above).
		const LocalArt localArt = findLocalArt(filePath);
		cacheLocalImage(localArt.posterPath,
		                imdbId.isEmpty() ? (m_cacheDir + "/" + baseStem + ".jpg")
		                                 : (m_cacheDir + "/" + imdbId + ".jpg"));
		if (!imdbId.isEmpty())
			cacheLocalImage(localArt.fanartPath, m_fanartDir + "/" + imdbId + ".jpg");

		PosterRecord rec;
		rec.fileId    = fileId;
		rec.imdbId    = imdbId;
		rec.fetchedAt = QDateTime::currentMSecsSinceEpoch();

		// Reuse a poster already on disk (survives DB deletion).
		// If imdbId is known, look for {imdbId}.jpg; fall back to {baseStem}.jpg.
		const QString byImdb = imdbId.isEmpty() ? QString() : (m_cacheDir + "/" + imdbId + ".jpg");
		const QString byBase = m_cacheDir + "/" + baseStem + ".jpg";
		const QString cached = (!byImdb.isEmpty() && QFile::exists(byImdb)) ? byImdb
		                     : (QFile::exists(byBase) ? byBase : QString());

		if (!cached.isEmpty()) {
			rec.source    = "tmdb";
			rec.status    = "done";
			rec.imagePath = cached;
			if (!m_tmdbApiKey.isEmpty()) {
				if (!imdbId.isEmpty()) {
					const TmdbInfo info = fetchTmdbInfo(imdbId);
					rec.voteAverage = info.voteAverage;
					rec.voteCount   = info.voteCount;
					applyTmdbInfo(info, imdbId);
				} else {
					// Poster named by baseStem — no imdbId known; try title search to recover it.
					const auto [title, yearHint] = bestTitleAndYear(*fileOpt);
					if (!title.isEmpty()) {
						const bool preferTv = pathLooksLikeTv(filePath);
						const TmdbInfo info = fetchTmdbInfoByTitle(title, yearHint, preferTv);
						if (!info.imdbId.isEmpty()) {
							rec.imdbId      = info.imdbId;
							rec.voteAverage = info.voteAverage;
							rec.voteCount   = info.voteCount;
							applyTmdbInfo(info, info.imdbId);
						}
					}
				}
			}
			DatabaseManager::instance().upsertPosterRecord(rec);
			emit posterReady(fileId, cached);

			// Also surface fanart if already on disk — no API call needed.
			if (!rec.imdbId.isEmpty()) {
				const QString fanartOut = m_fanartDir + "/" + rec.imdbId + ".jpg";
				if (QFile::exists(fanartOut)) {
					PosterRecord pr = rec;
					pr.fanartPath = fanartOut;
					DatabaseManager::instance().upsertPosterRecord(pr);
					QImage img(fanartOut);
					if (!img.isNull() && img.height() > 300) {
						const int y = (img.height() - 300) / 2;
						img = img.copy(0, y, img.width(), 300);
					}
					emit fanartReady(fileId, fanartOut, img);
				}
			}
			return;
		}

		// Nothing on disk — need to download.  No API key → drain queue cleanly.
		if (m_tmdbApiKey.isEmpty()) {
			rec.status       = "no_poster";
			rec.attemptCount = (existing ? existing->attemptCount : 0) + 1;
			DatabaseManager::instance().upsertPosterRecord(rec);
			return;
		}

		// Resolve TmdbInfo: by known imdbId, or by title search if we have none.
		TmdbInfo info;
		if (!imdbId.isEmpty()) {
			info = fetchTmdbInfo(imdbId);
		} else {
			const auto [title, yearHint] = bestTitleAndYear(*fileOpt);
			if (!title.isEmpty())
				info = fetchTmdbInfoByTitle(title, yearHint, pathLooksLikeTv(filePath));
			if (!info.imdbId.isEmpty())
				imdbId = info.imdbId;
		}

		QString imagePath;
		if (!imdbId.isEmpty() && !info.posterPath.isEmpty()) {
			const QString outPath = m_cacheDir + "/" + imdbId + ".jpg";
			if (downloadImage(
			        QStringLiteral("https://image.tmdb.org/t/p/%1%2").arg(m_posterTmdbSize, info.posterPath),
			        outPath))
				imagePath = outPath;
		}
		if (!imdbId.isEmpty()) {
			const QString outPath = m_fanartDir + "/" + imdbId + ".jpg";
			QByteArray fanartBytes;
			const bool diskHit = QFile::exists(outPath);
			const bool dlOk    = !diskHit && !info.backdropPath.isEmpty()
			    && downloadImage(
			           QStringLiteral("https://image.tmdb.org/t/p/original%1").arg(info.backdropPath),
			           outPath, &fanartBytes);
			if (diskHit || dlOk) {
				rec.fanartPath = outPath;
				QImage img;
				if (dlOk && !fanartBytes.isEmpty())
					img.loadFromData(fanartBytes);
				else
					img = QImage(outPath);
				if (!img.isNull() && img.height() > 300) {
					const int y = (img.height() - 300) / 2;
					img = img.copy(0, y, img.width(), 300);
				}
				emit fanartReady(fileId, outPath, img);
			}
		}
		rec.imdbId      = imdbId;
		rec.voteAverage = info.voteAverage;
		rec.voteCount   = info.voteCount;
		if (!info.title.isEmpty())
			applyTmdbInfo(info, imdbId);

		if (!imagePath.isEmpty()) {
			rec.source    = "tmdb";
			rec.status    = "done";
			rec.imagePath = imagePath;
			DatabaseManager::instance().upsertPosterRecord(rec);
			emit posterReady(fileId, imagePath);
		} else {
			rec.status       = "no_poster";
			rec.attemptCount = (existing ? existing->attemptCount : 0) + 1;
			DatabaseManager::instance().upsertPosterRecord(rec);
		}
	}

	// ── NFO parsing ───────────────────────────────────────────────────────────

	QString findNfoImdbId(const QString& filePath)
	{
		const QDir dir(QFileInfo(filePath).absolutePath());
		const QStringList nfoFiles = dir.entryList({"*.nfo"}, QDir::Files);
		static const QRegularExpression imdbRe(R"(\btt\d{7,8}\b)");

		for (const QString& nfo : nfoFiles) {
			QFile f(dir.filePath(nfo));
			if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
			const auto match = imdbRe.match(QString::fromUtf8(f.readAll()));
			if (match.hasMatch()) return match.captured(0);
		}
		return {};
	}

	// ── Local poster/fanart sidecar images ───────────────────────────────────

	struct LocalArt {
		QString posterPath;  // e.g. "<video-basename>-poster.<ext>" or "folder.<ext>"
		QString fanartPath;  // e.g. "<video-basename>-fanart.<ext>" or "backdrop.<ext>"
	};

	// Kodi/Plex both recognize the same two local-artwork conventions (Plex's docs
	// explicitly borrow Kodi's): a "<video-basename>-poster.<ext>" / "-fanart.<ext>"
	// form that stays unambiguous even when a folder holds several movies, and a
	// bare generic form ("folder.jpg", "cover.jpg", "backdrop.jpg", ...) meant for
	// a movie that owns its folder outright (single-file/optical-media layout).
	// See https://kodi.wiki/view/Artwork_types and
	// https://support.plex.tv/articles/200220677-local-media-assets-movies/
	//
	// Extensions and generic names are a small fixed vocabulary defined by those
	// two apps, not an open pattern, so plain case-insensitive name comparisons
	// are used rather than a regex.
	static LocalArt findLocalArt(const QString& videoPath)
	{
		const QFileInfo videoFi(videoPath);
		const QString   baseName = videoFi.completeBaseName();
		const QDir      dir(videoFi.absolutePath());

		// jpg/jpeg/png are the formats Qt's bundled plugins can always decode;
		// .tbn is Kodi's legacy thumbnail extension — always a jpg/png underneath,
		// so no separate decoding path is needed, just recognizing the extension.
		static const QStringList imageFilters = {"*.jpg", "*.jpeg", "*.png", "*.tbn"};
		static const QStringList posterNames  = {"poster", "folder", "cover", "movie", "default"};
		static const QStringList fanartNames  = {"fanart", "backdrop", "background", "art"};

		const QFileInfoList entries = dir.entryInfoList(imageFilters, QDir::Files);

		LocalArt result;

		// Pass 1: "<video-basename>-poster/-fanart.<ext>" — safe no matter how many
		// other video files share this folder.
		for (const QFileInfo& fi : entries) {
			const QString stem = fi.completeBaseName();
			if (result.posterPath.isEmpty()
			    && stem.compare(baseName + QLatin1String("-poster"), Qt::CaseInsensitive) == 0)
				result.posterPath = fi.absoluteFilePath();
			else if (result.fanartPath.isEmpty()
			    && stem.compare(baseName + QLatin1String("-fanart"), Qt::CaseInsensitive) == 0)
				result.fanartPath = fi.absoluteFilePath();
		}
		if (!result.posterPath.isEmpty() && !result.fanartPath.isEmpty())
			return result;

		// Pass 2: bare generic names. Only trustworthy when this video is the only
		// one in the folder — otherwise "folder.jpg" would be claimed by whichever
		// movie happens to be scanned first.
		static const QStringList videoFilters = [] {
			QStringList f;
			for (const QString& ext : ScanWorker::videoExtensions())
				f << (QLatin1String("*.") + ext);
			return f;
		}();
		if (dir.entryList(videoFilters, QDir::Files).size() == 1) {
			for (const QFileInfo& fi : entries) {
				const QString stem = fi.completeBaseName().toLower();
				if (result.posterPath.isEmpty() && posterNames.contains(stem))
					result.posterPath = fi.absoluteFilePath();
				else if (result.fanartPath.isEmpty() && fanartNames.contains(stem))
					result.fanartPath = fi.absoluteFilePath();
			}
		}
		return result;
	}

	// Copies a local sidecar image into the poster/fanart cache dir, keyed the same
	// way the rest of this class looks images up, so the existing disk-cache-reuse
	// checks below pick it up transparently and skip TMDB entirely. A no-op if the
	// cache already has a file at that path (nothing to refresh from disk).
	static void cacheLocalImage(const QString& sourcePath, const QString& destPath)
	{
		if (sourcePath.isEmpty() || destPath.isEmpty() || QFile::exists(destPath))
			return;
		if (!QFile::copy(sourcePath, destPath))
			qWarning() << "PosterWorker: failed to cache local art" << sourcePath << "->" << destPath;
	}

	// ── TMDB lookup ───────────────────────────────────────────────────────────

	struct TmdbInfo {
		QString posterPath;
		QString backdropPath;  // best backdrop for use as card fanart
		double  voteAverage = 0.0;
		int     voteCount   = 0;
		QString title;          // localized per m_understoodLanguages' top resolvable entry, else unlocalized
		int     year        = 0;
		QString imdbId;     // populated by fetchTmdbInfoByTitle; caller already has it for fetchTmdbInfo
		int     tmdbId        = 0;
		QString originalTitle; // TMDB's original_title / original_name — language-independent
		QString releaseDate;   // full YYYY-MM-DD, for <premiered>
		QString mediaType;     // MediaTypes::* (movie/tv/documentary)
		bool    isTv        = false; // true → use /tv/* endpoints for localized title / external ids
	};

	// TMDB genre id 99 = Documentary (movies and TV).
	static constexpr int kGenreDocumentary = 99;

	static bool jsonHasGenre(const QJsonObject& obj, int genreId)
	{
		for (const QJsonValue& v : obj[QStringLiteral("genre_ids")].toArray()) {
			if (v.toInt() == genreId)
				return true;
		}
		// /movie/{id} and /tv/{id} return genres as [{id,name}, ...] not genre_ids
		for (const QJsonValue& v : obj[QStringLiteral("genres")].toArray()) {
			if (v.toObject()[QStringLiteral("id")].toInt() == genreId)
				return true;
		}
		return false;
	}

	// UserProfile::understoodLanguages(), in priority order, converted to the
	// ISO 639-1 codes TMDB's `language` query param expects. Unmapped/duplicate
	// codes are dropped.
	QStringList preferredIso1Languages() const
	{
		QStringList out;
		for (const QString& code : m_understoodLanguages) {
			const QString iso1 = McLanguageFlags::toIso1(code);
			if (!iso1.isEmpty() && !out.contains(iso1))
				out << iso1;
		}
		return out;
	}

	static TmdbInfo tmdbInfoFromMovieJson(const QJsonObject& obj, const QString& imdbId)
	{
		const QString releaseDate = obj[QStringLiteral("release_date")].toString();
		const int year = releaseDate.left(4).toInt();
		TmdbInfo info;
		info.posterPath    = obj[QStringLiteral("poster_path")].toString();
		info.backdropPath  = obj[QStringLiteral("backdrop_path")].toString();
		info.voteAverage   = obj[QStringLiteral("vote_average")].toDouble();
		info.voteCount     = obj[QStringLiteral("vote_count")].toInt();
		info.title         = obj[QStringLiteral("title")].toString();
		info.year          = year;
		info.imdbId        = imdbId;
		info.tmdbId        = obj[QStringLiteral("id")].toInt();
		info.originalTitle = obj[QStringLiteral("original_title")].toString();
		info.releaseDate   = releaseDate;
		info.isTv          = false;
		info.mediaType     = MediaTypes::classify(
		    QStringLiteral("movie"), jsonHasGenre(obj, kGenreDocumentary));
		return info;
	}

	static TmdbInfo tmdbInfoFromTvJson(const QJsonObject& obj, const QString& imdbId)
	{
		// TV uses name / original_name / first_air_date instead of title / release_date.
		const QString airDate = obj[QStringLiteral("first_air_date")].toString();
		const int year = airDate.left(4).toInt();
		TmdbInfo info;
		info.posterPath    = obj[QStringLiteral("poster_path")].toString();
		info.backdropPath  = obj[QStringLiteral("backdrop_path")].toString();
		info.voteAverage   = obj[QStringLiteral("vote_average")].toDouble();
		info.voteCount     = obj[QStringLiteral("vote_count")].toInt();
		info.title         = obj[QStringLiteral("name")].toString();
		info.year          = year;
		info.imdbId        = imdbId;
		info.tmdbId        = obj[QStringLiteral("id")].toInt();
		info.originalTitle = obj[QStringLiteral("original_name")].toString();
		info.releaseDate   = airDate;
		info.isTv          = true;
		info.mediaType     = MediaTypes::classify(
		    QStringLiteral("tv"), jsonHasGenre(obj, kGenreDocumentary));
		return info;
	}

	// Localized title lookup, kept separate from art/rating/id resolution so a
	// language-tagged request can never cost us the poster (TMDB returns NULL
	// image paths when no language-tagged art exists).
	QString fetchLocalizedTitle(int tmdbId, bool isTv, const QStringList& langs)
	{
		const QString kind = isTv ? QStringLiteral("tv") : QStringLiteral("movie");
		const QString titleKey = isTv ? QStringLiteral("name") : QStringLiteral("title");
		for (const QString& lang : langs) {
			QByteArray data;
			if (!fetchHttp(QUrl(QStringLiteral(
			        "https://api.themoviedb.org/3/%1/%2?api_key=%3&language=%4")
			        .arg(kind).arg(tmdbId).arg(m_tmdbApiKey, lang)), data))
				continue;
			const QString localizedTitle =
			    QJsonDocument::fromJson(data).object()[titleKey].toString();
			if (!localizedTitle.isEmpty())
				return localizedTitle;
		}
		return {};
	}

	// Look up by known IMDb ID — checks movie_results then tv_results.
	TmdbInfo fetchTmdbInfo(const QString& imdbId)
	{
		const QUrl url(QStringLiteral(
		    "https://api.themoviedb.org/3/find/%1?external_source=imdb_id&api_key=%2")
		    .arg(imdbId, m_tmdbApiKey));

		QByteArray data;
		if (!fetchHttp(url, data)) return {};

		const QJsonObject root = QJsonDocument::fromJson(data).object();
		const QJsonArray movies = root[QStringLiteral("movie_results")].toArray();
		const QJsonArray tvs    = root[QStringLiteral("tv_results")].toArray();

		TmdbInfo info;
		if (!movies.isEmpty())
			info = tmdbInfoFromMovieJson(movies.first().toObject(), imdbId);
		else if (!tvs.isEmpty())
			info = tmdbInfoFromTvJson(tvs.first().toObject(), imdbId);
		else
			return {};

		const QString localizedTitle =
		    fetchLocalizedTitle(info.tmdbId, info.isTv, preferredIso1Languages());
		if (!localizedTitle.isEmpty())
			info.title = localizedTitle;
		return info;
	}

	// Search TMDB by title (movies and/or TV). preferTv flips which endpoint is
	// tried first — used when path heuristics suggest a series (S01E02, Season, …).
	TmdbInfo fetchTmdbInfoByTitle(const QString& title, int yearHint = 0, bool preferTv = false)
	{
		auto tryMovieSearch = [&](int year) -> TmdbInfo {
			const QByteArray encoded = QUrl::toPercentEncoding(title);
			QString urlStr = QStringLiteral(
			    "https://api.themoviedb.org/3/search/movie?query=%1&api_key=%2")
			    .arg(QString::fromLatin1(encoded), m_tmdbApiKey);
			if (year > 0)
				urlStr += QStringLiteral("&year=%1").arg(year);

			QByteArray data;
			if (!fetchHttp(QUrl(urlStr), data)) return {};

			const QJsonArray results = QJsonDocument::fromJson(data)["results"].toArray();
			if (results.isEmpty()) return {};

			QJsonObject obj;
			if (year > 0) {
				for (const QJsonValue& v : results) {
					const int ry = v.toObject()["release_date"].toString().left(4).toInt();
					if (ry == year) { obj = v.toObject(); break; }
				}
			}
			if (obj.isEmpty())
				obj = results.first().toObject();

			const int tmdbId = obj["id"].toInt();
			QString imdbId;
			{
				QByteArray extData;
				if (fetchHttp(QUrl(QStringLiteral(
				        "https://api.themoviedb.org/3/movie/%1/external_ids?api_key=%2")
				        .arg(tmdbId).arg(m_tmdbApiKey)), extData))
					imdbId = QJsonDocument::fromJson(extData)["imdb_id"].toString();
			}
			if (imdbId.isEmpty()) return {};
			return tmdbInfoFromMovieJson(obj, imdbId);
		};

		auto tryTvSearch = [&](int year) -> TmdbInfo {
			const QByteArray encoded = QUrl::toPercentEncoding(title);
			QString urlStr = QStringLiteral(
			    "https://api.themoviedb.org/3/search/tv?query=%1&api_key=%2")
			    .arg(QString::fromLatin1(encoded), m_tmdbApiKey);
			if (year > 0)
				urlStr += QStringLiteral("&first_air_date_year=%1").arg(year);

			QByteArray data;
			if (!fetchHttp(QUrl(urlStr), data)) return {};

			const QJsonArray results = QJsonDocument::fromJson(data)["results"].toArray();
			if (results.isEmpty()) return {};

			QJsonObject obj;
			if (year > 0) {
				for (const QJsonValue& v : results) {
					const int ry = v.toObject()["first_air_date"].toString().left(4).toInt();
					if (ry == year) { obj = v.toObject(); break; }
				}
			}
			if (obj.isEmpty())
				obj = results.first().toObject();

			const int tmdbId = obj["id"].toInt();
			QString imdbId;
			{
				QByteArray extData;
				if (fetchHttp(QUrl(QStringLiteral(
				        "https://api.themoviedb.org/3/tv/%1/external_ids?api_key=%2")
				        .arg(tmdbId).arg(m_tmdbApiKey)), extData))
					imdbId = QJsonDocument::fromJson(extData)["imdb_id"].toString();
			}
			if (imdbId.isEmpty()) return {};
			return tmdbInfoFromTvJson(obj, imdbId);
		};

		auto runPrimary = [&](bool tv) -> TmdbInfo {
			TmdbInfo info = yearHint > 0
			    ? (tv ? tryTvSearch(yearHint) : tryMovieSearch(yearHint))
			    : TmdbInfo{};
			if (info.imdbId.isEmpty())
				info = tv ? tryTvSearch(0) : tryMovieSearch(0);
			return info;
		};

		TmdbInfo info = runPrimary(preferTv);
		if (info.imdbId.isEmpty())
			info = runPrimary(!preferTv);
		if (info.imdbId.isEmpty())
			return info;

		const QString localizedTitle =
		    fetchLocalizedTitle(info.tmdbId, info.isTv, preferredIso1Languages());
		if (!localizedTitle.isEmpty())
			info.title = localizedTitle;
		return info;
	}

	// Path/filename heuristics that strongly suggest a TV episode rather than a movie.
	static bool pathLooksLikeTv(const QString& path)
	{
		static const QRegularExpression re(
		    QStringLiteral(R"((?i)(?:[\\/]s\d{1,2}e\d{1,3}\b|s\d{1,2}e\d{1,3}|season[\s._-]?\d{1,2}|episode[\s._-]?\d{1,3}))"));
		return re.match(path).hasMatch();
	}

	// Derive the best human-readable search title and an optional year hint from
	// the file's location and embedded metadata, mirroring the UI's suggestion logic.
	static std::pair<QString, int> bestTitleAndYear(const FileRecord& file)
	{
		auto parseYear = [](const QString& s) -> std::pair<QString, int> {
			static const QRegularExpression yearRe(R"(\b((?:19|20)\d{2})\b)");
			const auto m = yearRe.match(s);
			if (m.hasMatch())
				return { s.left(m.capturedStart()).simplified(), m.captured(1).toInt() };
			return { s.simplified(), 0 };
		};

		static const QStringList kVideoExts{
		    "*.mkv","*.mp4","*.avi","*.m4v","*.mov","*.wmv","*.ts","*.m2ts","*.mpg","*.mpeg"};
		const QDir folder = QFileInfo(file.path).dir();
		const int videoCount = folder.entryList(kVideoExts, QDir::Files).count();

		if (videoCount == 1)
			return parseYear(folder.dirName());

		if (!file.containerTitle.isEmpty()) {
			const QString stemmed = QFileInfo(file.path).completeBaseName();
			if (file.containerTitle.compare(stemmed, Qt::CaseInsensitive) != 0)
				return parseYear(file.containerTitle);
		}

		// Filename fallback: replace dots/underscores.
		QString name = QFileInfo(file.filename).completeBaseName();
		name.replace('.', ' ').replace('_', ' ');
		return parseYear(name);
	}

	// ── HTTP helpers ──────────────────────────────────────────────────────────

	bool fetchHttp(const QUrl& url, QByteArray& out)
	{
		// Once shutdown has been requested, don't start further requests — keeps
		// stop() from having to abort a growing chain of them one at a time.
		if (m_stopping) return false;

		QNetworkRequest req(url);
		req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		                 QNetworkRequest::NoLessSafeRedirectPolicy);
		// Bounds how long a stuck connection (dead route, hung TLS handshake, ...)
		// can block this thread's nested event loop below — without this, a truly
		// wedged request could hold the thread past the shutdown wait in
		// PosterManager::stopWorkerPool(), which used to fall back to
		// QThread::terminate() in that case: forcibly killing a thread mid
		// network I/O, which is what caused the "Cannot send events to objects
		// owned by a different thread" crash on close.
		req.setTransferTimeout(15000);
		m_currentReply = m_nam->get(req);

		QEventLoop loop;
		connect(m_currentReply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		loop.exec();

		QNetworkReply* reply = m_currentReply;
		m_currentReply = nullptr;
		if (!reply) return false;   // aborted before loop started (extremely unlikely)

		const bool ok = (reply->error() == QNetworkReply::NoError);
		if (ok) out = reply->readAll();
		else if (reply->error() != QNetworkReply::OperationCanceledError)
			qWarning() << "PosterWorker HTTP error:" << reply->errorString() << url;
		reply->deleteLater();
		return ok;
	}

	bool downloadImage(const QString& url, const QString& outputPath,
	                   QByteArray* outBytes = nullptr)
	{
		QByteArray data;
		if (!fetchHttp(QUrl(url), data) || data.isEmpty()) return false;
		QFile f(outputPath);
		if (!f.open(QIODevice::WriteOnly)) return false;
		f.write(data);
		if (outBytes) *outBytes = std::move(data);
		return true;
	}

	PosterWorkQueue*       m_queue        = nullptr;
	QNetworkAccessManager* m_nam          = nullptr;
	QNetworkReply*         m_currentReply = nullptr;
	QString                m_tmdbApiKey;
	bool                   m_writeNfoFiles = false;
	QStringList            m_understoodLanguages;
	int                    m_retryCooldownDays = 7;
	QString                m_posterTmdbSize;   // see tmdbPosterSizeForDpr()
	QString                m_cacheDir;
	QString                m_fanartDir;
	bool                   m_stopping   = false;
	bool                   m_processing = false;
};

// ── PosterManager ─────────────────────────────────────────────────────────────

PosterManager& PosterManager::instance()
{
	static PosterManager s;
	return s;
}

PosterManager::PosterManager(QObject* parent)
	: QObject(parent)
{}

void PosterManager::start(const QString& tmdbApiKey)
{
	if (!m_workers.isEmpty()) return;

	m_tmdbApiKey = tmdbApiKey;
	m_parallelWorkers = qBound(1,
	    AppSettings::instance().value(QStringLiteral("poster/parallelWorkers"), 4).toInt(),
	    12);

	// Fixed for the process's lifetime — card posters aren't resized at runtime,
	// so there's nothing to react to if the display scale changes later.
	const QScreen* screen = QGuiApplication::primaryScreen();
	m_posterTmdbSize = tmdbPosterSizeForDpr(screen ? screen->devicePixelRatio() : 1.0);

	resetMissingPosterMediaInDb();

	m_queue = new PosterWorkQueue(this);

	// Persistent main-thread NAM used for fast direct poster fetches in refresh().
	// connectToHostEncrypted pre-warms the TLS session with image.tmdb.org so the
	// first user-triggered download doesn't pay the Windows TLS handshake cost (~5 s).
	m_nam = new QNetworkAccessManager(this);
	if (!tmdbApiKey.isEmpty())
		m_nam->connectToHostEncrypted(QStringLiteral("image.tmdb.org"));

	startWorkerPool();

	if (!tmdbApiKey.isEmpty())
		m_queue->enqueueFile(-1);
}

void PosterManager::startWorkerPool()
{
	while (m_workers.size() < m_parallelWorkers) {
		auto* thread = new QThread(this);
		auto* worker = new PosterWorker(m_queue, m_tmdbApiKey, m_writeNfoFiles,
		                                m_understoodLanguages, m_retryCooldownDays,
		                                m_posterTmdbSize);
		worker->moveToThread(thread);

		connect(thread, &QThread::started,  worker, &PosterWorker::startProcessing);
		connect(thread, &QThread::finished, worker, &QObject::deleteLater);

		connect(worker, &PosterWorker::posterReady,
		        this,     &PosterManager::posterReady);
		connect(worker, &PosterWorker::fanartReady,
		        this,     &PosterManager::fanartReady);
		connect(worker, &PosterWorker::tmdbDataReady,
		        this,     &PosterManager::tmdbDataReady);
		connect(worker, &PosterWorker::imdbIdSaved,
		        this,     &PosterManager::imdbIdSaved);
		connect(worker, &PosterWorker::tmdbIdSaved,
		        this,     &PosterManager::tmdbIdSaved);
		connect(worker, &PosterWorker::fileProcessed,
		        this, [this](qint64 fileId) {
			if (!m_batchActive || !m_batchIds.remove(fileId)) return;
			++m_batchDone;
			emit batchProgressChanged(m_batchDone, m_batchTotal);
			if (m_batchIds.isEmpty()) {
				m_batchActive = false;
				emit batchFinished(m_batchDone, m_batchTotal, false);
			}
		});

		connect(this, &PosterManager::workerTmdbKeyChanged,
		        worker, &PosterWorker::setTmdbApiKey,
		        Qt::QueuedConnection);
		connect(this, &PosterManager::workerWriteNfoChanged,
		        worker, &PosterWorker::setWriteNfoFiles,
		        Qt::QueuedConnection);
		connect(this, &PosterManager::workerUnderstoodLanguagesChanged,
		        worker, &PosterWorker::setUnderstoodLanguages,
		        Qt::QueuedConnection);
		connect(this, &PosterManager::workerRetryCooldownChanged,
		        worker, &PosterWorker::setRetryCooldownDays,
		        Qt::QueuedConnection);
		connect(this, &PosterManager::workerStop,
		        worker, &PosterWorker::stop,
		        Qt::QueuedConnection);

		m_workers.append({thread, worker});
		thread->start();
	}
}

void PosterManager::stopWorkerPool(bool wait)
{
	if (m_workers.isEmpty()) return;

	logRestartDebug(QStringLiteral("PosterManager::stopWorkerPool: stopping %1 worker(s)")
	                    .arg(m_workers.size()));

	// Ask every worker to stop; each one quits its own thread's event loop once
	// it's confirmed safe to (see PosterWorker::stop()/tryProcessNext()). Do NOT
	// call slot.thread->quit() here — QThread::quit()/exit() only affects
	// whichever event loop is currently innermost for that thread, so calling it
	// from this (unrelated) thread while a worker is mid-network-call would quit
	// its nested fetchHttp() wait instead of the real loop, leaving the real loop
	// running forever. That used to make the wait() below time out and fall back
	// to terminate() — forcibly killing a thread mid network I/O, which crashed
	// with "Cannot send events to objects owned by a different thread".
	emit workerStop();
	int i = 0;
	for (WorkerSlot& slot : m_workers) {
		++i;
		if (!slot.thread) continue;
		QElapsedTimer t; t.start();
		if (wait && !slot.thread->wait(20000)) {
			qWarning() << "PosterManager: worker thread did not stop in time; abandoning it";
			logRestartDebug(QStringLiteral("PosterManager::stopWorkerPool: worker %1 did NOT stop after %2 ms — abandoned, still running")
			                    .arg(i).arg(t.elapsed()));
		} else {
			logRestartDebug(QStringLiteral("PosterManager::stopWorkerPool: worker %1 stopped after %2 ms")
			                    .arg(i).arg(t.elapsed()));
		}
		slot.thread = nullptr;
		slot.worker = nullptr;
	}
	m_workers.clear();
}

void PosterManager::stop()
{
	stopWorkerPool(true);
}

void PosterManager::setParallelWorkers(int count)
{
	count = qBound(1, count, 12);
	if (count == m_parallelWorkers) {
		if (m_workers.isEmpty())
			AppSettings::instance().setValue(QStringLiteral("poster/parallelWorkers"), count);
		return;
	}

	m_parallelWorkers = count;
	AppSettings::instance().setValue(QStringLiteral("poster/parallelWorkers"), count);

	if (m_workers.isEmpty()) return;

	startWorkerPool();

	while (m_workers.size() > m_parallelWorkers) {
		WorkerSlot slot = m_workers.takeLast();
		if (slot.worker)
			QMetaObject::invokeMethod(slot.worker, "stop", Qt::QueuedConnection);
		// slot.thread->quit() deliberately omitted here — see the comment in
		// stopWorkerPool() above the analogous wait() call.
		if (slot.thread && !slot.thread->wait(20000))
			qWarning() << "PosterManager: worker thread did not stop in time; abandoning it";
	}
}

void PosterManager::setTmdbApiKey(const QString& key)
{
	if (key == m_tmdbApiKey) return;
	const bool wasEmpty = m_tmdbApiKey.isEmpty();
	m_tmdbApiKey = key;

	if (wasEmpty && !key.isEmpty())
		DatabaseManager::instance().resetNoPosterRecords();

	emit workerTmdbKeyChanged(key);
	if (!key.isEmpty() && m_queue)
		m_queue->enqueueFile(-1);
}

void PosterManager::setWriteNfoFiles(bool enabled)
{
	if (enabled == m_writeNfoFiles) return;
	m_writeNfoFiles = enabled;
	emit workerWriteNfoChanged(enabled);
}

void PosterManager::setUnderstoodLanguages(const QStringList& languages)
{
	if (languages == m_understoodLanguages) return;
	m_understoodLanguages = languages;
	emit workerUnderstoodLanguagesChanged(languages);
}

void PosterManager::setRetryCooldownDays(int days)
{
	days = qMax(0, days);
	if (days == m_retryCooldownDays) return;
	m_retryCooldownDays = days;
	emit workerRetryCooldownChanged(days);
}

void PosterManager::enqueue(qint64 fileId)
{
	if (m_queue)
		m_queue->enqueueFile(fileId);
}

void PosterManager::enqueueBatch(const QList<qint64>& fileIds)
{
	if (m_queue)
		m_queue->enqueueBatch(fileIds);
}

void PosterManager::refresh(qint64 fileId, const QString& posterPath,
                             const QByteArray& imageData, const QString& imdbIdHint,
                             double voteAverage, int voteCount,
                             const QString& fanartTmdbPath, int tmdbId)
{
	const auto rec = DatabaseManager::instance().posterForFile(fileId);
	const QString imdbId = !imdbIdHint.isEmpty() ? imdbIdHint
	                     : (rec ? rec->imdbId : QString());

	// Persist the imdb_id immediately so the card IMDb button appears without
	// waiting for the async poster download.  The signal lets the model update
	// only the affected row, avoiding a full reload that would clear selection.
	if (!imdbIdHint.isEmpty()) {
		DatabaseManager::instance().updateImdbId(fileId, imdbIdHint);
		emit imdbIdSaved(fileId, imdbIdHint);
	}
	if (tmdbId > 0) {
		DatabaseManager::instance().updateTmdbId(fileId, tmdbId);
		emit tmdbIdSaved(fileId, tmdbId);
	}

	const QString cacheDir  = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	                          + "/posters";
	const QString fanartDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	                          + "/fanart";
	const QString fanartStem = imdbId.isEmpty() ? QString::number(fileId) : imdbId;
	const QString fanartOut  = fanartDir + "/" + fanartStem + ".jpg";

	if (rec && !rec->imagePath.isEmpty())
		QFile::remove(rec->imagePath);
	// Fanart must be invalidated too, or a local "-fanart.<ext>" file dropped into the
	// movie folder after the TMDB backdrop was already cached at this path will be
	// silently skipped by cacheLocalImage()'s exists-check on every future refresh.
	if (rec && !rec->fanartPath.isEmpty())
		QFile::remove(rec->fanartPath);

	// Downloads the user-selected backdrop at full TMDB resolution and emits
	// fanartReady. Only fires when the caller provided a TMDB fanart path (i.e.
	// the user picked a specific backdrop in the dialog). Captures all state by
	// value so it is safe to copy into async reply lambdas.
	auto schedFanart = [this, fanartTmdbPath, fanartDir, fanartOut, fileId]() {
		if (fanartTmdbPath.isEmpty() || !m_nam) return;
		QDir().mkpath(fanartDir);
		QNetworkRequest req(QUrl(QStringLiteral("https://image.tmdb.org/t/p/original%1").arg(fanartTmdbPath)));
		req.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");
		req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		                 QNetworkRequest::NoLessSafeRedirectPolicy);
		QNetworkReply* r = m_nam->get(req);
		connect(r, &QNetworkReply::finished, this, [this, r, fileId, fanartOut]() {
			r->deleteLater();
			if (r->error() != QNetworkReply::NoError) return;
			const QByteArray bytes = r->readAll();
			if (bytes.isEmpty()) return;
			QFile f(fanartOut);
			if (!f.open(QIODevice::WriteOnly)) return;
			f.write(bytes);
			f.close();
			const auto existing = DatabaseManager::instance().posterForFile(fileId);
			PosterRecord pr = existing ? *existing : PosterRecord{};
			pr.fileId     = fileId;
			pr.fanartPath = fanartOut;
			DatabaseManager::instance().upsertPosterRecord(pr);
			QImage img;
			img.loadFromData(bytes);
			if (!img.isNull() && img.height() > 300) {
				const int y = (img.height() - 300) / 2;
				img = img.copy(0, y, img.width(), 300);
			}
			emit fanartReady(fileId, fanartOut, img);
		});
	};

	// Helper: write bytes to disk and notify the model.
	const auto writePoster = [&](const QByteArray& bytes) -> bool {
		if (bytes.isEmpty()) return false;
		QDir().mkpath(cacheDir);
		const QString stem    = imdbId.isEmpty() ? QString::number(fileId) : imdbId;
		const QString outPath = cacheDir + "/" + stem + ".jpg";
		QFile f(outPath);
		if (!f.open(QIODevice::WriteOnly)) return false;
		f.write(bytes);
		f.close();
		PosterRecord pr;
		pr.fileId       = fileId;
		pr.source       = "tmdb";
		pr.status       = "done";
		pr.imagePath    = outPath;
		pr.imdbId       = imdbId;
		pr.fetchedAt    = QDateTime::currentMSecsSinceEpoch();
		pr.voteAverage  = voteAverage;
		pr.voteCount    = voteCount;
		DatabaseManager::instance().upsertPosterRecord(pr);
		emit posterReady(fileId, outPath);
		if (fanartTmdbPath.isEmpty() && m_queue)
			m_queue->enqueueFile(fileId);
		return true;
	};

	// Path 1 — bytes already available (dialog thumbnail was ready): write directly, instant.
	if (!imageData.isEmpty()) {
		if (writePoster(imageData)) {
			schedFanart();
			return;
		}
	}

	// Path 2 — poster_path known but no bytes: download asynchronously on the persistent
	// main-thread NAM whose TLS session with image.tmdb.org is pre-warmed at startup.
	// Skips both the worker queue and the /find API round-trip — typically <200 ms.
	if (!posterPath.isEmpty() && m_nam) {
		const QUrl url(QStringLiteral("https://image.tmdb.org/t/p/%1%2").arg(m_posterTmdbSize, posterPath));
		QNetworkRequest req(url);
		req.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");
		req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		                 QNetworkRequest::NoLessSafeRedirectPolicy);
		QNetworkReply* reply = m_nam->get(req);
		connect(reply, &QNetworkReply::finished, this,
		        [this, reply, fileId, imdbId, cacheDir, voteAverage, voteCount,
		         fanartTmdbPath, schedFanart]() mutable {
			if (reply->error() == QNetworkReply::NoError) {
				const QByteArray bytes = reply->readAll();
				if (!bytes.isEmpty()) {
					QDir().mkpath(cacheDir);
					const QString stem    = imdbId.isEmpty() ? QString::number(fileId) : imdbId;
					const QString outPath = cacheDir + "/" + stem + ".jpg";
					QFile f(outPath);
					if (f.open(QIODevice::WriteOnly)) {
						f.write(bytes);
						f.close();
						PosterRecord pr;
						pr.fileId      = fileId;
						pr.source      = "tmdb";
						pr.status      = "done";
						pr.imagePath   = outPath;
						pr.imdbId      = imdbId;
						pr.fetchedAt   = QDateTime::currentMSecsSinceEpoch();
						pr.voteAverage = voteAverage;
						pr.voteCount   = voteCount;
						DatabaseManager::instance().upsertPosterRecord(pr);
						emit posterReady(fileId, outPath);
						if (fanartTmdbPath.isEmpty()) {
							if (m_queue)
								m_queue->enqueueFile(fileId);
						} else {
							schedFanart();
						}
					}
				}
			} else {
				qWarning() << "PosterManager: direct fetch failed:" << reply->errorString();
			}
			reply->deleteLater();
		});
		return;
	}

	// Path 3 — no poster_path known: hand off to the worker pool for a full TMDB lookup.
	// Covers "Refresh Poster" from the context menu when no .nfo exists yet.
	DatabaseManager::instance().resetPosterForFile(fileId);
	if (m_queue)
		m_queue->enqueueFile(fileId);
}

void PosterManager::refreshBatch(const QList<qint64>& fileIds)
{
	if (fileIds.isEmpty()) return;

	int added = 0;
	for (qint64 fileId : fileIds) {
		if (!m_batchIds.contains(fileId)) {
			m_batchIds.insert(fileId);
			++added;
		}
	}
	m_batchTotal += added;
	if (!m_batchActive) {
		m_batchDone   = 0;
		m_batchActive = true;
	}
	emit batchProgressChanged(m_batchDone, m_batchTotal);

	for (qint64 fileId : fileIds)
		refresh(fileId);
}

void PosterManager::cancelBatch()
{
	if (!m_batchActive) return;
	if (m_queue)
		m_queue->removeIds(m_batchIds);
	const int done = m_batchDone, total = m_batchTotal;
	m_batchIds.clear();
	m_batchTotal  = 0;
	m_batchDone   = 0;
	m_batchActive = false;
	emit batchFinished(done, total, true);
}

} // namespace Mc

#include "PosterManager.moc"
