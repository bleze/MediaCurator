#pragma once
#include <QObject>
#include <QString>
#include <QVariant>
#include <QJsonObject>

namespace Mc {

/**
 * AppSettings — JSON-backed key/value store for application settings.
 *
 * Owns settings.json in AppDataLocation. The file has two top-level sections:
 *   "profile"  — managed by UserProfile (curation policy)
 *   "app"      — everything else (folders, UI flags, panel state)
 *
 * Geometry/splitter state (opaque QByteArray blobs) stay in QSettings (INI).
 *
 * Call load() once at startup before constructing any UI. setValue() persists
 * immediately; save() is called internally.
 */
class AppSettings : public QObject {
	Q_OBJECT
public:
	static AppSettings& instance();

	static QString filePath();
	static QString geometryFilePath();

	void load();
	void save();

	// "app" section ────────────────────────────────────────────────────────────
	QVariant value(const QString& key, const QVariant& defaultValue = {}) const;
	void     setValue(const QString& key, const QVariant& value);

	// Lifetime "space reclaimed" counter — paired with an HMAC so hand-editing
	// settings.json to inflate it is detected and punished by zeroing it out.
	// Not real security (this is open-source, the key is right there in
	// AppSettings.cpp) — it only stops casually editing the number in a text
	// editor. reclaimedBytes() self-heals (resets to 0) on a failed check.
	qint64 reclaimedBytes();
	void   addReclaimedBytes(qint64 deltaBytes);

	// Re-seeds the counter from a trusted external value (e.g. this player's
	// last dreamlo-posted score) after the integrity check above reset it to
	// 0 — so a false positive doesn't permanently erase real history.
	void restoreReclaimedBytes(qint64 bytesValue);

	// Lifetime "money saved" counter, in USD cents — derived from reclaimed
	// bytes at whatever storage price was cached at the time each job
	// finished (see StoragePriceService). Plain counter, no HMAC: it's an
	// estimate, not something posted to the dreamlo leaderboard, so there's
	// nothing worth protecting against hand-editing.
	qint64 moneySavedCentsUsd();
	void   addMoneySavedCentsUsd(qint64 deltaCents);

	// Lifetime bytes/money reclaimed by manually deleting files from within
	// the app (e.g. clearing out flagged duplicates), as opposed to lossless
	// track removal via mkvmerge. Tracked separately from the counters above
	// — and never submitted to the dreamlo leaderboard — because a manual
	// delete is trivially game-able (copy a file, then "reclaim" it) in a way
	// mkvmerge track removal isn't. Whether these get folded into the
	// figures shown in the status bar is the user's call
	// (settings/includeManualDeletesInTotals in McSettingsDialog).
	qint64 manualDeletedBytes();
	void   addManualDeletedBytes(qint64 deltaBytes);
	qint64 manualMoneySavedCentsUsd();
	void   addManualMoneySavedCentsUsd(qint64 deltaCents);

	// "profile" section — used by UserProfile ─────────────────────────────────
	QJsonObject profileSection() const;
	void        setProfileSection(const QJsonObject& obj);

	// Raw root — used during migration from old flat-file format
	QJsonObject rawRoot() const { return m_root; }

private:
	AppSettings() = default;
	QJsonObject m_root;
};

} // namespace Mc
