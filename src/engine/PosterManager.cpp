#include "engine/PosterManager.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"
#include "scanner/NfoParser.h"

#include <QDateTime>
#include <QUrl>
#include <utility>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QMutex>
#include <QThread>
#include <QDebug>

#include <optional>

namespace Mc {

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
	explicit PosterWorker(PosterWorkQueue* queue, const QString& tmdbApiKey, QObject* parent = nullptr)
	    : QObject(parent)
	    , m_queue(queue)
	    , m_tmdbApiKey(tmdbApiKey)
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
			m_currentReply->abort();
			m_currentReply = nullptr;
		}
	}

	void setTmdbApiKey(const QString& key) { m_tmdbApiKey = key; }

signals:
	void posterReady(qint64 fileId, QString imagePath);
	void fanartReady(qint64 fileId, QString fanartPath, QImage image);
	void tmdbDataReady(qint64 fileId, QString title, int year, double rating);
	void imdbIdSaved(qint64 fileId, QString imdbId);

private slots:
	void tryProcessNext()
	{
		if (m_stopping || m_processing)
			return;

		const auto fileId = m_queue->takeNext();
		if (!fileId)
			return;

		m_processing = true;
		processFile(*fileId);
		m_processing = false;

		if (!m_stopping)
			QMetaObject::invokeMethod(this, &PosterWorker::tryProcessNext, Qt::QueuedConnection);
	}

