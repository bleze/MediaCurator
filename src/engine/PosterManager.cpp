#include "engine/PosterManager.h"
#include "core/DatabaseManager.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QDebug>

namespace Mc {

// ── PosterWorker ──────────────────────────────────────────────────────────────

class PosterWorker : public QObject
{
	Q_OBJECT
public:
	explicit PosterWorker(const QString& tmdbApiKey, QObject* parent = nullptr)
	    : QObject(parent)
	    , m_tmdbApiKey(tmdbApiKey)
	{
		const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
		m_cacheDir = dataDir + "/posters";
		QDir().mkpath(m_cacheDir);
	}

public slots:
	void startProcessing()
	{
		m_nam   = new QNetworkAccessManager(this);
		m_timer = new QTimer(this);
		m_timer->setSingleShot(false);
		m_timer->setInterval(0);
		connect(m_timer, &QTimer::timeout, this, &PosterWorker::processNext);

		if (!m_tmdbApiKey.isEmpty()) {
			loadPending();
			if (!m_queue.isEmpty()) m_timer->start();
		}
	}

	void stop()
	{
		m_stopping = true;
		if (m_timer) m_timer->stop();
	}

	void setTmdbApiKey(const QString& key) { m_tmdbApiKey = key; }

	void enqueueFile(qint64 fileId)
	{
		if (m_stopping) return;
		if (fileId == -1) {
			// Triggered when the TMDB key is set for the first time — load every
			// file that is still pending or was previously marked "no_poster".
			loadPending();
		} else if (fileId > 0 && !m_queue.contains(fileId)) {
			m_queue.prepend(fileId);
		}
		if (m_timer && !m_timer->isActive() && !m_queue.isEmpty())
			m_timer->start();
	}

signals:
	void posterReady(qint64 fileId, QString imagePath);

private slots:
	void processNext()
	{
		if (m_stopping) { m_timer->stop(); return; }

		if (m_queue.isEmpty()) {
			loadPending();
			if (m_queue.isEmpty()) { m_timer->stop(); return; }
		}

		const qint64 fileId = m_queue.takeFirst();
		processFile(fileId);
	}

private:
	void loadPending()
	{
		for (qint64 id : DatabaseManager::instance().fileIdsNeedingPosters())
			if (!m_queue.contains(id)) m_queue.append(id);
	}

	void processFile(qint64 fileId)
	{
		const auto fileOpt = DatabaseManager::instance().fileById(fileId);
		if (!fileOpt) return;

		const QString filePath  = fileOpt->path;
		QString       imdbId    = findNfoImdbId(filePath);
		const QString baseStem  = QFileInfo(filePath).completeBaseName();

		const auto existing = DatabaseManager::instance().posterForFile(fileId);

		// Fall back to the DB-stored IMDb ID if the NFO file doesn't have one.
		if (imdbId.isEmpty() && existing && !existing->imdbId.isEmpty())
			imdbId = existing->imdbId;

		// Skip files that are already fully done (poster + rating).
		// If the poster is done but rating is missing and we have an imdbId,
		// fall through to fetch the rating from TMDB.
		if (existing && existing->status == "done"
		    && !existing->imagePath.isEmpty()
		    && QFile::exists(existing->imagePath)) {
			if (existing->voteAverage > 0.0 || imdbId.isEmpty() || m_tmdbApiKey.isEmpty())
				return;
			// Rating-only fetch — poster already on disk.
			const TmdbInfo info = fetchTmdbInfo(imdbId);
			if (info.voteAverage > 0.0) {
				PosterRecord pr = *existing;
				pr.voteAverage = info.voteAverage;
				pr.voteCount   = info.voteCount;
				DatabaseManager::instance().upsertPosterRecord(pr);
			}
			return;
		}

		PosterRecord rec;
		rec.fileId    = fileId;
		rec.imdbId    = imdbId;
		rec.fetchedAt = QDateTime::currentMSecsSinceEpoch();

		// If the DB was deleted but the posters folder still exists, reuse
		// whatever file is already on disk rather than re-downloading.
		const QString byImdb = imdbId.isEmpty() ? QString() : (m_cacheDir + "/" + imdbId + ".jpg");
		const QString byBase = m_cacheDir + "/" + baseStem + ".jpg";
		const QString cached = (!byImdb.isEmpty() && QFile::exists(byImdb)) ? byImdb
		                     : (QFile::exists(byBase) ? byBase : QString());

		if (!cached.isEmpty()) {
			rec.source    = "tmdb";
			rec.status    = "done";
			rec.imagePath = cached;
			// Try to fetch rating while we're here — no image download needed,
			// just a lightweight /find API call.
			if (!imdbId.isEmpty() && !m_tmdbApiKey.isEmpty()) {
				const TmdbInfo info = fetchTmdbInfo(imdbId);
				rec.voteAverage = info.voteAverage;
				rec.voteCount   = info.voteCount;
			}
			DatabaseManager::instance().upsertPosterRecord(rec);
			emit posterReady(fileId, cached);
			return;
		}

		// Nothing cached — fetch from TMDB if we have an API key.
		// Without a key, mark as "no_poster" so the queue drains cleanly;
		// setTmdbApiKey() calls resetNoPosterRecords() which resets these to
		// "pending" when the user later adds a key.
		if (m_tmdbApiKey.isEmpty()) {
			rec.status = "no_poster";
			DatabaseManager::instance().upsertPosterRecord(rec);
			return;
		}

		QString imagePath;
		if (!imdbId.isEmpty()) {
			const TmdbInfo info = fetchTmdbInfo(imdbId);
			if (!info.posterPath.isEmpty()) {
				const QString outPath = m_cacheDir + "/" + imdbId + ".jpg";
				if (downloadImage(
				        QStringLiteral("https://image.tmdb.org/t/p/w92%1").arg(info.posterPath),
				        outPath))
					imagePath = outPath;
			}
			rec.voteAverage = info.voteAverage;
			rec.voteCount   = info.voteCount;
		}

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

	// ── TMDB lookup ───────────────────────────────────────────────────────────

	struct TmdbInfo { QString posterPath; double voteAverage = 0.0; int voteCount = 0; };

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
		return { obj["poster_path"].toString(),
		         obj["vote_average"].toDouble(),
		         obj["vote_count"].toInt() };
	}

