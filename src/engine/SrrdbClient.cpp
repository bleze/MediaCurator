#include "engine/SrrdbClient.h"
#include "core/DatabaseManager.h"
#include "scanner/NfoParser.h"

#include <QDateTime>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QUrl>

Q_LOGGING_CATEGORY(lcSrrdb, "mc.srrdb")

namespace {

// A resolution/quality tier string, or empty if none detected. UHD/4K fold
// into the 2160p tier — "COMPLETE.UHD.BLURAY"-style full-disc releases often
// carry no explicit "2160p" token.
QString resolutionTier(const QString& name)
{
	static const QRegularExpression resRe(
		R"(\b(480p|576p|720p|1080p|2160p|4320p)\b)", QRegularExpression::CaseInsensitiveOption);
	const auto m = resRe.match(name);
	if (m.hasMatch()) return m.captured(1).toLower();

	static const QRegularExpression uhdRe(R"(\b(UHD|4K)\b)", QRegularExpression::CaseInsensitiveOption);
	if (uhdRe.match(name).hasMatch()) return QStringLiteral("2160p");

	return {};
}

// Every genuine scene movie/TV release name carries a resolution, source, or
// codec tag — nothing else srrDB indexes (music, ebooks, audiobooks) does.
// srrDB's keyword search is a loose wildcard match, so a common word plus a
// coincidental hit against a release's OWN scene-upload-year suffix (e.g. an
// unrelated album tagged "-WEB-2026-GROUP") otherwise pollutes results with
// things that were never real candidates — confirmed live against srrDB
// while diagnosing why "The Invite" (2026) returned nothing but albums/ebooks.
bool looksLikeVideoRelease(const QString& name)
{
	static const QRegularExpression re(
		R"(\b(480p|576p|720p|1080p|2160p|4320p|UHD|4K|BluRay|BDRip|BRRip|WEB[-.]?DL|WEBRip|WEB|HDTV|DVDRip|DVDR|HDRip|x264|x265|[hH]264|[hH]265|HEVC|AVC|XviD|DivX)\b)",
		QRegularExpression::CaseInsensitiveOption);
	return re.match(name).hasMatch();
}

} // namespace

