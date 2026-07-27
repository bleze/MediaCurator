#pragma once
#include <QColor>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QToolButton>

class QListWidget;
class QListWidgetItem;

namespace Mc {

// Togglable pill-style QToolButton used for quick-filter chips in both McFilterPanel
// (library) and McJobPanel (job queue) filter bars — one shared definition so the two
// bars can't drift apart the way makePill()'s duplicated copies once did. Fixed height
// matches McMultiCheckDropdown's own (see McMultiCheckDropdown.cpp) so a row mixing
// pills and a dropdown lines up.
QToolButton* makeFilterPill(const QString& text, const QColor& color, QWidget* parent);

// A dropdown checklist filter — button shows "<label>" or "<label> (N)" once
// N items are checked, and clicking it opens a popup list of checkable items
// below the button. Used for open-ended, data-driven value sets (e.g. every
// distinct edition name found in the library) where a fixed set of toggle
// pills doesn't fit. Deliberately not a QComboBox: Qt's combobox popup closes
// on every item click unless you fight its private hidePopup()/view internals
// to keep it open across multiple selections, which is fragile across Qt
// versions. A plain Qt::Popup child widget gets the same "click outside to
// dismiss" behavior for free, directly from QWidget, with none of that risk.
class McMultiCheckDropdown final : public QToolButton
{
	Q_OBJECT
public:
	// fillColor, when valid, styles this button like the toolbar's other pill
	// buttons (McFilterPanel's makePill) — muted fill normally, full-color
	// fill once something is checked — instead of the plain neutral chrome.
	explicit McMultiCheckDropdown(const QString& label, QWidget* parent = nullptr,
	                              const QColor& fillColor = QColor());

	// Replaces the full set of selectable items. Any item that was checked
	// before AND is still present in the new list keeps its checked state
	// (e.g. after a rescan changes which editions actually exist) — a
	// checked item that's no longer present is dropped, and selectionChanged
	// fires if that changed the effective filter.
	void setItems(const QStringList& items);

	QSet<QString> checkedItems() const { return m_checked; }

	// Unchecks every item, restoring "no filter" (show all). No-op, no signal,
	// if nothing was checked.
	void clearSelection();

signals:
	void selectionChanged(const QSet<QString>& checked);

private:
	void togglePopup();
	void onItemChanged(QListWidgetItem* item);
	void updateButtonText();
	void updateFillStyle();

	QString      m_label;
	QColor       m_fillColor;
	QSet<QString> m_checked;
	QWidget*     m_popup = nullptr;
	QListWidget* m_list  = nullptr;
};

} // namespace Mc