private:
	void processFile(qint64 fileId)
	{
		const auto fileOpt = DatabaseManager::instance().fileById(fileId);
		if (!fileOpt) return;

		const QString filePath = fileOpt->path;
		QString       imdbId   = findNfoImdbId(filePath);
		const QString baseStem = QFileInfo(filePath).completeBaseName();

		const auto existing = DatabaseManager::instance().posterForFile(fileId);

		// Fall back to the DB-stored IMDb ID if the NFO file doesn't have one.
		if (imdbId.isEmpty() && existing && !existing->imdbId.isEmpty())
			imdbId = existing->imdbId;

		// A "<basename>-poster.<ext>" / "<basename>-fanart.<ext>" file next to the
		// video takes priority over TMDB — seed the cache from it before the
		// disk-cache-reuse checks below run, so they pick it up and skip the network
		// entirely. Fanart caching needs a known imdbId (same requirement the rest
		// of this class already has for fanart, e.g. the lazy re-check just below).
		const LocalArt localArt = findLocalArt(filePath);
		cacheLocalImage(localArt.posterPath,
		                imdbId.isEmpty() ? (m_cacheDir + "/" + baseStem + ".jpg")
		                                 : (m_cacheDir + "/" + imdbId + ".jpg"));
		if (!imdbId.isEmpty())
			cacheLocalImage(localArt.fanartPath, m_fanartDir + "/" + imdbId + ".jpg");

		// Helper: persist TMDB data from a TmdbInfo result, writing .nfo for durability.
		// Also emits tmdbDataReady so the UI updates immediately without a DB round-trip.
		auto applyTmdbInfo = [&](const TmdbInfo& info, const QString& resolvedImdbId) {
			if (!info.title.isEmpty()) {
				if (fileOpt->displayTitle.isEmpty())
					DatabaseManager::instance().updateDisplayTitle(fileId, info.title, info.year);
				emit tmdbDataReady(fileId, info.title, info.year, info.voteAverage);
			}
			// Write .nfo so imdbId survives future DB deletions.
			if (!resolvedImdbId.isEmpty() && !info.title.isEmpty()
			    && !QFile::exists(NfoParser::nfoPathFor(filePath)))
				NfoParser::writeMovieNfo(filePath, resolvedImdbId, info.title, info.year);
			// Newly-discovered imdbId (title-search recovery) needs to reach the live
			// UI models too -- upsertPosterRecord() alone only updates the DB.
			if (!resolvedImdbId.isEmpty())
				emit imdbIdSaved(fileId, resolvedImdbId);
		};

		// Skip files that are already fully done (poster + rating + title).
		// If any piece is still missing and we have an imdbId, fall through to fetch it.
		if (existing && existing->status == "done"
		    && !existing->imagePath.isEmpty()
		    && QFile::exists(existing->imagePath)) {
			const bool needsRating = existing->voteAverage <= 0.0;
			const bool needsTitle  = fileOpt->displayTitle.isEmpty();
			const bool needsNfo    = !QFile::exists(NfoParser::nfoPathFor(filePath));
			bool needsFanart       = existing->fanartPath.isEmpty()
			                         || !QFile::exists(existing->fanartPath);

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

			if (!needsRating && !needsTitle && !needsNfo && !needsFanart) return;
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
				           QStringLiteral("https://image.tmdb.org/t/p/w1280%1").arg(info.backdropPath),
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
						const TmdbInfo info = fetchTmdbInfoByTitle(title, yearHint);
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
			rec.status = "no_poster";
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
				info = fetchTmdbInfoByTitle(title, yearHint);
			if (!info.imdbId.isEmpty())
				imdbId = info.imdbId;
		}

		QString imagePath;
		if (!imdbId.isEmpty() && !info.posterPath.isEmpty()) {
			const QString outPath = m_cacheDir + "/" + imdbId + ".jpg";
			if (downloadImage(
			        QStringLiteral("https://image.tmdb.org/t/p/w92%1").arg(info.posterPath),
			        outPath))
				imagePath = outPath;
		}
		if (!imdbId.isEmpty()) {
			const QString outPath = m_fanartDir + "/" + imdbId + ".jpg";
			QByteArray fanartBytes;
			const bool diskHit = QFile::exists(outPath);
			const bool dlOk    = !diskHit && !info.backdropPath.isEmpty()
			    && downloadImage(
			           QStringLiteral("https://image.tmdb.org/t/p/w1280%1").arg(info.backdropPath),
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
			rec.status = "no_poster";
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
		QString posterPath;  // "<video-basename>-poster.<ext>", if present
		QString fanartPath;  // "<video-basename>-fanart.<ext>", if present
	};

	// Mirrors ScanWorker::scanSidecarSubtitles' one-shot directory enumeration —
	// list image files in the video's folder once, then match by base filename.
	// Limited to jpg/png (the image formats Qt's bundled plugins can always
	// decode) rather than also accepting webp, which isn't bundled.
	static LocalArt findLocalArt(const QString& videoPath)
	{
		const QFileInfo videoFi(videoPath);
		const QString   baseName = videoFi.completeBaseName();
		const QDir      dir(videoFi.absolutePath());

		static const QStringList imageFilters = {"*.jpg", "*.jpeg", "*.png"};

		LocalArt result;
		for (const QFileInfo& fi : dir.entryInfoList(imageFilters, QDir::Files)) {
			const QString stem = fi.completeBaseName();
			if (result.posterPath.isEmpty()
			    && stem.compare(baseName + QLatin1String("-poster"), Qt::CaseInsensitive) == 0)
				result.posterPath = fi.absoluteFilePath();
			else if (result.fanartPath.isEmpty()
			    && stem.compare(baseName + QLatin1String("-fanart"), Qt::CaseInsensitive) == 0)
				result.fanartPath = fi.absoluteFilePath();
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
		QString title;
		int     year        = 0;
		QString imdbId;     // populated by fetchTmdbInfoByTitle; caller already has it for fetchTmdbInfo
	};

	// Look up by known IMDb ID — single API call.
	TmdbInfo fetchTmdbInfo(const QString& imdbId)
	{
		const QUrl url(QStringLiteral(
		    "https://api.themoviedb.org/3/find/%1?external_source=imdb_id&api_key=%2")
		    .arg(imdbId, m_tmdbApiKey));

		QByteArray data;
		if (!fetchHttp(url, data)) return {};

		const QJsonArray results = QJsonDocument::fromJson(data)["movie_results"].toArray();
		if (results.isEmpty()) return {};
		const QJsonObject obj = results.first().toObject();
		const int year = obj["release_date"].toString().left(4).toInt();
		return { obj["poster_path"].toString(),
		         obj["backdrop_path"].toString(),
		         obj["vote_average"].toDouble(),
		         obj["vote_count"].toInt(),
		         obj["title"].toString(),
		         year,
		         imdbId };
	}

	// Search TMDB by movie title.
	// With a year hint: take the first result whose release year matches.
	// Without a year hint (or when year-search finds no match): take TMDB's
	// top relevance-ranked result — works for the vast majority of mainstream titles.
	TmdbInfo fetchTmdbInfoByTitle(const QString& title, int yearHint = 0)
	{
		auto trySearch = [&](int year) -> TmdbInfo {
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

			// With year: take the first result whose release year matches exactly.
			// Without year: trust TMDB's popularity ranking and take the top result.
			QJsonObject obj;
			if (year > 0) {
				for (const QJsonValue& v : results) {
					const int ry = v.toObject()["release_date"].toString().left(4).toInt();
					if (ry == year) { obj = v.toObject(); break; }
				}
			}
			// No year match (or no year hint): take the top relevance-ranked result.
			if (obj.isEmpty())
				obj = results.first().toObject();

			const int resultYear = obj["release_date"].toString().left(4).toInt();
			const int tmdbId     = obj["id"].toInt();

			// Resolve IMDb ID via external_ids (second lightweight call).
			QString imdbId;
			{
				QByteArray extData;
				if (fetchHttp(QUrl(QStringLiteral(
				        "https://api.themoviedb.org/3/movie/%1/external_ids?api_key=%2")
				        .arg(tmdbId).arg(m_tmdbApiKey)), extData))
					imdbId = QJsonDocument::fromJson(extData)["imdb_id"].toString();
			}
			if (imdbId.isEmpty()) return {};

			return { obj["poster_path"].toString(),
			         obj["backdrop_path"].toString(),
			         obj["vote_average"].toDouble(),
			         obj["vote_count"].toInt(),
			         obj["title"].toString(),
			         resultYear,
			         imdbId };
		};

		// Try with year hint first; fall back to year-less search only if no hint.
		if (yearHint > 0) {
			const TmdbInfo info = trySearch(yearHint);
			if (!info.imdbId.isEmpty()) return info;
		}
		return trySearch(0);
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
		QNetworkRequest req(url);
		req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		                 QNetworkRequest::NoLessSafeRedirectPolicy);
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
		auto* worker = new PosterWorker(m_queue, m_tmdbApiKey);
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

		connect(this, &PosterManager::workerTmdbKeyChanged,
		        worker, &PosterWorker::setTmdbApiKey,
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

	emit workerStop();
	for (WorkerSlot& slot : m_workers) {
		if (!slot.thread) continue;
		slot.thread->quit();
		if (wait && !slot.thread->wait(5000)) {
			slot.thread->terminate();
			slot.thread->wait(1000);
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
		if (slot.thread) {
			slot.thread->quit();
			if (!slot.thread->wait(5000)) {
				slot.thread->terminate();
				slot.thread->wait(1000);
			}
		}
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
                             const QString& fanartTmdbPath)
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

	const QString cacheDir  = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	                          + "/posters";
	const QString fanartDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	                          + "/fanart";
	const QString fanartStem = imdbId.isEmpty() ? QString::number(fileId) : imdbId;
	const QString fanartOut  = fanartDir + "/" + fanartStem + ".jpg";

	if (rec && !rec->imagePath.isEmpty())
		QFile::remove(rec->imagePath);

	// Downloads the user-selected backdrop at w1280 and emits fanartReady.
	// Only fires when the caller provided a TMDB fanart path (i.e. the user picked
	// a specific backdrop in the dialog).  Captures all state by value so it is
	// safe to copy into async reply lambdas.
	auto schedFanart = [this, fanartTmdbPath, fanartDir, fanartOut, fileId]() {
		if (fanartTmdbPath.isEmpty() || !m_nam) return;
		QDir().mkpath(fanartDir);
		QNetworkRequest req(QUrl(QStringLiteral("https://image.tmdb.org/t/p/w1280%1").arg(fanartTmdbPath)));
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
		const QUrl url(QStringLiteral("https://image.tmdb.org/t/p/w92%1").arg(posterPath));
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

} // namespace Mc

#include "PosterManager.moc"