namespace Mc {

namespace {

// Result of resolving scene-release candidates for a video file — shared by
// both SceneNfoDownloadWorker (which shows an interactive picker on ambiguity)
// and SceneNfoWorker (the background auto-download queue, which just skips a
// file on ambiguity and leaves it for the manual action). Exactly one of
// `single`/`ambiguous`/`error` is meaningful: `single.release` non-empty means
// a confident match; otherwise `ambiguous` holds 2+ candidates needing a
// manual pick, or is empty with `error` set to why nothing came back at all.
struct ResolvedCandidates {
	SceneRelease         single;
	QList<SceneRelease>  ambiguous;
	QString              error;
};

// The exact-name → keyword-fallback → resolution-tier-filter → IMDb-id
// disambiguation chain, extracted from what used to be entirely inline in
// SceneNfoDownloadWorker::search() so both callers share one implementation.
ResolvedCandidates resolveSceneCandidates(SrrdbClient& client, const QString& videoPath,
                                           const QString& imdbId)
{
	ResolvedCandidates result;

	const QString exactName = QFileInfo(videoPath).completeBaseName();
	SceneRelease exact;
	QList<SceneRelease> candidates;
	if (client.searchExact(exactName, exact)) {
		candidates << exact;
	} else {
		// Renamed by Radarr/Sonarr/Plex naming — the exact original release name
		// is gone, so fall back to a keyword search on the title+year instead.
		const QString title = NfoParser::titleFromFilename(QFileInfo(videoPath).fileName());
		const QStringList keywords = title.split(QLatin1Char(' '), Qt::SkipEmptyParts);
		if (!keywords.isEmpty())
			candidates = client.searchKeywords(keywords);
		candidates.removeIf([](const SceneRelease& r) { return !r.hasNfo; });
		candidates.removeIf([](const SceneRelease& r) { return !looksLikeVideoRelease(r.release); });

		// Require a matching resolution tier when the file's own is known — a
		// scene NFO describes ITS release's specific tech details, so a 480p
		// DVDRip's NFO attached to a 2160p file would misdescribe it (wrong
		// bitrate/audio/HDR info), not just be imprecise. Most UHD Remuxes have
		// no scene equivalent at all (remuxing isn't scene practice), so this
		// correctly empties the list rather than offering a wrong-tier fallback.
		const QString myTier = resolutionTier(QFileInfo(videoPath).fileName());
		if (!myTier.isEmpty()) {
			QList<SceneRelease> sameTier;
			for (const SceneRelease& r : candidates)
				if (resolutionTier(r.release) == myTier)
					sameTier << r;
			candidates = sameTier;
		}
	}

	if (candidates.isEmpty()) {
		result.error = client.lastError().isEmpty()
		    ? QStringLiteral("No matching scene release found on srrDB.")
		    : client.lastError();
		return result;
	}

	// Auto-disambiguate by IMDb id when we already know it — srrDB's imdbId
	// field has no "tt" prefix, unlike ours.
	if (!imdbId.isEmpty()) {
		const QString bareImdb = QString(imdbId).remove(QStringLiteral("tt"));
		QList<SceneRelease> byImdb;
		for (const SceneRelease& r : candidates)
			if (!r.imdbId.isEmpty() && r.imdbId == bareImdb)
				byImdb << r;
		if (byImdb.size() == 1) {
			result.single = byImdb.first();
			return result;
		}
		if (!byImdb.isEmpty())
			candidates = byImdb;
	}

	if (candidates.size() == 1) {
		result.single = candidates.first();
		return result;
	}

	result.ambiguous = candidates;
	return result;
}

} // namespace

SrrdbClient::SrrdbClient(QObject* parent)
	: QObject(parent)
{
	m_nam = new QNetworkAccessManager(this);
}

SrrdbClient::~SrrdbClient() = default;

void SrrdbClient::cancel()
{
	m_cancelled.storeRelaxed(1);
	if (m_reply)
		m_reply->abort();
}

QList<SceneRelease> SrrdbClient::parseResults(const QByteArray& json) const
{
	QList<SceneRelease> out;
	const QJsonObject root = QJsonDocument::fromJson(json).object();
	for (const QJsonValue& v : root.value(QStringLiteral("results")).toArray()) {
		const QJsonObject o = v.toObject();
		SceneRelease r;
		r.release = o.value(QStringLiteral("release")).toString();
		r.imdbId  = o.value(QStringLiteral("imdbId")).toVariant().toString();
		r.hasNfo  = o.value(QStringLiteral("hasNFO")).toString().compare(
		                QStringLiteral("yes"), Qt::CaseInsensitive) == 0;
		r.size    = o.value(QStringLiteral("size")).toVariant().toLongLong();
		if (!r.release.isEmpty())
			out << r;
	}
	return out;
}

bool SrrdbClient::searchExact(const QString& releaseName, SceneRelease& out)
{
	const QUrl url(QStringLiteral("%1/search/r:%2")
	    .arg(QLatin1String(kBaseUrl), QString::fromUtf8(QUrl::toPercentEncoding(releaseName))));

	QByteArray body;
	if (!httpGetJson(url, body)) return false;

	for (const SceneRelease& r : parseResults(body)) {
		if (r.release.compare(releaseName, Qt::CaseInsensitive) == 0 && r.hasNfo) {
			out = r;
			return true;
		}
	}
	m_lastError = QStringLiteral("No exact release match");
	return false;
}

QList<SceneRelease> SrrdbClient::searchKeywords(const QStringList& keywords)
{
	QStringList encoded;
	for (const QString& kw : keywords)
		encoded << QString::fromUtf8(QUrl::toPercentEncoding(kw));
	const QUrl url(QStringLiteral("%1/search/%2")
	    .arg(QLatin1String(kBaseUrl), encoded.join(QLatin1Char('/'))));

	QByteArray body;
	if (!httpGetJson(url, body)) return {};
	return parseResults(body);
}

bool SrrdbClient::resolveNfoLink(const QString& releaseName, QString& outUrl)
{
	const QUrl url(QStringLiteral("%1/nfo/%2")
	    .arg(QLatin1String(kBaseUrl), QString::fromUtf8(QUrl::toPercentEncoding(releaseName))));

	QByteArray body;
	if (!httpGetJson(url, body)) return false;

	const QJsonObject root = QJsonDocument::fromJson(body).object();
	const QJsonArray  links = root.value(QStringLiteral("nfolink")).toArray();
	if (links.isEmpty()) {
		m_lastError = QStringLiteral("srrDB has no NFO link for this release");
		return false;
	}
	outUrl = links.first().toString();
	return !outUrl.isEmpty();
}

bool SrrdbClient::downloadBytes(const QUrl& url, QByteArray& outBytes)
{
	return httpGetBytes(url, outBytes);
}

bool SrrdbClient::httpGetJson(const QUrl& url, QByteArray& out)
{
	if (m_cancelled.loadRelaxed()) {
		m_lastError = QStringLiteral("Cancelled");
		return false;
	}

	QNetworkRequest req(url);
	req.setRawHeader(QByteArrayLiteral("Accept"),     QByteArrayLiteral("application/json"));
	req.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("MediaCurator/1.0"));
	req.setTransferTimeout(30000);

