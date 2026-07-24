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
 *   check(/*silent=* /true)  — startup path. Always hits the network, but
 *   stays quiet if the latest release has been explicitly skipped by the
 *   user (skipVersion()) — so once dismissed, a given release won't be
 *   re-prompted on every launch, while the *next* release still will be.
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

	// Download installerUrl (a release asset from updateAvailable's installerUrl
	// param) and, once complete, emit installerReady(path) instead of launching
	// it immediately. The elevated installer (and, via
	// CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL, the *old* version's silent
	// uninstall step) will try to overwrite MediaCurator.exe/its DLLs as soon as
	// it starts — so the caller must fully tear down (release those file locks)
	// before calling launchInstaller(), not just before closing the window.
	// See main.cpp: it calls launchInstaller() only after the main window has
	// been destroyed. Windows-only; no-op elsewhere.
	void downloadAndInstall(const QString& installerUrl);
	void cancelDownload();

	// Launches path elevated (normal NSIS wizard UI, not silent — see the
	// comment on the /S removal below for why). Public so the caller can defer
	// this until its own teardown has actually released its file locks, rather
	// than racing it the way emitting installerReady() and launching in the
	// same step used to.
	bool launchInstaller(const QString& path);

signals:
	// installerUrl is the matching Windows installer asset's browser_download_url,
	// or empty if the release has no such asset (or on non-Windows platforms).
	void updateAvailable(QString version, QString htmlUrl, QString releaseNotes,
	                      QString installerUrl, bool silent);
	void upToDate(bool silent);
	void checkFailed(QString error, bool silent);

	void downloadProgress(qint64 received, qint64 total);
	void downloadFailed(QString error);
	// path is the downloaded installer, ready to hand to launchInstaller() once
	// the receiver has released its own file locks on the install directory.
	void installerReady(QString path);

private:
	explicit UpdateChecker(QObject* parent = nullptr);

	void onReplyFinished(QNetworkReply* reply, bool silent);

	QNetworkAccessManager* m_nam           = nullptr;
	QNetworkReply*         m_downloadReply = nullptr;
	bool                   m_busy          = false;
};

} // namespace Mc
