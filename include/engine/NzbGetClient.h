#pragma once
#include "engine/DownloadClient.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace Mc {

/**
 * NzbGetClient — polls a configured NZBGet instance's JSON-RPC API
 * (http://host:port/jsonrpc) every 5s for the active queue (`listgroups`)
 * and completed downloads (`history`). Inert (timer stopped) until
 * DownloadIntegrationSettings::nzbgetConfig() reports enabled + a host.
 */
class NzbGetClient : public DownloadClient
{
	Q_OBJECT
public:
	explicit NzbGetClient(QObject* parent = nullptr);

	[[nodiscard]] QString providerId() const override  { return QStringLiteral("nzbget"); }
	[[nodiscard]] QString displayName() const override { return QStringLiteral("NZBGet"); }
	[[nodiscard]] bool    isConfigured() const override;

	void reconfigure() override;
	void pollNow() override;

private:
	QNetworkReply* postRpc(const QString& method, const QByteArray& paramsJson);
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
