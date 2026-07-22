#pragma once
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

class QNetworkAccessManager;

namespace Mc {

struct HighscoreEntry {
	QString   name;
	qint64    score = 0;   // MB reclaimed
	QDateTime lastActive;  // dreamlo's "date" field — last submit time, NOT first-joined
	                       // (dreamlo overwrites it on every /add call). Invalid if unparsable.
};

/**
 * HighscoreClient — thin wrapper around the dreamlo.com free leaderboard API.
 * Singleton with one persistent main-thread QNetworkAccessManager (mirrors
 * UpdateChecker) — this is two occasional GET calls, not a bulk workload.
 *
 * isEnabled() reflects whether a private (write) code was compiled in via the
 * gitignored McHighscoreConfig.local.h. When false, submitScore() is a no-op;
 * callers are expected to gate all UI on isEnabled() too (see McMainWindow).
 */
class HighscoreClient : public QObject
{
	Q_OBJECT
public:
	static HighscoreClient& instance();

	[[nodiscard]] bool isEnabled() const;

	// name is percent-encoded internally. score must be >= 0 (whole MB).
	void submitScore(const QString& name, qint64 score);

	// Fetches the full public leaderboard. Safe to call with no private code
	// configured (the read-only public code is always compiled in).
	void fetchLeaderboard();

signals:
	void scoreSubmitted(bool ok);
	void leaderboardReady(QList<Mc::HighscoreEntry> entries);
	void leaderboardFetchFailed(QString error);

private:
	explicit HighscoreClient(QObject* parent = nullptr);

	static QList<HighscoreEntry> parseLeaderboard(const QByteArray& json);

	QNetworkAccessManager* m_nam               = nullptr;
	bool                   m_submitInFlight    = false;
	bool                   m_fetchInFlight     = false;
	bool                   m_fetchPendingRerun = false;   // a fetch was requested while one was in flight
};

} // namespace Mc

Q_DECLARE_METATYPE(Mc::HighscoreEntry)
