#include "ui/McMultiCheckDropdown.h"
#include "ui/SvgIcon.h"

#include <QApplication>
#include <QListWidget>
#include <QScreen>
#include <QVBoxLayout>

namespace Mc {

McMultiCheckDropdown::McMultiCheckDropdown(const QString& label, QWidget* parent)
	: QToolButton(parent), m_label(label)
{
	setAutoRaise(true);
	setStyleSheet(QStringLiteral(
		"QToolButton { border: 1px solid palette(mid); border-radius: 4px;"
		"              padding: 2px 8px; background: palette(base); }"
		"QToolButton:hover { background: palette(alternate-base); }"));
	// QToolButton always draws its icon before the text; a dropdown indicator
	// reads as an indicator when it trails the label instead (matching the
	// combos elsewhere in this same filter bar). Flipping layout direction is
	// the standard, simple way to get that without a custom paintEvent — Qt
	// still shapes the text itself left-to-right, only the icon/text order mirrors.
	setLayoutDirection(Qt::RightToLeft);
	setIcon(svgIcon(QStringLiteral(":/icons/dropdown_arrow.svg")));
	setIconSize(QSize(14, 14));
	// QToolButton defaults to icon-only outside a QToolBar — without this, the
	// icon above replaces the label entirely instead of sitting beside it.
	setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	updateButtonText();

	m_popup = new QWidget(this, Qt::Popup);
	m_popup->setStyleSheet(QStringLiteral(
		"QWidget { border: 1px solid palette(mid); background: palette(base); }"));
	auto* layout = new QVBoxLayout(m_popup);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	m_list = new QListWidget(m_popup);
	m_list->setFrameShape(QFrame::NoFrame);
	m_list->setUniformItemSizes(true);
	layout->addWidget(m_list);
	connect(m_list, &QListWidget::itemChanged, this, &McMultiCheckDropdown::onItemChanged);

	connect(this, &QToolButton::clicked, this, &McMultiCheckDropdown::togglePopup);
}

void McMultiCheckDropdown::setItems(const QStringList& items)
{
	const QSet<QString> previouslyChecked = m_checked;
	m_checked.clear();

	// Rebuilding the list would re-fire itemChanged per row as checkstates
	// get (re)applied — block that noise and recompute m_checked once below.
	m_list->blockSignals(true);
	m_list->clear();
	for (const QString& item : items) {
		auto* w = new QListWidgetItem(item, m_list);
		w->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
		const bool checked = previouslyChecked.contains(item);
		w->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
		if (checked) m_checked.insert(item);
	}
	m_list->blockSignals(false);

	updateButtonText();
	// m_checked is always the subset of previouslyChecked that's still present
	// in the new item list (built above) — a shrunk size is the only way its
	// content could have changed, so that's all that needs checking here.
	if (m_checked.size() != previouslyChecked.size())
		emit selectionChanged(m_checked);
}

void McMultiCheckDropdown::togglePopup()
{
	if (m_popup->isVisible()) {
		m_popup->hide();
		return;
	}
	const int rowH   = m_list->sizeHintForRow(0) > 0 ? m_list->sizeHintForRow(0) : 22;
	const int rows   = qMin(m_list->count(), 12); // scroll rather than grow unbounded
	const int height = qMax(rowH, rowH * rows) + 4;
	m_popup->setFixedSize(qMax(width(), 160), height);
	m_popup->move(mapToGlobal(QPoint(0, this->height())));
	m_popup->show();
}

void McMultiCheckDropdown::onItemChanged(QListWidgetItem* item)
{
	if (item->checkState() == Qt::Checked)
		m_checked.insert(item->text());
	else
		m_checked.remove(item->text());
	updateButtonText();
	emit selectionChanged(m_checked);
}

void McMultiCheckDropdown::updateButtonText()
{
	setText(m_checked.isEmpty() ? m_label : QStringLiteral("%1 (%2)").arg(m_label).arg(m_checked.size()));
}

} // namespace Mc