	m_reply = m_nam->get(req);
	{
		QEventLoop loop;
		connect(m_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		loop.exec();
	}

	m_lastStatus = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	out = m_reply->readAll();
	m_reply->deleteLater();
	m_reply = nullptr;

	if (m_cancelled.loadRelaxed()) {
		m_lastError = QStringLiteral("Cancelled");
		return false;
	}
	if (m_lastStatus == 0) {
		m_lastError = QStringLiteral("Network error contacting srrDB");
		return false;
	}
	if (m_lastStatus >= 400) {
		m_lastError = QStringLiteral("srrDB HTTP %1").arg(m_lastStatus);
		if (m_lastStatus >= 500)
			qWarning(lcSrrdb) << "Server error" << m_lastStatus << "for" << url;
		return false;
	}
	return true;
}

bool SrrdbClient::httpGetBytes(const QUrl& url, QByteArray& out)
{
	if (m_cancelled.loadRelaxed()) {
		m_lastError = QStringLiteral("Cancelled");
		return false;
	}

	QNetworkRequest req(url);
	req.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("MediaCurator/1.0"));
	req.setTransferTimeout(30000);

	m_reply = m_nam->get(req);
	{
		QEventLoop loop;
		connect(m_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		loop.exec();
	}

	m_lastStatus = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	out = m_reply->readAll();
	m_reply->deleteLater();
	m_reply = nullptr;

	if (m_cancelled.loadRelaxed()) {
		m_lastError = QStringLiteral("Cancelled");
		return false;
	}
	if (m_lastStatus == 0) {
		m_lastError = QStringLiteral("Network error downloading NFO");
		return false;
	}
	if (m_lastStatus >= 400) {
		m_lastError = QStringLiteral("HTTP %1 downloading NFO").arg(m_lastStatus);
		return false;
	}
	return true;
}

// ── SceneNfoDownloadWorker ───────────────────────────────────────────────────

SceneNfoDownloadWorker::SceneNfoDownloadWorker(const QString& videoPath, const QString& imdbId,
                                               QObject* parent)
	: QObject(parent)
	, m_videoPath(videoPath)
	, m_imdbId(imdbId)
{
}

void SceneNfoDownloadWorker::cancel()
{
	m_cancelled.storeRelaxed(1);
	if (m_client) {
		// A phase is mid network-call — aborting its reply makes that phase's
		// own httpGetJson()/httpGetBytes() fail and unwind through the normal
		// done(false, ...) path below.
		m_client->cancel();
		return;
	}
	// No phase is running — search() already returned after emitting
	// candidatesFound() and is waiting on the dialog's picker, so there's no
	// in-flight request for the above to abort. Finish the run here instead,
	// or the dialog's Cancel button would wait forever for a done() that
	// nothing would otherwise emit.
	emit done(false, QString(), QByteArray(), QStringLiteral("Cancelled"));
	QThread::currentThread()->quit();
}

void SceneNfoDownloadWorker::search()
{
	SrrdbClient client;
	m_client = &client;
	if (m_cancelled.loadRelaxed())
		client.cancel();

	const ResolvedCandidates resolved = resolveSceneCandidates(client, m_videoPath, m_imdbId);

	if (m_cancelled.loadRelaxed()) {
		emit done(false, QString(), QByteArray(), QStringLiteral("Cancelled"));
		m_client = nullptr;
		QThread::currentThread()->quit();
		return;
	}

	if (!resolved.single.release.isEmpty()) {
		startDownload(resolved.single.release);
		return;
	}

	if (resolved.ambiguous.isEmpty()) {
		emit done(false, QString(), QByteArray(), resolved.error);
		m_client = nullptr;
		QThread::currentThread()->quit();
		return;
	}

	QStringList labels, names;
	for (const SceneRelease& r : resolved.ambiguous) {
		labels << (r.imdbId.isEmpty()
		    ? r.release
		    : QStringLiteral("%1  [tt%2]").arg(r.release, r.imdbId));
		names << r.release;
	}
	m_client = nullptr;
	emit candidatesFound(labels, names);
	// Thread's event loop stays alive here — chooseRelease() arrives later via
	// a queued invocation once the dialog's picker resolves.
}

void SceneNfoDownloadWorker::chooseRelease(QString releaseName)
{
	startDownload(releaseName);
}

void SceneNfoDownloadWorker::startDownload(const QString& releaseName)
{
	if (m_cancelled.loadRelaxed()) {
		// chooseRelease() and cancel() are both queued cross-thread invocations
		// — this one can still have been in flight when cancel() landed first.
		emit done(false, releaseName, QByteArray(), QStringLiteral("Cancelled"));
		QThread::currentThread()->quit();
		return;
	}
	emit releaseSelected(releaseName);

	SrrdbClient client;
	m_client = &client;
	if (m_cancelled.loadRelaxed())
		client.cancel();

	QString nfoUrl;
	if (!client.resolveNfoLink(releaseName, nfoUrl)) {
		emit done(false, releaseName, QByteArray(), client.lastError());
		m_client = nullptr;
		QThread::currentThread()->quit();
		return;
	}

	QByteArray bytes;
	if (!client.downloadBytes(QUrl(nfoUrl), bytes)) {
		emit done(false, releaseName, QByteArray(), client.lastError());
		m_client = nullptr;
		QThread::currentThread()->quit();
		return;
	}

	emit done(true, releaseName, bytes, QString());
	m_client = nullptr;
	// Self-quit rather than relying solely on the done->QThread::quit connection
	// — see SubtitleDownloadWorker::run() for why (a dialog destructor
	// synchronously wait()-ing on this thread would otherwise deadlock).
	QThread::currentThread()->quit();
}

// ── SceneNfoWorker ───────────────────────────────────────────────────────────
//
// Background auto-download queue for scene NFOs — same single-thread, 0ms
// QTimer-driven queue shape as SubtitleWorker (SubtitleManager.cpp), but
// without any quota/backoff subsystem (srrDB has no known daily quota).

class SceneNfoWorker : public QObject
{
	Q_OBJECT
public:
	SceneNfoWorker() = default;

public slots:
	void startProcessing()
	{
		m_timer = new QTimer(this);
		m_timer->setSingleShot(false);
		m_timer->setInterval(0);
		connect(m_timer, &QTimer::timeout, this, &SceneNfoWorker::processNext);
	}

