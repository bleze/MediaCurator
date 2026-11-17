#include "McHotDamnUpdater.h"

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
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVersionNumber>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

namespace Mc {

namespace {
// Newest first. 100 is the API's maximum page size; MediaCurator would need
// that many releases after the last Hot DAMN one for it to fall off the page.
constexpr const char* kReleasesUrl  = "https://api.github.com/repos/bleze/MediaCurator/releases?per_page=100";
constexpr const char* kTagPrefix    = "hotdamn-v";
constexpr const char* kKeySkip      = "update/skipVersion";
constexpr const char* kKeyLastCheck = "update/lastAutomaticCheck";
constexpr qint64      kAutomaticCheckIntervalSecs = 24 * 60 * 60;

QByteArray userAgent()
{
	return "HotDamn/" + QCoreApplication::applicationVersion().toLatin1();
}

// The workflow attaches exactly one installer, HotDamn-Setup-<version>.exe.
QString findInstallerUrl(const QJsonObject& release)
{
	for (const QJsonValue& v : release.value(QStringLiteral("assets")).toArray()) {
		const QJsonObject asset = v.toObject();
		const QString name = asset.value(QStringLiteral("name")).toString();
		if (name.startsWith(QStringLiteral("HotDamn-Setup-"), Qt::CaseInsensitive)
		    && name.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive))
			return asset.value(QStringLiteral("browser_download_url")).toString();
	}
	return {};
}
} // namespace

McHotDamnUpdater& McHotDamnUpdater::instance()
{
	static McHotDamnUpdater s_instance;
	return s_instance;
}

McHotDamnUpdater::McHotDamnUpdater(QObject* parent)
	: QObject(parent)
{
	m_nam = new QNetworkAccessManager(this);
}

void McHotDamnUpdater::check(bool silent)
{
	if (m_busy) return;
	if (silent) {
		QSettings settings;
		const qint64 now  = QDateTime::currentSecsSinceEpoch();
		const qint64 last = settings.value(kKeyLastCheck, 0).toLongLong();
		if (now - last < kAutomaticCheckIntervalSecs) return;
		// Only the automatic check moves this: a manual check must not
		// postpone the next automatic one.
		settings.setValue(kKeyLastCheck, now);
	}

	m_busy = true;
	QNetworkRequest req{ QUrl(QString::fromLatin1(kReleasesUrl)) };
	req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
	req.setRawHeader("Accept", "application/vnd.github+json");
	req.setTransferTimeout(20000);
	QNetworkReply* reply = m_nam->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply, silent]() { onCheckFinished(reply, silent); });
}

void McHotDamnUpdater::onCheckFinished(QNetworkReply* reply, bool silent)
{
	m_busy = false;
	reply->deleteLater();
	if (reply->error() != QNetworkReply::NoError) {
		emit checkFailed(reply->errorString(), silent);
		return;
	}

	const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
	if (!doc.isArray()) {
		emit checkFailed(tr("Unexpected response from GitHub."), silent);
		return;
	}

	// Newest Hot DAMN release by version, skipping drafts and pre-releases
	// (unlike /releases/latest, the list includes pre-releases).
	QJsonObject newest;
	QVersionNumber newestVersion;
	for (const QJsonValue& v : doc.array()) {
		const QJsonObject release = v.toObject();
		const QString tag = release.value(QStringLiteral("tag_name")).toString();
		if (!tag.startsWith(QLatin1String(kTagPrefix)) || release.value(QStringLiteral("draft")).toBool()
		    || release.value(QStringLiteral("prerelease")).toBool())
			continue;
		const QVersionNumber version = QVersionNumber::fromString(tag.mid(int(qstrlen(kTagPrefix))));
		if (!version.isNull() && version > newestVersion) {
			newestVersion = version;
			newest        = release;
		}
	}

	const QVersionNumber current = QVersionNumber::fromString(QCoreApplication::applicationVersion());
	if (newestVersion.isNull() || newestVersion <= current) {
		emit upToDate(silent);
		return;
	}

	const QString version = newestVersion.toString();
	if (silent && QSettings().value(kKeySkip).toString() == version) return;

	emit updateAvailable(version, newest.value(QStringLiteral("html_url")).toString(),
	                     newest.value(QStringLiteral("body")).toString(), findInstallerUrl(newest), silent);
}

void McHotDamnUpdater::skipVersion(const QString& version)
{
	QSettings().setValue(kKeySkip, version);
}

void McHotDamnUpdater::download(const QString& installerUrl)
{
	if (m_downloadReply || installerUrl.isEmpty()) return;

	QNetworkRequest req{ QUrl(installerUrl) };
	req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	m_downloadReply = m_nam->get(req);
	connect(m_downloadReply, &QNetworkReply::downloadProgress, this, &McHotDamnUpdater::downloadProgress);
	connect(m_downloadReply, &QNetworkReply::finished, this, [this, installerUrl]() {
		QNetworkReply* reply = m_downloadReply;
		m_downloadReply = nullptr;
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			if (reply->error() != QNetworkReply::OperationCanceledError)
				emit downloadFailed(reply->errorString());
			return;
		}

		// The request URL, not reply->url(): GitHub redirects to a storage URL
		// whose path is not the file name.
		QString name = QFileInfo(QUrl(installerUrl).path()).fileName();
		if (name.isEmpty()) name = QStringLiteral("HotDamn-update.exe");
		const QString path = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(name);
		QFile f(path);
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			emit downloadFailed(tr("Could not save the installer to %1").arg(QDir::toNativeSeparators(path)));
			return;
		}
		f.write(reply->readAll());
		f.close();
		emit installerReady(path);
	});
}

void McHotDamnUpdater::cancelDownload()
{
	if (m_downloadReply) m_downloadReply->abort();
}

bool McHotDamnUpdater::launchInstaller(const QString& path)
{
#ifdef Q_OS_WIN
	// "runas": the installer writes to Program Files and HKLM, and only
	// ShellExecuteEx can raise the UAC prompt for a non-elevated caller.
	SHELLEXECUTEINFOW sei{};
	sei.cbSize = sizeof(sei);
	sei.lpVerb = L"runas";
	const std::wstring file = QDir::toNativeSeparators(path).toStdWString();
	sei.lpFile = file.c_str();
	sei.nShow  = SW_SHOWNORMAL;
	return ShellExecuteExW(&sei) != FALSE;
#else
	Q_UNUSED(path);
	return false;
#endif
}

} // namespace Mc
