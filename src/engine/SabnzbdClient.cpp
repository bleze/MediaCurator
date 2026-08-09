#include "engine/SabnzbdClient.h"
#include "core/AppSettings.h"
#include "core/DownloadIntegrationSettings.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace Mc {

namespace {
constexpr int kPollIntervalMs = 5000;

// SABnzbd's queue "mb"/"mbleft" fields are returned as strings (e.g. "700.00")
// rather than JSON numbers, unlike NZBGet's API.
qint64 toMb(const QJsonValue& v)
{
	if (v.isString())
		return static_cast<qint64>(v.toString().toDouble());
	return static_cast<qint64>(v.toDouble());
}

// SABnzbd history status is a single flat value ("Completed", "Failed",
// "Extracting", ...) rather than NZBGet's compound "SUCCESS/ALL" style.
bool isSuccessStatus(const QString& status)
{
	return status.compare(QStringLiteral("Completed"), Qt::CaseInsensitive) == 0;
}
}

SabnzbdClient::SabnzbdClient(QObject* parent)
	: DownloadClient(parent)
{
	m_nam = new QNetworkAccessManager(this);

	m_timer = new QTimer(this);
	m_timer->setInterval(kPollIntervalMs);
	connect(m_timer, &QTimer::timeout, this, &SabnzbdClient::pollNow);
}

bool SabnzbdClient::isConfigured() const
{
	const SabnzbdConfig config = DownloadIntegrationSettings::sabnzbdConfig();
	return config.enabled && !config.host.isEmpty() && !config.apiKey.isEmpty();
}

void SabnzbdClient::reconfigure()
{
	if (isConfigured()) {
		if (!m_timer->isActive()) {
			m_timer->start();
			pollNow();
		}
	} else {
		m_timer->stop();
		emit queueUpdated({});
	}
}

void SabnzbdClient::pollNow()
{
	if (!isConfigured())
		return;
	pollQueue();
	pollHistory();
}

QNetworkReply* SabnzbdClient::getApi(const QString& mode)
{
	const SabnzbdConfig config = DownloadIntegrationSettings::sabnzbdConfig();

	QUrl url;
	url.setScheme(QStringLiteral("http"));
	url.setHost(config.host);
	url.setPort(config.port);
	url.setPath(QStringLiteral("/api"));

	QUrlQuery query;
	query.addQueryItem(QStringLiteral("mode"), mode);
	query.addQueryItem(QStringLiteral("output"), QStringLiteral("json"));
	query.addQueryItem(QStringLiteral("apikey"), config.apiKey);
	url.setQuery(query);

	return m_nam->get(QNetworkRequest(url));
}

void SabnzbdClient::pollQueue()
{
	if (m_queueInFlight)
		return;
	m_queueInFlight = true;

	QNetworkReply* reply = getApi(QStringLiteral("queue"));
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		m_queueInFlight = false;
		onQueueReply(reply);
	});
}

void SabnzbdClient::onQueueReply(QNetworkReply* reply)
{
	reply->deleteLater();
	if (reply->error() != QNetworkReply::NoError) {
		emit connectionError(reply->errorString());
		return;
	}

	const QJsonObject root    = QJsonDocument::fromJson(reply->readAll()).object();
	const QJsonArray  entries = root.value(QStringLiteral("queue")).toObject()
	                                .value(QStringLiteral("slots")).toArray();

	QList<DownloadQueueItem> items;
	items.reserve(entries.size());
	for (const QJsonValue& v : entries) {
		const QJsonObject s = v.toObject();
		DownloadQueueItem item;
		item.providerId  = providerId();
		item.name        = s.value(QStringLiteral("filename")).toString();
		item.totalMb     = toMb(s.value(QStringLiteral("mb")));
		item.remainingMb = toMb(s.value(QStringLiteral("mbleft")));
		item.status      = s.value(QStringLiteral("status")).toString();
		items << item;
	}

	emit queueUpdated(items);
}

void SabnzbdClient::pollHistory()
{
	if (m_historyInFlight)
		return;
	m_historyInFlight = true;

	QNetworkReply* reply = getApi(QStringLiteral("history"));
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		m_historyInFlight = false;
		onHistoryReply(reply);
	});
}

void SabnzbdClient::onHistoryReply(QNetworkReply* reply)
{
	reply->deleteLater();
	if (reply->error() != QNetworkReply::NoError) {
		emit connectionError(reply->errorString());
		return;
	}

	const QJsonObject root    = QJsonDocument::fromJson(reply->readAll()).object();
	const QJsonArray  entries = root.value(QStringLiteral("history")).toObject()
	                                .value(QStringLiteral("slots")).toArray();

	const QString    lastSeenKey = QStringLiteral("sabnzbd/lastHistoryTime");
	const qint64     lastSeen    = AppSettings::instance().value(lastSeenKey, 0).toLongLong();
	const bool       haveBaseline = lastSeen > 0;
	qint64           newestSeen  = lastSeen;
	QStringList      completedNames;

	for (const QJsonValue& v : entries) {
		const QJsonObject h = v.toObject();
		const qint64 completedTime = static_cast<qint64>(h.value(QStringLiteral("completed")).toDouble());
		if (completedTime > newestSeen)
			newestSeen = completedTime;

		if (!haveBaseline || completedTime <= lastSeen)
			continue;

		const QString status = h.value(QStringLiteral("status")).toString();
		if (isSuccessStatus(status))
			completedNames << h.value(QStringLiteral("name")).toString();
	}

	if (newestSeen != lastSeen)
		AppSettings::instance().setValue(lastSeenKey, newestSeen);

	// First-ever poll after enabling: only record the baseline, don't treat
	// all pre-existing history as new completions.
	if (haveBaseline && !completedNames.isEmpty())
		emit downloadsCompleted(completedNames);
}

} // namespace Mc