	void stop()
	{
		// Same shutdown hazard as SubtitleWorker::stop() (see its extensive
		// comment there): a cross-thread QThread::quit() only ever affects
		// whichever event loop is currently innermost for this thread. While a
		// search/download is in flight, that's SrrdbClient's own blocking
		// QEventLoop, not this thread's real one — cancelling the in-flight
		// request instead unwinds that nested loop on its own, and processNext()
		// then notices m_stopping once back on the real loop. Only safe to quit
		// directly here when genuinely idle (not mid-processFile).
		m_stopping = true;
		if (m_client) {
			m_client->cancel();
		} else if (!m_processing) {
			if (m_timer) m_timer->stop();
			QThread::currentThread()->quit();
		}
	}

	void cancelAll()
	{
		const bool hadWork = !m_queue.isEmpty() || m_processing;
		m_queue.clear();
		if (m_client) m_client->cancel();
		if (hadWork) emit queueActiveChanged(false);
	}

	void setEnabled(bool enabled)
	{
		m_enabled = enabled;
		startTimerIfWork();
		if (canRun() && !m_queue.isEmpty()) emit queueActiveChanged(true);
	}

	void setWriteNfoFiles(bool enabled)
	{
		m_writeNfoFiles = enabled;
	}

	void setRetryCooldownDays(int days)
	{
		m_retryCooldownDays = qMax(0, days);
	}

