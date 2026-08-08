#include "engine/DownloadClientRegistry.h"
#include "engine/NzbGetClient.h"

#include <utility>

namespace Mc {

DownloadClientRegistry& DownloadClientRegistry::instance()
{
	static DownloadClientRegistry s_instance;
	return s_instance;
}

DownloadClientRegistry::DownloadClientRegistry(QObject* parent)
	: QObject(parent)
{
	auto* nzbget = new NzbGetClient(this);
	m_clients << nzbget;

	for (DownloadClient* client : std::as_const(m_clients)) {
		connect(client, &DownloadClient::queueUpdated, this, [this, client](QList<DownloadQueueItem> items) {
			m_itemsByProvider.insert(client->providerId(), items);

			m_allItems.clear();
			for (auto it = m_itemsByProvider.cbegin(); it != m_itemsByProvider.cend(); ++it)
				m_allItems << it.value();

			emit queueChanged();
		});
		connect(client, &DownloadClient::downloadsCompleted, this, [this, client](QStringList names) {
			emit downloadsCompleted(names, client->providerId());
		});
	}
}

bool DownloadClientRegistry::anyConfigured() const
{
	for (DownloadClient* client : m_clients)
		if (client->isConfigured())
			return true;
	return false;
}

void DownloadClientRegistry::reconfigureAll()
{
	for (DownloadClient* client : std::as_const(m_clients))
		client->reconfigure();
}

} // namespace Mc
