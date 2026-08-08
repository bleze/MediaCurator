#pragma once
#include <QDialog>
#include <QList>
#include <QStringList>

#include "engine/DownloadClient.h"

class QTableWidget;

namespace Mc {

// McDownloadQueueDialog — full live queue view across every configured
// download-client provider (NZBGet today), opened by clicking
// McDownloadQueueBand. Purely a display widget — McMainWindow owns data flow.
class McDownloadQueueDialog : public QDialog
{
	Q_OBJECT
public:
	// editionTokens: UserProfile::editionTokens() — same list the library cards use
	// to detect edition/3D from a filename, applied here to raw release names so
	// the queue shows the same 4K/3D/Edition badges as the library, pre-scan.
	explicit McDownloadQueueDialog(const QList<DownloadQueueItem>& items,
	                                const QStringList& editionTokens, QWidget* parent = nullptr);

	void setItems(const QList<DownloadQueueItem>& items);   // live-refresh while open

private:
	void rebuildTable(const QList<DownloadQueueItem>& items);

	QTableWidget* m_table = nullptr;
	QStringList   m_editionTokens;
};

} // namespace Mc
