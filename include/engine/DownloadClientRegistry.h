#pragma once
#include "engine/DownloadClient.h"

#include <QHash>
#include <QList>

namespace Mc {

/**
 * DownloadClientRegistry — owns every configured DownloadClient (NZBGet,
 * SABnzbd; future providers register here too) and presents one merged view
 * so callers (McMainWindow, the queue band/dialog) never touch a concrete
 * provider directly. Adding a new provider is registering it in the
 * constructor — no other call site changes.
 */
class DownloadClientRegistry : public QObject
{
	Q_OBJECT
public:
	static DownloadClientRegistry& instance();

	[[nodiscard]] QList<DownloadQueueItem> allQueueItems() const { return m_allItems; }
	[[nodiscard]] bool anyConfigured() const;

	// Re-reads settings for every registered client (called once at startup
	// and again whenever Settings are saved).
	void reconfigureAll();

signals:
	void queueChanged();
	void downloadsCompleted(QStringList names, QString providerId);

private:
	explicit DownloadClientRegistry(QObject* parent = nullptr);

	QList<DownloadClient*>              m_clients;
	QHash<QString, QList<DownloadQueueItem>> m_itemsByProvider;
	QList<DownloadQueueItem>            m_allItems;
};

} // namespace Mc
