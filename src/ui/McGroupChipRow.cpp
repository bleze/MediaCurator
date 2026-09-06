#include "ui/McGroupChipRow.h"
#include "core/StorageGroupSettings.h"
#include "ui/McCardDelegate.h"

#include <algorithm>

#include <QMouseEvent>
#include <QPainter>

namespace Mc {

McGroupChipRow::McGroupChipRow(int maxGroup, int initialGroup, bool allowNone, QWidget* parent)
    : QWidget(parent)
    , m_maxGroup(std::max(maxGroup, initialGroup))
    , m_selected(initialGroup)
    , m_allowNone(allowNone)
{
	setMouseTracking(true);
	setToolTip(m_allowNone
	    ? tr("Assign folders on the same drive or NAS to the same storage group."
	         " Click the selected chip again to clear it.")
	    : tr("Assign folders on the same drive or NAS to the same storage group."
	         " Different groups can scan and remux in parallel."));
}

void McGroupChipRow::paintEvent(QPaintEvent*)
{
	QPainter p(this);
	const qreal dpr = devicePixelRatioF();
	for (int g = StorageGroupSettings::MinGroup; g <= m_maxGroup; ++g) {
		const QRect r       = chipRect(g);
		const bool  checked = (g == m_selected);
		const bool  hovered = r.contains(m_hoverPos);
		// Same chip McCardDelegate draws on cards — only opacity varies here
		// to convey on/off (darker when off, brighter on hover, full when
		// selected), the same darker→brighter convention as McFilterPanel's
		// quick-filter pills (4K/DV/Atmos…), without switching to a
		// different background/icon/text style.
		const double opacity = checked ? 1.0 : (hovered ? 0.55 : 0.25);
		McCardDelegate::drawGroupChip(&p, r.left(), r.top(), r.height(), g, font(), dpr, opacity);
	}
}

void McGroupChipRow::mousePressEvent(QMouseEvent* e)
{
	for (int g = StorageGroupSettings::MinGroup; g <= m_maxGroup; ++g) {
		if (!chipRect(g).contains(e->pos()))
			continue;
		if (g == m_selected) {
			if (!m_allowNone)
				return;
			m_selected = 0;
		} else {
			m_selected = g;
		}
		update();
		if (onGroupChanged) onGroupChanged(m_selected);
		return;
	}
}

void McGroupChipRow::mouseMoveEvent(QMouseEvent* e) { m_hoverPos = e->pos(); update(); }
void McGroupChipRow::leaveEvent(QEvent*) { m_hoverPos = { -1, -1 }; update(); }

QSize McGroupChipRow::sizeHint() const
{
	int totalW = 0;
	for (int g = StorageGroupSettings::MinGroup; g <= m_maxGroup; ++g) {
		if (g > StorageGroupSettings::MinGroup) totalW += kChipGap;
		totalW += McCardDelegate::groupChipWidth(g, font());
	}
	return { totalW, kChipH };
}

// y is centered on the widget's actual height, not kChipH — setCellWidget()
// stretches this widget to fill the full (taller) row height, so a fixed
// y=0 would leave the chip stuck to the top of the cell.
QRect McGroupChipRow::chipRect(int group) const
{
	int x = 0;
	for (int g = StorageGroupSettings::MinGroup; g < group; ++g)
		x += McCardDelegate::groupChipWidth(g, font()) + kChipGap;
	const int y = (height() - kChipH) / 2;
	return QRect(x, y, McCardDelegate::groupChipWidth(group, font()), kChipH);
}

} // namespace Mc
