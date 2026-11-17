#pragma once
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace Mc {

// Checks GitHub for a newer Hot DAMN release and downloads its installer.
// Modelled on MediaCurator's UpdateChecker (src/core), with one difference:
// both apps publish to the same repository, and /releases/latest is always
// MediaCurator's, so this lists the releases and takes the newest one tagged
// hotdamn-vX.Y.Z.
//
// check(silent=true) is the automatic check at startup: at most once a day
// (Hot DAMN starts for every NFO opened), and quiet about a release the user
// skipped. check(silent=false) is Help > Check for Updates: always asks
// GitHub and always reports.
class McHotDamnUpdater : public QObject
{
	Q_OBJECT
public:
	static McHotDamnUpdater& instance();

	void check(bool silent);
	void skipVersion(const QString& version);

	// Downloads the installer and emits installerReady(path). The caller must
	// close its window first and launch the installer only after that (see
	// main.cpp): the installer overwrites HotDamn.exe and its DLLs.
	void download(const QString& installerUrl);
	void cancelDownload();

	// Starts the installer elevated, with its normal wizard (not silent: a
	// silent install skips locked files without a word). The wizard's finish
	// page starts Hot DAMN again. Windows only.
	bool launchInstaller(const QString& path);

signals:
	void updateAvailable(QString version, QString htmlUrl, QString releaseNotes, QString installerUrl, bool silent);
	void upToDate(bool silent);
	void checkFailed(QString error, bool silent);

	void downloadProgress(qint64 received, qint64 total);
	void downloadFailed(QString error);
	void installerReady(QString path);

private:
	explicit McHotDamnUpdater(QObject* parent = nullptr);
	void onCheckFinished(QNetworkReply* reply, bool silent);

	QNetworkAccessManager* m_nam           = nullptr;
	QNetworkReply*         m_downloadReply = nullptr;
	bool                   m_busy          = false;
};

} // namespace Mc