	// ── HTTP helpers ──────────────────────────────────────────────────────────

	bool fetchHttp(const QUrl& url, QByteArray& out)
	{
		QNetworkRequest req(url);
		req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		                 QNetworkRequest::NoLessSafeRedirectPolicy);
		QNetworkReply* reply = m_nam->get(req);

		QEventLoop loop;
		connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		loop.exec();

		const bool ok = (reply->error() == QNetworkReply::NoError);
		if (ok) out = reply->readAll();
		else    qWarning() << "PosterWorker HTTP error:" << reply->errorString() << url;
		reply->deleteLater();
		return ok;
	}

	bool downloadImage(const QString& url, const QString& outputPath)
	{
		QByteArray data;
		if (!fetchHttp(QUrl(url), data) || data.isEmpty()) return false;
		QFile f(outputPath);
		if (!f.open(QIODevice::WriteOnly)) return false;
		f.write(data);
		return true;
	}

	QNetworkAccessManager* m_nam      = nullptr;
	QTimer*                m_timer    = nullptr;
	QList<qint64>          m_queue;
	QString                m_tmdbApiKey;
	QString                m_cacheDir;
	bool                   m_stopping  = false;
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
	if (m_thread) return;

	m_tmdbApiKey = tmdbApiKey;

	// Persistent main-thread NAM used for fast direct poster fetches in refresh().
	// connectToHostEncrypted pre-warms the TLS session with image.tmdb.org so the
	// first user-triggered download doesn't pay the Windows TLS handshake cost (~5 s).
	m_nam = new QNetworkAccessManager(this);
	if (!tmdbApiKey.isEmpty())
		m_nam->connectToHostEncrypted(QStringLiteral("image.tmdb.org"));

	m_worker     = new PosterWorker(tmdbApiKey);
	m_thread     = new QThread(this);
	m_worker->moveToThread(m_thread);

	connect(m_thread, &QThread::started,  m_worker, &PosterWorker::startProcessing);
	connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

	connect(m_worker, &PosterWorker::posterReady,
	        this,     &PosterManager::posterReady);

	connect(this, &PosterManager::workerTmdbKeyChanged,
	        m_worker, &PosterWorker::setTmdbApiKey,
	        Qt::QueuedConnection);
	connect(this, &PosterManager::workerEnqueueFile,
	        m_worker, &PosterWorker::enqueueFile,
	        Qt::QueuedConnection);
	connect(this, &PosterManager::workerStop,
	        m_worker, &PosterWorker::stop,
	        Qt::QueuedConnection);

	m_thread->start();
}

void PosterManager::stop()
{
	if (!m_thread || !m_thread->isRunning()) return;
	emit workerStop();
	m_thread->quit();
	if (!m_thread->wait(5000)) {
		m_thread->terminate();
		m_thread->wait(1000);
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
	if (!key.isEmpty() && m_worker)
		emit workerEnqueueFile(-1);
}

void PosterManager::enqueue(qint64 fileId)
{
	emit workerEnqueueFile(fileId);
}

void PosterManager::refresh(qint64 fileId, const QString& posterPath,
                             const QByteArray& imageData, const QString& imdbIdHint,
                             double voteAverage, int voteCount)
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

	const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	                         + "/posters";

	if (rec && !rec->imagePath.isEmpty())
		QFile::remove(rec->imagePath);

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
		return true;
	};

	// Path 1 — bytes already available (dialog thumbnail was ready): write directly, instant.
	if (!imageData.isEmpty()) {
		if (writePoster(imageData)) return;
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
		        [this, reply, fileId, imdbId, cacheDir, voteAverage, voteCount]() {
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
					}
				}
			} else {
				qWarning() << "PosterManager: direct fetch failed:" << reply->errorString();
			}
			reply->deleteLater();
		});
		return;
	}

	// Path 3 — no poster_path known: hand off to the worker for a full TMDB lookup.
	// Covers "Refresh Poster" from the context menu when no .nfo exists yet.
	DatabaseManager::instance().resetPosterForFile(fileId);
	emit workerEnqueueFile(fileId);
}

} // namespace Mc

#include "PosterManager.moc"
