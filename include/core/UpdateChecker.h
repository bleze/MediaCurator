#pragma once
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace Mc {

/**
 * UpdateChecker — checks GitHub Releases for a newer published version.
 *
 * Hits the unauthenticated GitHub REST API once per call:
 *   GET https://api.github.com/repos/bleze/MediaCurator/releases/latest
 * (drafts and pre-releases are never returned by this endpoint)
 *
 * Two call modes:
 *   check(/*silent=* /true)  — startup path. No-ops if checked within the last
 *   24h or if the latest release has been explicitly skipped by the user.
 *   check(/*silent=* /false) — "Check for Updates…" menu path. Always hits the
 *   network and always reports a result, skip list notwithstanding.
 *
 * Every signal carries back the `silent` flag from the triggering call so a
 * single slot can decide whether a result should be surfaced to the user
 * (e.g. suppress "you're up to date" chatter for the silent startup check).
 */
class UpdateChecker : public QObject
{
	Q_OBJECT
public:
	static UpdateChecker& instance();

	void check(bool silent);

	// Persists tagName so silent checks stop reporting this release.
	// Manual checks ignore the skip list.
	void skipVersion(const QString& tagName);

signals:
	void updateAvailable(QString version, QString htmlUrl, QString releaseNotes, bool silent);
	void upToDate(bool silent);
	void checkFailed(QString error, bool silent);

private:
	explicit UpdateChecker(QObject* parent = nullptr);

	void onReplyFinished(QNetworkReply* reply, bool silent);

	QNetworkAccessManager* m_nam  = nullptr;
	bool                   m_busy = false;
};

} // namespace Mc
