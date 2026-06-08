#pragma once
#include <QList>
#include <QString>
#include <QWidget>

namespace Mc {

/**
 * McJobStatsBar — horizontal row of stat cells used by both McBulkSummaryDialog
 * and McPreviewDialog.
 *
 * Renders: [cell] [cell] … [stretch] [= label] [savings cell]
 */
class McJobStatsBar : public QWidget
{
	Q_OBJECT
public:
	struct StatItem {
		QString number;
		QString label;
	};

	explicit McJobStatsBar(const QList<StatItem>& items, qint64 estimatedBytes,
	                       QWidget* parent = nullptr);
};

} // namespace Mc