	void enqueueFile(qint64 fileId)
	{
		if (m_stopping || fileId <= 0) return;
		const bool wasEmpty = m_queue.isEmpty() && !m_processing;
		if (!m_queue.contains(fileId))
			m_queue.append(fileId);
		if (wasEmpty && !m_queue.isEmpty() && canRun()) emit queueActiveChanged(true);
		startTimerIfWork();
	}

	void enqueueBatch(const QList<qint64>& fileIds)
	{
		if (m_stopping || fileIds.isEmpty()) return;
		const bool wasEmpty = m_queue.isEmpty() && !m_processing;
		for (qint64 id : fileIds) {
			if (id > 0 && !m_queue.contains(id))
				m_queue.append(id);
		}
		if (wasEmpty && !m_queue.isEmpty() && canRun()) emit queueActiveChanged(true);
		startTimerIfWork();
	}

signals:
	void sceneNfoReady(qint64 fileId);
	void queueActiveChanged(bool active);

private slots:
	void processNext()
	{
		// Guard against re-entry: SrrdbClient's httpGetJson()/httpGetBytes() block
		// on their own QEventLoop while this 0ms timer keeps firing — see
		// SubtitleWorker::processNext() for the identical concern.
		if (m_processing) return;

		if (m_stopping) {
			if (m_timer) m_timer->stop();
			QThread::currentThread()->quit();
			return;
		}

		if (!canRun() || m_queue.isEmpty()) {
			m_timer->stop();
			if (m_queue.isEmpty()) emit queueActiveChanged(false);
			return;
		}

		m_processing = true;
		m_timer->stop();
		const qint64 fileId = m_queue.takeFirst();
		processFile(fileId);
		m_processing = false;

		if (m_stopping) {
			QThread::currentThread()->quit();
			return;
		}
		startTimerIfWork();
		if (m_queue.isEmpty()) emit queueActiveChanged(false);
	}

private:
	bool canRun() const { return m_enabled; }

	void startTimerIfWork()
	{
		if (canRun() && m_timer && !m_timer->isActive() && !m_queue.isEmpty())
			m_timer->start();
	}

	void processFile(qint64 fileId)
	{
		const auto fileOpt = DatabaseManager::instance().fileById(fileId);
		if (!fileOpt) return;
		const FileRecord& file = *fileOpt;

		if (file.hasSceneNfo) return;

		// Something's genuinely missing, but if we already tried this file
		// recently and srrDB had nothing usable, don't hammer the same search
		// every scan — wait out the cooldown. 0 disables this (always retry).
		if (m_retryCooldownDays > 0 && file.sceneNfoAttemptedMs > 0) {
			const qint64 cooldownMs = qint64(m_retryCooldownDays) * 24 * 60 * 60 * 1000;
			if (QDateTime::currentMSecsSinceEpoch() - file.sceneNfoAttemptedMs < cooldownMs)
				return;
		}

		QString imdbId;
		if (const auto pr = DatabaseManager::instance().posterForFile(fileId))
			imdbId = pr->imdbId;
		if (imdbId.isEmpty())
			imdbId = NfoParser::readImdbId(file.path);

		SrrdbClient client;
		m_client = &client;
		if (m_stopping) client.cancel();

		const ResolvedCandidates resolved = resolveSceneCandidates(client, file.path, imdbId);

		bool downloaded = false;
		QByteArray bytes;
		if (!resolved.single.release.isEmpty()) {
			QString nfoUrl;
			if (client.resolveNfoLink(resolved.single.release, nfoUrl))
				downloaded = client.downloadBytes(QUrl(nfoUrl), bytes);
		}
		m_client = nullptr;

		// Stamp regardless of outcome (single match downloaded, ambiguous, no
		// match, or a failed fetch) — even "nothing usable" is a resolved
		// attempt that should hold off the next one until the cooldown elapses,
		// same as SubtitleWorker::processFile().
		DatabaseManager::instance().updateSceneNfoAttempted(fileId, QDateTime::currentMSecsSinceEpoch());

		if (!downloaded) return;   // ambiguous/no-match/failed — leave for the manual action

		DatabaseManager::instance().updateSceneNfo(fileId, true, NfoParser::decodeSceneNfoBytes(bytes));
		if (!m_writeNfoFiles)
			NfoParser::writeSceneNfoFile(file.path, bytes);
		emit sceneNfoReady(fileId);
	}

