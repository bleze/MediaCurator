#include "core/DownloadIntegrationSettings.h"
#include "core/AppSettings.h"

#include <QByteArray>
#include <QJsonObject>

namespace Mc {

namespace {
const QLatin1String kDownloadsKey("downloads");
const QLatin1String kNzbGetKey("nzbget");
const QLatin1String kSabnzbdKey("sabnzbd");

// NOT real security — this only keeps the password from showing up as plain
// text to a casual glance at settings.json (screenshots, screen-sharing a
// support session, etc.). The key lives right here in this open-source repo,
// so anyone motivated can trivially reverse it; real protection would mean OS
// keychain integration (Credential Manager / Keychain / Secret Service), which
// this cross-platform app has no infrastructure for. Same spirit as
// AppSettings::reclaimedBytes()'s HMAC — a deterrent, not a guarantee.
constexpr char kObfuscationKey[] = "MediaCurator-nzbget-v1";

QString obfuscate(const QString& plain)
{
	if (plain.isEmpty())
		return {};
	const QByteArray input = plain.toUtf8();
	const int        keyLen = static_cast<int>(sizeof(kObfuscationKey) - 1);
	QByteArray out;
	out.reserve(input.size());
	for (int i = 0; i < input.size(); ++i)
		out.append(static_cast<char>(input[i] ^ kObfuscationKey[i % keyLen]));
	return QString::fromLatin1(out.toBase64());
}

QString deobfuscate(const QString& encoded)
{
	if (encoded.isEmpty())
		return {};
	const QByteArray input  = QByteArray::fromBase64(encoded.toLatin1());
	const int        keyLen = static_cast<int>(sizeof(kObfuscationKey) - 1);
	QByteArray out;
	out.reserve(input.size());
	for (int i = 0; i < input.size(); ++i)
		out.append(static_cast<char>(input[i] ^ kObfuscationKey[i % keyLen]));
	return QString::fromUtf8(out);
}

QJsonObject downloadsObject()
{
	const QJsonObject app = AppSettings::instance().rawRoot().value(QStringLiteral("app")).toObject();
	return app.value(kDownloadsKey).toObject();
}

void saveDownloadsObject(const QJsonObject& obj)
{
	AppSettings::instance().setValue(QString(kDownloadsKey), obj);
}
}

NzbGetConfig DownloadIntegrationSettings::nzbgetConfig()
{
	const QJsonObject obj = downloadsObject().value(kNzbGetKey).toObject();

	NzbGetConfig config;
	config.enabled  = obj.value(QStringLiteral("enabled")).toBool(false);
	config.host     = obj.value(QStringLiteral("host")).toString();
	config.port     = obj.value(QStringLiteral("port")).toInt(6789);
	config.username = obj.value(QStringLiteral("username")).toString();
	config.password = deobfuscate(obj.value(QStringLiteral("password")).toString());
	return config;
}

void DownloadIntegrationSettings::setNzbgetConfig(const NzbGetConfig& config)
{
	QJsonObject nzbget;
	nzbget.insert(QStringLiteral("enabled"), config.enabled);
	nzbget.insert(QStringLiteral("host"), config.host);
	nzbget.insert(QStringLiteral("port"), config.port);
	nzbget.insert(QStringLiteral("username"), config.username);
	nzbget.insert(QStringLiteral("password"), obfuscate(config.password));

	QJsonObject downloads = downloadsObject();
	downloads.insert(kNzbGetKey, nzbget);
	saveDownloadsObject(downloads);
}

SabnzbdConfig DownloadIntegrationSettings::sabnzbdConfig()
{
	const QJsonObject obj = downloadsObject().value(kSabnzbdKey).toObject();

	SabnzbdConfig config;
	config.enabled = obj.value(QStringLiteral("enabled")).toBool(false);
	config.host    = obj.value(QStringLiteral("host")).toString();
	config.port    = obj.value(QStringLiteral("port")).toInt(8080);
	config.apiKey  = deobfuscate(obj.value(QStringLiteral("apiKey")).toString());
	return config;
}

void DownloadIntegrationSettings::setSabnzbdConfig(const SabnzbdConfig& config)
{
	QJsonObject sabnzbd;
	sabnzbd.insert(QStringLiteral("enabled"), config.enabled);
	sabnzbd.insert(QStringLiteral("host"), config.host);
	sabnzbd.insert(QStringLiteral("port"), config.port);
	sabnzbd.insert(QStringLiteral("apiKey"), obfuscate(config.apiKey));

	QJsonObject downloads = downloadsObject();
	downloads.insert(kSabnzbdKey, sabnzbd);
	saveDownloadsObject(downloads);
}

bool DownloadIntegrationSettings::autoQuickScanOnComplete()
{
	const QJsonObject downloads = downloadsObject();
	if (downloads.contains(QStringLiteral("autoQuickScan")))
		return downloads.value(QStringLiteral("autoQuickScan")).toBool(true);
	// Migrate pre-SABnzbd installs where this lived under the nzbget key.
	return downloads.value(kNzbGetKey).toObject().value(QStringLiteral("autoQuickScan")).toBool(true);
}

void DownloadIntegrationSettings::setAutoQuickScanOnComplete(bool enabled)
{
	QJsonObject downloads = downloadsObject();
	downloads.insert(QStringLiteral("autoQuickScan"), enabled);
	saveDownloadsObject(downloads);
}

bool DownloadIntegrationSettings::autoQuickAnalyzeOnComplete()
{
	const QJsonObject downloads = downloadsObject();
	if (downloads.contains(QStringLiteral("autoQuickAnalyze")))
		return downloads.value(QStringLiteral("autoQuickAnalyze")).toBool(true);
	// Migrate pre-SABnzbd installs where this lived under the nzbget key.
	return downloads.value(kNzbGetKey).toObject().value(QStringLiteral("autoQuickAnalyze")).toBool(true);
}

void DownloadIntegrationSettings::setAutoQuickAnalyzeOnComplete(bool enabled)
{
	QJsonObject downloads = downloadsObject();
	downloads.insert(QStringLiteral("autoQuickAnalyze"), enabled);
	saveDownloadsObject(downloads);
}

int DownloadIntegrationSettings::downloadingStorageGroup()
{
	return downloadsObject().value(QStringLiteral("downloadingStorageGroup")).toInt(0);
}

void DownloadIntegrationSettings::setDownloadingStorageGroup(int group)
{
	QJsonObject downloads = downloadsObject();
	downloads.insert(QStringLiteral("downloadingStorageGroup"), group);
	saveDownloadsObject(downloads);
}

int DownloadIntegrationSettings::downloadFinishedStorageGroup()
{
	return downloadsObject().value(QStringLiteral("downloadFinishedStorageGroup")).toInt(0);
}

void DownloadIntegrationSettings::setDownloadFinishedStorageGroup(int group)
{
	QJsonObject downloads = downloadsObject();
	downloads.insert(QStringLiteral("downloadFinishedStorageGroup"), group);
	saveDownloadsObject(downloads);
}

} // namespace Mc
