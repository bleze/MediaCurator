#include "engine/SrrdbClient.h"
#include "scanner/NfoParser.h"

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

	const QString exactName = QFileInfo(m_videoPath).completeBaseName();
	SceneRelease exact;
	QList<SceneRelease> candidates;
	if (client.searchExact(exactName, exact)) {
		candidates << exact;
	} else {
		// Renamed by Radarr/Sonarr/Plex naming — the exact original release name
		// is gone, so fall back to a keyword search on the title+year instead.
		const QString title = NfoParser::titleFromFilename(QFileInfo(m_videoPath).fileName());
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
		const QString myTier = resolutionTier(QFileInfo(m_videoPath).fileName());
		if (!myTier.isEmpty()) {
			QList<SceneRelease> sameTier;
			for (const SceneRelease& r : candidates)
				if (resolutionTier(r.release) == myTier)
					sameTier << r;
			candidates = sameTier;
		}
	}

	if (m_cancelled.loadRelaxed()) {
		emit done(false, QString(), QByteArray(), QStringLiteral("Cancelled"));
		m_client = nullptr;
		QThread::currentThread()->quit();
		return;
	}

	if (candidates.isEmpty()) {
		const QString err = client.lastError().isEmpty()
		    ? QStringLiteral("No matching scene release found on srrDB.")
		    : client.lastError();
		emit done(false, QString(), QByteArray(), err);
		m_client = nullptr;
		QThread::currentThread()->quit();
		return;
	}

	// Auto-disambiguate by IMDb id when we already know it — srrDB's imdbId
	// field has no "tt" prefix, unlike ours.
	if (!m_imdbId.isEmpty()) {
		const QString bareImdb = QString(m_imdbId).remove(QStringLiteral("tt"));
		QList<SceneRelease> byImdb;
		for (const SceneRelease& r : candidates)
			if (!r.imdbId.isEmpty() && r.imdbId == bareImdb)
				byImdb << r;
		if (byImdb.size() == 1) {
			startDownload(byImdb.first().release);
			return;
		}
		if (!byImdb.isEmpty())
			candidates = byImdb;
	}

	if (candidates.size() == 1) {
		startDownload(candidates.first().release);
		return;
	}

	QStringList labels, names;
	for (const SceneRelease& r : candidates) {
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

} // namespace Mc
