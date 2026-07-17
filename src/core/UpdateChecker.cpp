#include "core/UpdateChecker.h"
#include "core/AppSettings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QVersionNumber>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace Mc {

namespace {
constexpr const char* kApiUrl = "https://api.github.com/repos/bleze/MediaCurator/releases/latest";
constexpr qint64 kMinIntervalMs = 24LL * 60 * 60 * 1000;

QVersionNumber parseTag(const QString& tagName)
{
	QString t = tagName;
	if (t.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
		t.remove(0, 1);
	return QVersionNumber::fromString(t);
}

// The Windows CI job packages exactly one NSIS installer per release
// (MediaCurator-<version>-win64.exe) — pick the first .exe asset.
QString findInstallerUrl(const QJsonObject& release)
{
	const QJsonArray assets = release.value(QStringLiteral("assets")).toArray();
	for (const QJsonValue& v : assets) {
		const QJsonObject asset = v.toObject();
		const QString name = asset.value(QStringLiteral("name")).toString();
		if (name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive))
			return asset.value(QStringLiteral("browser_download_url")).toString();
	}
	return {};
}
} // namespace

UpdateChecker& UpdateChecker::instance()
{
	static UpdateChecker s_instance;
	return s_instance;
}

UpdateChecker::UpdateChecker(QObject* parent)
	: QObject(parent)
{
	m_nam = new QNetworkAccessManager(this);
}

void UpdateChecker::check(bool silent)
{
	if (m_busy)
		return;

	if (silent) {
		const qint64 lastCheckMs = AppSettings::instance().value("update/lastCheckMs", 0).toLongLong();
		if (QDateTime::currentMSecsSinceEpoch() - lastCheckMs < kMinIntervalMs)
			return;
	}

	m_busy = true;
	QNetworkRequest req{QUrl(QString::fromLatin1(kApiUrl))};
	req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MediaCurator"));
	req.setRawHeader("Accept", "application/vnd.github+json");

	QNetworkReply* reply = m_nam->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply, silent] {
		onReplyFinished(reply, silent);
	});
}

void UpdateChecker::onReplyFinished(QNetworkReply* reply, bool silent)
{
	m_busy = false;
	reply->deleteLater();

	if (reply->error() != QNetworkReply::NoError) {
		emit checkFailed(reply->errorString(), silent);
		return;
	}

	const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
	const QString tagName = obj.value(QStringLiteral("tag_name")).toString();
	if (tagName.isEmpty()) {
		emit checkFailed(tr("Unexpected response from GitHub"), silent);
		return;
	}

	AppSettings::instance().setValue("update/lastCheckMs", QDateTime::currentMSecsSinceEpoch());

	const QVersionNumber latest  = parseTag(tagName);
	const QVersionNumber current = QVersionNumber::fromString(QCoreApplication::applicationVersion());

	if (QVersionNumber::compare(latest, current) <= 0) {
		emit upToDate(silent);
		return;
	}

	if (silent) {
		const QString skipped = AppSettings::instance().value("update/skipVersion").toString();
		if (skipped == tagName)
			return;
	}

	emit updateAvailable(tagName,
	                      obj.value(QStringLiteral("html_url")).toString(),
	                      obj.value(QStringLiteral("body")).toString(),
	                      findInstallerUrl(obj),
	                      silent);
}

void UpdateChecker::skipVersion(const QString& tagName)
{
	AppSettings::instance().setValue("update/skipVersion", tagName);
}

void UpdateChecker::downloadAndInstall(const QString& installerUrl)
{
	if (m_downloadReply || installerUrl.isEmpty())
		return;

	QNetworkRequest req{QUrl(installerUrl)};
	req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MediaCurator"));
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
	                 QNetworkRequest::NoLessSafeRedirectPolicy);

	m_downloadReply = m_nam->get(req);
	connect(m_downloadReply, &QNetworkReply::downloadProgress,
	        this, &UpdateChecker::downloadProgress);
	connect(m_downloadReply, &QNetworkReply::finished, this, [this, installerUrl] {
		QNetworkReply* reply = m_downloadReply;
		m_downloadReply = nullptr;
		reply->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			if (reply->error() != QNetworkReply::OperationCanceledError)
				emit downloadFailed(reply->errorString());
			return;
		}

		// Use the original request URL, not reply->url(): GitHub's
		// browser_download_url redirects to a signed storage URL whose path
		// is an opaque object key, not the asset's filename.
		const QString assetName = QFileInfo(QUrl(installerUrl).path()).fileName();
		const QString savePath  = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
		                              .filePath(assetName.isEmpty()
		                                            ? QStringLiteral("MediaCurator-update.exe")
		                                            : assetName);

		QFile f(savePath);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			emit downloadFailed(tr("Could not save the installer to %1").arg(savePath));
			return;
		}
		f.write(reply->readAll());
		f.close();

		if (launchInstaller(savePath))
			emit installerLaunched();
		// launchInstaller() emits downloadFailed() itself on failure.
	});
}

void UpdateChecker::cancelDownload()
{
	if (m_downloadReply) m_downloadReply->abort();
}

bool UpdateChecker::launchInstaller(const QString& path)
{
#ifdef Q_OS_WIN
	SHELLEXECUTEINFOW sei{};
	sei.cbSize = sizeof(sei);
	sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
	// The installer writes to Program Files and touches PATH, so it needs
	// elevation. Only ShellExecuteEx (not QProcess/CreateProcess) can trigger
	// the UAC consent prompt for a non-elevated caller.
	sei.lpVerb = L"runas";
	const std::wstring filePath = path.toStdWString();
	sei.lpFile       = filePath.c_str();
	// Deliberately NOT /S. ShellExecuteExW returns as soon as the elevated
	// installer process exists, racing this process's own closeEvent teardown
	// (job-queue drain, thread shutdown) for the lock on MediaCurator.exe/its
	// DLLs — the old uninstaller (CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL)
	// runs against those files first. A silent install can't prompt when a
	// file is still locked, so it silently skips/no-ops and reports success,
	// leaving the old version in place with no visible error. The normal
	// wizard UI shows real progress and, if it does hit the lock, a blocking
	// "close the application and click Retry" prompt instead of a silent
	// partial install.
	sei.lpParameters = nullptr;
	sei.nShow        = SW_SHOWNORMAL;

	if (!ShellExecuteExW(&sei)) {
		const DWORD err = GetLastError();
		if (err == ERROR_CANCELLED)
			emit downloadFailed(tr("Update cancelled — the elevation prompt was declined."));
		else
			emit downloadFailed(tr("Could not launch the installer (error %1). "
			                        "You can run it manually from %2.").arg(err).arg(path));
		return false;
	}
	if (sei.hProcess) CloseHandle(sei.hProcess);
	return true;
#else
	Q_UNUSED(path);
	emit downloadFailed(tr("Automatic install isn't supported on this platform."));
	return false;
#endif
}

} // namespace Mc
