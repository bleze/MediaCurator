#include "engine/HighscoreClient.h"
#include "engine/HighscoreConfig.h"

#if __has_include("engine/McHighscoreConfig.local.h")
#include "engine/McHighscoreConfig.local.h"
#define MC_HAS_DREAMLO_PRIVATE_CODE 1
#endif

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QDebug>
#include <algorithm>

namespace Mc {

namespace {

QString privateCode()
{
#ifdef MC_HAS_DREAMLO_PRIVATE_CODE
	return QString::fromLatin1(kDreamloPrivateCode);
#else
	return {};
#endif
}

// dreamlo's "date" field, e.g. "7/22/2026 3:45:12 PM". Returns an invalid
// QDateTime (caller must check isValid()) if the format ever changes on us.
QDateTime parseDreamloDate(const QString& s)
{
	return QDateTime::fromString(s, QStringLiteral("M/d/yyyy h:mm:ss AP"));
}

} // namespace

HighscoreClient& HighscoreClient::instance()
{
	static HighscoreClient s_instance;
	return s_instance;
}

HighscoreClient::HighscoreClient(QObject* parent)
	: QObject(parent)
{
	m_nam = new QNetworkAccessManager(this);
}

bool HighscoreClient::isEnabled() const
{
	return !privateCode().isEmpty();
}

void HighscoreClient::submitScore(const QString& name, qint64 score)
{
	if (!isEnabled() || m_submitInFlight) {
		emit scoreSubmitted(false);
		return;
	}
	m_submitInFlight = true;

	const QString encodedName = QString::fromLatin1(QUrl::toPercentEncoding(name));
	QNetworkRequest req{ QUrl(QStringLiteral("http://dreamlo.com/lb/%1/add/%2/%3")
	                              .arg(privateCode(), encodedName)
	                              .arg(qMax<qint64>(0, score))) };
	req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MediaCurator"));

	QNetworkReply* reply = m_nam->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		m_submitInFlight = false;
		const bool ok = reply->error() == QNetworkReply::NoError;
		if (!ok)
			qWarning() << "HighscoreClient: submit failed:" << reply->errorString();
		reply->deleteLater();
		emit scoreSubmitted(ok);
	});
}

void HighscoreClient::fetchLeaderboard()
{
	if (m_fetchInFlight) {
		m_fetchPendingRerun = true;
		return;
	}
	m_fetchInFlight = true;

	QNetworkRequest req{ QUrl(QStringLiteral("http://dreamlo.com/lb/%1/json")
	                              .arg(QLatin1String(kDreamloPublicCode))) };
	req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MediaCurator"));

	QNetworkReply* reply = m_nam->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		m_fetchInFlight = false;
		if (reply->error() != QNetworkReply::NoError) {
			qWarning() << "HighscoreClient: fetch failed:" << reply->errorString();
			emit leaderboardFetchFailed(reply->errorString());
		} else {
			emit leaderboardReady(parseLeaderboard(reply->readAll()));
		}
		reply->deleteLater();
		if (m_fetchPendingRerun) {
			m_fetchPendingRerun = false;
			fetchLeaderboard();
		}
	});
}

QList<HighscoreEntry> HighscoreClient::parseLeaderboard(const QByteArray& json)
{
	QList<HighscoreEntry> out;
	const QJsonObject board = QJsonDocument::fromJson(json)
	                               .object().value(QStringLiteral("dreamlo")).toObject()
	                               .value(QStringLiteral("leaderboard")).toObject();
	const QJsonValue entryVal = board.value(QStringLiteral("entry"));

	// dreamlo's XML->JSON conversion emits a bare object (not a 1-element
	// array) when there is exactly one entry, and omits "entry" entirely when
	// the board is empty — guard both.
	QJsonArray entries;
	if (entryVal.isArray())
		entries = entryVal.toArray();
	else if (entryVal.isObject())
		entries.append(entryVal);

	for (const QJsonValue& v : entries) {
		const QJsonObject o = v.toObject();
		// score may come back as a JSON string or number depending on
		// dreamlo's converter — QVariant::toLongLong() handles both.
		out.append({ o.value(QStringLiteral("name")).toString(),
		             o.value(QStringLiteral("score")).toVariant().toLongLong(),
		             parseDreamloDate(o.value(QStringLiteral("date")).toString()) });
	}

	// Defensive — dreamlo documents descending order, but don't trust it blindly.
	std::sort(out.begin(), out.end(), [](const HighscoreEntry& a, const HighscoreEntry& b) {
		return a.score > b.score;
	});
	return out;
}

} // namespace Mc
