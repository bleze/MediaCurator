#pragma once
#include "engine/DownloadClient.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Mc {

/**
 * SabnzbdClient — polls a configured SABnzbd instance's HTTP API
 * (http://host:port/api) every 5s for the active queue (mode=queue) and
 * completed downloads (mode=history). Inert (timer stopped) until
 * DownloadIntegrationSettings::sabnzbdConfig() reports enabled + a host.
 * Unlike NZBGet's JSON-RPC, SABnzbd is plain GET requests authenticated by
 * an apikey query parameter rather than HTTP basic auth.
 */
class SabnzbdClient : public DownloadClient
{
	Q_OBJECT
public:
	explicit SabnzbdClient(QObject* parent = nullptr);

	[[nodiscard]] QString providerId() const override  { return QStringLiteral("sabnzbd"); }
	[[nodiscard]] QString displayName() const override { return QStringLiteral("SABnzbd"); }
	[[nodiscard]] bool    isConfigured() const override;

	void reconfigure() override;
	void pollNow() override;

private:
	QNetworkReply* getApi(const QString& mode);
	void pollQueue();
	void pollHistory();
	void onQueueReply(QNetworkReply* reply);
	void onHistoryReply(QNetworkReply* reply);

	QNetworkAccessManager* m_nam   = nullptr;
	QTimer*                m_timer = nullptr;
	bool                   m_queueInFlight   = false;
	bool                   m_historyInFlight = false;
};

} // namespace Mc
