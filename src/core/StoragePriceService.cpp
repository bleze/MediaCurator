#include "core/StoragePriceService.h"
#include "core/AppSettings.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace Mc {

namespace {
// Rough current consumer SATA HDD price, used only until the first
// successful fetch (or if DatacenterDisk.com is ever unreachable/gone) so
// "Money Saved" always shows something rather than nothing.
constexpr double kFallbackPricePerTbUsd = 18.0;

// DatacenterDisk.com's own cache_ttl is 3600s, but the underlying Amazon
// prices only refresh every ~2h anyway — no reason to hit them more than
// once a day from every installed copy of this app.
constexpr qint64 kRefreshIntervalSecs = 24 * 60 * 60;
} // namespace

StoragePriceService& StoragePriceService::instance()
{
	static StoragePriceService s;
	return s;
}

StoragePriceService::StoragePriceService(QObject* parent)
	: QObject(parent)
{
	m_nam = new QNetworkAccessManager(this);
}

double StoragePriceService::pricePerTbUsd() const
{
	const double cached = AppSettings::instance()
	                           .value(QStringLiteral("pricing/sataHddPricePerTbUsd"), 0.0)
	                           .toDouble();
	return cached > 0.0 ? cached : kFallbackPricePerTbUsd;
}

bool StoragePriceService::hasLivePrice() const
{
	return AppSettings::instance()
	           .value(QStringLiteral("pricing/sataHddPricePerTbUsd"), 0.0)
	           .toDouble()
	       > 0.0;
}

QDateTime StoragePriceService::sourceUpdatedAt() const
{
	return QDateTime::fromString(
	    AppSettings::instance().value(QStringLiteral("pricing/sourceUpdatedAt")).toString(),
	    Qt::ISODate);
}

void StoragePriceService::refreshIfStale()
{
	if (m_fetchInFlight)
		return;

	const QDateTime lastFetch = QDateTime::fromString(
	    AppSettings::instance().value(QStringLiteral("pricing/fetchedAt")).toString(), Qt::ISODate);
	if (lastFetch.isValid()
	        && lastFetch.secsTo(QDateTime::currentDateTimeUtc()) < kRefreshIntervalSecs)
		return;

	m_fetchInFlight = true;

	QNetworkRequest req{ QUrl(QStringLiteral("https://datacenterdisk.com/api/v1/categories")) };
	req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MediaCurator"));

	QNetworkReply* reply = m_nam->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		m_fetchInFlight = false;
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			qWarning() << "StoragePriceService: price fetch failed:" << reply->errorString();
			return;
		}

		const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
		const QJsonArray  categories = root.value(QStringLiteral("data")).toObject()
		                                    .value(QStringLiteral("categories")).toArray();

		double price = 0.0;
		for (const QJsonValue& v : categories) {
			const QJsonObject cat = v.toObject();
			if (cat.value(QStringLiteral("slug")).toString() == QLatin1String("sata-hdd")) {
				price = cat.value(QStringLiteral("avg_price_per_tb")).toDouble();
				break;
			}
		}

		if (price <= 0.0) {
			qWarning() << "StoragePriceService: response had no usable sata-hdd price"
			              " — keeping previous value";
			return;
		}

		const QString remoteUpdatedAt = root.value(QStringLiteral("meta")).toObject()
		                                     .value(QStringLiteral("updated_at")).toString();

		auto& settings = AppSettings::instance();
		settings.setValue(QStringLiteral("pricing/sataHddPricePerTbUsd"), price);
		settings.setValue(QStringLiteral("pricing/sourceUpdatedAt"), remoteUpdatedAt);
		settings.setValue(QStringLiteral("pricing/fetchedAt"),
		                   QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

		emit priceRefreshed();
	});
}

void StoragePriceService::recordSavings(qint64 deltaBytes)
{
	if (deltaBytes <= 0)
		return;

	const double dollars    = (static_cast<double>(deltaBytes) / 1.0e12) * pricePerTbUsd();
	const qint64 deltaCents = static_cast<qint64>(dollars * 100.0 + 0.5);
	if (deltaCents > 0)
		AppSettings::instance().addMoneySavedCentsUsd(deltaCents);
	emit moneySavedChanged();
}

void StoragePriceService::recordManualDeletion(qint64 deltaBytes)
{
	if (deltaBytes <= 0)
		return;

	const double dollars    = (static_cast<double>(deltaBytes) / 1.0e12) * pricePerTbUsd();
	const qint64 deltaCents = static_cast<qint64>(dollars * 100.0 + 0.5);

	auto& settings = AppSettings::instance();
	settings.addManualDeletedBytes(deltaBytes);
	if (deltaCents > 0)
		settings.addManualMoneySavedCentsUsd(deltaCents);
	emit moneySavedChanged();
}

qint64 StoragePriceService::manualDeletedBytes() const
{
	return AppSettings::instance().manualDeletedBytes();
}

qint64 StoragePriceService::manualMoneySavedCentsUsd() const
{
	return AppSettings::instance().manualMoneySavedCentsUsd();
}

qint64 StoragePriceService::moneySavedCentsUsd() const
{
	auto& settings = AppSettings::instance();

	const bool backfilled =
	    settings.value(QStringLiteral("stats/moneySavedBackfilled"), false).toBool();
	if (!backfilled) {
		const qint64 reclaimed = settings.reclaimedBytes();
		if (reclaimed > 0) {
			const double dollars = (static_cast<double>(reclaimed) / 1.0e12) * pricePerTbUsd();
			const qint64 cents   = static_cast<qint64>(dollars * 100.0 + 0.5);
			settings.addMoneySavedCentsUsd(cents);
		}
		settings.setValue(QStringLiteral("stats/moneySavedBackfilled"), true);
	}

	return settings.moneySavedCentsUsd();
}

} // namespace Mc
