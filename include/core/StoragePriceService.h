#pragma once
#include <QDateTime>
#include <QObject>

class QNetworkAccessManager;

namespace Mc {

/**
 * StoragePriceService — tracks an estimated $/TB storage price, used to turn
 * the lifetime reclaimed-bytes counter (AppSettings::reclaimedBytes()) into a
 * "money saved" figure for the status bar.
 *
 * Price comes from DatacenterDisk.com's free, unauthenticated API
 * (GET https://datacenterdisk.com/api/v1/categories, the "sata-hdd" category's
 * avg_price_per_tb — see https://datacenterdisk.com/api-docs). Their terms
 * require linking back to them wherever the data is used; the "Money Saved"
 * status-bar tooltip does this.
 *
 * The price is fetched at most once a day (refreshIfStale()) and cached in
 * AppSettings. recordSavings() — called right alongside
 * AppSettings::addReclaimedBytes() when a mux job finishes — only does
 * arithmetic against the already-cached price, so job completion never waits
 * on network I/O and is safe to call from whatever thread that happens on.
 * If nothing has ever been fetched (fresh install, offline machine, or the
 * service is ever unreachable/gone) kFallbackPricePerTbUsd is used instead so
 * the feature still shows a number.
 */
class StoragePriceService : public QObject
{
	Q_OBJECT
public:
	static StoragePriceService& instance();

	// Refreshes the cached price if it's missing or older than one day.
	// Fire-and-forget async GET; safe to call repeatedly (an in-flight
	// request is not duplicated).
	void refreshIfStale();

	// Current price to use, in USD per TB — the cached value, or
	// kFallbackPricePerTbUsd if nothing has been fetched yet.
	[[nodiscard]] double pricePerTbUsd() const;

	// True once a real (non-fallback) price has been cached.
	[[nodiscard]] bool hasLivePrice() const;

	// Timestamp DatacenterDisk.com last recomputed this price (their
	// meta.updated_at), for the tooltip. Invalid if never fetched.
	[[nodiscard]] QDateTime sourceUpdatedAt() const;

	// Adds to the lifetime "money saved" counter using the currently cached
	// price. Pure arithmetic + a settings write, no network call.
	void recordSavings(qint64 deltaBytes);

	// Same, but for a file manually deleted from within the app (see
	// AppSettings::addManualDeletedBytes for why this is a separate counter).
	void recordManualDeletion(qint64 deltaBytes);

	// Lazily backfills the counter, once, from the pre-existing lifetime
	// AppSettings::reclaimedBytes() total at whatever price is currently
	// cached (or the fallback) — otherwise a user who reclaimed space before
	// this feature existed would see $0.00 here despite a non-zero lifetime
	// total everywhere else (e.g. the leaderboard's retroactive column).
	// Guarded by a settings flag so it only ever applies once.
	[[nodiscard]] qint64 moneySavedCentsUsd() const;

	[[nodiscard]] qint64 manualDeletedBytes() const;
	[[nodiscard]] qint64 manualMoneySavedCentsUsd() const;

signals:
	// Emitted after a successful refresh so the UI can update its tooltip.
	void priceRefreshed();

	// Emitted after recordSavings()/recordManualDeletion() so the UI can
	// refresh the status-bar figures immediately rather than waiting for the
	// next unrelated refresh.
	void moneySavedChanged();

private:
	explicit StoragePriceService(QObject* parent = nullptr);

	QNetworkAccessManager* m_nam           = nullptr;
	bool                   m_fetchInFlight = false;
};

} // namespace Mc
