#include "engine/NzbGetClient.h"
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

namespace Mc {

namespace {
constexpr int kPollIntervalMs = 5000;

// NZBGet history "Status" is compound, e.g. "SUCCESS/ALL", "SUCCESS/UNPACK",
// "WARNING/SCRIPT", "FAILURE/PAR". Only the top-level SUCCESS/* entries mean
// the file (post any pp-script sort/rename) actually landed where expected.
bool isSuccessStatus(const QString& status)
{
	return status.startsWith(QStringLiteral("SUCCESS"), Qt::CaseInsensitive);
}
}

NzbGetClient::NzbGetClient(QObject* parent)
	: DownloadClient(parent)
{
	m_nam = new QNetworkAccessManager(this);

	m_timer = new QTimer(this);
	m_timer->setInterval(kPollIntervalMs);
	connect(m_timer, &QTimer::timeout, this, &NzbGetClient::pollNow);
}

bool NzbGetClient::isConfigured() const
{
	const NzbGetConfig config = DownloadIntegrationSettings::nzbgetConfig();
	return config.enabled && !config.host.isEmpty();
}

void NzbGetClient::reconfigure()
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

void NzbGetClient::pollNow()
{
	if (!isConfigured())
		return;
	pollQueue();
	pollHistory();
}

QNetworkReply* NzbGetClient::postRpc(const QString& method, const QByteArray& paramsJson)
{
	const NzbGetConfig config = DownloadIntegrationSettings::nzbgetConfig();

	QUrl url;
	url.setScheme(QStringLiteral("http"));
	url.setHost(config.host);
	url.setPort(config.port);
	url.setPath(QStringLiteral("/jsonrpc"));

	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	if (!config.username.isEmpty()) {
		const QByteArray creds = (config.username + QLatin1Char(':') + config.password).toUtf8();
		req.setRawHeader("Authorization", "Basic " + creds.toBase64());
	}

	const QByteArray body = "{\"method\":\"" + method.toUtf8() + "\",\"params\":" + paramsJson
	                         + ",\"id\":1}";
	return m_nam->post(req, body);
}

void NzbGetClient::pollQueue()
{
	if (m_queueInFlight)
		return;
	m_queueInFlight = true;

	QNetworkReply* reply = postRpc(QStringLiteral("listgroups"), "[0]");
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		m_queueInFlight = false;
		onQueueReply(reply);
	});
}

void NzbGetClient::onQueueReply(QNetworkReply* reply)
{
	reply->deleteLater();
	if (reply->error() != QNetworkReply::NoError) {
		emit connectionError(reply->errorString());
		return;
	}

	const QJsonObject root   = QJsonDocument::fromJson(reply->readAll()).object();
	const QJsonArray  groups = root.value(QStringLiteral("result")).toArray();

	QList<DownloadQueueItem> items;
	items.reserve(groups.size());
	for (const QJsonValue& v : groups) {
		const QJsonObject g = v.toObject();
		DownloadQueueItem item;
		item.providerId  = providerId();
		item.name        = g.value(QStringLiteral("NZBName")).toString();
		item.totalMb     = static_cast<qint64>(g.value(QStringLiteral("FileSizeMB")).toDouble());
		item.remainingMb = static_cast<qint64>(g.value(QStringLiteral("RemainingSizeMB")).toDouble());
		item.status      = g.value(QStringLiteral("Status")).toString();
		items << item;
	}

	emit queueUpdated(items);
}

void NzbGetClient::pollHistory()
{
	if (m_historyInFlight)
		return;
	m_historyInFlight = true;

	QNetworkReply* reply = postRpc(QStringLiteral("history"), "[false]");
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		m_historyInFlight = false;
		onHistoryReply(reply);
	});
}

void NzbGetClient::onHistoryReply(QNetworkReply* reply)
{
	reply->deleteLater();
	if (reply->error() != QNetworkReply::NoError) {
		emit connectionError(reply->errorString());
		return;
	}

	const QJsonObject root  = QJsonDocument::fromJson(reply->readAll()).object();
	const QJsonArray  items = root.value(QStringLiteral("result")).toArray();

	const QString    lastSeenKey = QStringLiteral("nzbget/lastHistoryTime");
	const qint64     lastSeen    = AppSettings::instance().value(lastSeenKey, 0).toLongLong();
	const bool       haveBaseline = lastSeen > 0;
	qint64           newestSeen  = lastSeen;
	QStringList      completedNames;

	for (const QJsonValue& v : items) {
		const QJsonObject h = v.toObject();
		const qint64 historyTime = static_cast<qint64>(h.value(QStringLiteral("HistoryTime")).toDouble());
		if (historyTime > newestSeen)
			newestSeen = historyTime;

		if (!haveBaseline || historyTime <= lastSeen)
			continue;

		const QString status = h.value(QStringLiteral("Status")).toString();
		if (isSuccessStatus(status))
			completedNames << h.value(QStringLiteral("Name")).toString();
	}

	if (newestSeen != lastSeen)
		AppSettings::instance().setValue(lastSeenKey, newestSeen);

	// First-ever poll after enabling: only record the baseline, don't treat
	// all pre-existing history as new completions.
	if (haveBaseline && !completedNames.isEmpty())
		emit downloadsCompleted(completedNames);
}

} // namespace Mc