	QTimer*       m_timer      = nullptr;
	QList<qint64> m_queue;
	bool          m_enabled         = false;
	bool          m_writeNfoFiles   = false;
	int           m_retryCooldownDays = 7;
	bool          m_stopping        = false;
	bool          m_processing      = false;
	SrrdbClient*  m_client          = nullptr; // valid only mid-processFile()
};

// ── SceneNfoManager ──────────────────────────────────────────────────────────

SceneNfoManager& SceneNfoManager::instance()
{
	static SceneNfoManager s;
	return s;
}

SceneNfoManager::SceneNfoManager(QObject* parent)
	: QObject(parent)
{}

void SceneNfoManager::start(bool enabled)
{
	if (m_thread) return;

	m_worker = new SceneNfoWorker();
	m_thread = new QThread(this);
	m_worker->moveToThread(m_thread);

	connect(m_thread, &QThread::started,  m_worker, &SceneNfoWorker::startProcessing);
	connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

	connect(m_worker, &SceneNfoWorker::sceneNfoReady,
	        this,      &SceneNfoManager::sceneNfoReady);
	connect(m_worker, &SceneNfoWorker::queueActiveChanged,
	        this,      &SceneNfoManager::queueActiveChanged);

	connect(this, &SceneNfoManager::workerSetEnabled,
	        m_worker, &SceneNfoWorker::setEnabled, Qt::QueuedConnection);
	connect(this, &SceneNfoManager::workerSetWriteNfoFiles,
	        m_worker, &SceneNfoWorker::setWriteNfoFiles, Qt::QueuedConnection);
	connect(this, &SceneNfoManager::workerSetRetryCooldownDays,
	        m_worker, &SceneNfoWorker::setRetryCooldownDays, Qt::QueuedConnection);
	connect(this, &SceneNfoManager::workerEnqueueFile,
	        m_worker, &SceneNfoWorker::enqueueFile, Qt::QueuedConnection);
	connect(this, &SceneNfoManager::workerEnqueueBatch,
	        m_worker, &SceneNfoWorker::enqueueBatch, Qt::QueuedConnection);
	connect(this, &SceneNfoManager::workerCancelAll,
	        m_worker, &SceneNfoWorker::cancelAll, Qt::QueuedConnection);
	connect(this, &SceneNfoManager::workerStop,
	        m_worker, &SceneNfoWorker::stop, Qt::QueuedConnection);

	m_thread->start();

	emit workerSetEnabled(enabled);
}

void SceneNfoManager::stop()
{
	if (!m_thread || !m_thread->isRunning()) return;
	emit workerStop();
	m_thread->quit();
	if (!m_thread->wait(5000)) {
		m_thread->terminate();
		m_thread->wait(1000);
	}
}

void SceneNfoManager::setEnabled(bool enabled)
{
	emit workerSetEnabled(enabled);
}

void SceneNfoManager::setWriteNfoFiles(bool enabled)
{
	emit workerSetWriteNfoFiles(enabled);
}

void SceneNfoManager::setRetryCooldownDays(int days)
{
	emit workerSetRetryCooldownDays(days);
}

void SceneNfoManager::enqueue(qint64 fileId)
{
	emit workerEnqueueFile(fileId);
}

void SceneNfoManager::enqueueBatch(const QList<qint64>& fileIds)
{
	if (fileIds.isEmpty()) return;
	emit workerEnqueueBatch(fileIds);
}

void SceneNfoManager::cancelAll()
{
	if (!m_thread || !m_thread->isRunning()) return;
	emit workerCancelAll();
}

} // namespace Mc

#include "SrrdbClient.moc"
