#pragma once
#include <QList>
#include <QWidget>

#include "engine/DownloadClient.h"

class QLabel;
class QProgressBar;

namespace Mc {

// McDownloadQueueBand — collapsible strip (mirrors McHighscoreBand) showing
// aggregate progress across every active/queued download-client item.
// Purely a display widget — McMainWindow owns all data flow. Clicking
// anywhere on the band opens the full queue dialog.
class McDownloadQueueBand : public QWidget
{
	Q_OBJECT
public:
	explicit McDownloadQueueBand(QWidget* parent = nullptr);

	void setItems(const QList<DownloadQueueItem>& items);

signals:
	void clicked();

protected:
	void mousePressEvent(QMouseEvent* e) override;

private:
	QLabel*       m_label    = nullptr;
	QProgressBar* m_progress = nullptr;
};

} // namespace Mc
