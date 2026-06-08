#include "ui/McFilterPanel.h"

#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPainter>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QToolButton>

namespace Mc {

namespace {

QFrame* vSep(QWidget* parent)
{
	auto* f = new QFrame(parent);
	f->setFrameShape(QFrame::VLine);
	f->setFrameShadow(QFrame::Sunken);
	return f;
}

QToolButton* makePill(const QString& text, const QColor& color, QWidget* parent)
{
	auto* btn = new QToolButton(parent);
	btn->setText(text);
	btn->setCheckable(true);
	btn->setAutoRaise(true);
	// Unchecked: muted semi-transparent fill (looks like a dimmed badge).
	// Checked: full-color fill — same appearance as the track badges on cards.
	const QString full  = color.name();
	const QString muted = QString("rgba(%1,%2,%3,80)")
	    .arg(color.red()).arg(color.green()).arg(color.blue());
	btn->setStyleSheet(QString(
		"QToolButton { border: none; border-radius: 4px;"
		"              padding: 2px 7px; background: %2;"
		"              color: white; font-weight: 600; }"
		"QToolButton:checked { background: %1; }"
	).arg(full, muted));
	return btn;
}

// Flat dropdown delegate — matches the styling of McStatusComboDelegate in
// the job panel so all comboboxes look consistent.
class McFlatComboDelegate final : public QStyledItemDelegate
{
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	void paint(QPainter* p, const QStyleOptionViewItem& opt,
	           const QModelIndex& idx) const override
	{
		const QString text = idx.data(Qt::DisplayRole).toString();
		// Row 0 is always the header label regardless of model enabled state.
		if (idx.row() == 0) {
			p->fillRect(opt.rect, opt.palette.base().color());
			p->setFont(opt.font);
			p->setPen(opt.palette.placeholderText().color());
			p->drawText(opt.rect.adjusted(8, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
			p->setPen(QPen(opt.palette.mid().color(), 1));
			p->drawLine(opt.rect.bottomLeft(), opt.rect.bottomRight());
			return;
		}
		// Normal item
		const bool active = (opt.state & QStyle::State_MouseOver)
		                  || (opt.state & QStyle::State_Selected);
		p->fillRect(opt.rect,
		    active ? opt.palette.highlight().color().lighter(175)
		           : opt.palette.base().color());
		p->setPen(opt.palette.text().color());
		p->setFont(opt.font);
		p->drawText(opt.rect.adjusted(8, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
	}

	QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
	{
		return { 160, 22 };
	}
};

} // namespace

McFilterPanel::McFilterPanel(QWidget* parent) : QWidget(parent)
{
	auto* lay = new QHBoxLayout(this);
	lay->setContentsMargins(4, 2, 4, 2);
	lay->setSpacing(4);

	// ── Search field ─────────────────────────────────────────────────────────
	m_search = new QLineEdit(this);
	m_search->setPlaceholderText(tr("Search title, folder, codec…"));
	m_search->setClearButtonEnabled(true);
	m_search->setMinimumWidth(180);
	lay->addWidget(m_search, 1);

	connect(m_search, &QLineEdit::textChanged, this, &McFilterPanel::filterTextChanged);

	// ── Status filter ─────────────────────────────────────────────────────────
	m_statusCombo = new QComboBox(this);
	m_statusCombo->setItemDelegate(new McFlatComboDelegate(m_statusCombo));
	m_statusCombo->addItem(tr("Show"),           QVariant());  // 0: header (row 0 in delegate)
	m_statusCombo->addItem(tr("All files"),      0);           // 1
	m_statusCombo->addItem(tr("With proposals"), 1);           // 2
	m_statusCombo->addItem(tr("Missing poster"), 2);           // 3
	if (auto* m = qobject_cast<QStandardItemModel*>(m_statusCombo->model()))
		if (auto* item = m->item(0))
			item->setEnabled(false);
	m_statusCombo->setCurrentIndex(1);
	m_statusCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	m_statusCombo->setMinimumWidth(m_statusCombo->sizeHint().width() + 15);
	m_statusCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
	lay->addWidget(m_statusCombo);
	connect(m_statusCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, [this](int i) {
		if (i <= 0) return;
		emit filterStatusChanged(m_statusCombo->currentData().toInt());
	});

	// ── Quick-filter pills ────────────────────────────────────────────────────
	struct PillDef {
		const char* label;
		quint32     flag;
		const char* tip;
	};

	const QColor videoColor { 0xa0, 0x50, 0x00 };   // matches card video badge
	const QColor hdrColor   { 0x70, 0x30, 0xa0 };   // purple — premium HDR
	const QColor audioColor { 0x10, 0x6a, 0xc0 };   // matches card audio badge

	auto addGroup = [&](const QColor& color, auto& pills) {
		lay->addWidget(vSep(this));
		for (const auto& pd : pills) {
			auto* btn = makePill(QLatin1String(pd.label), color, this);
			btn->setToolTip(QLatin1String(pd.tip));
			const quint32 f = pd.flag;
			connect(btn, &QToolButton::toggled, this, [this, f](bool on) { onPillToggled(f, on); });
			lay->addWidget(btn);
		}
	};

	const PillDef resGroup[] = {
		{ "4K", QF_4K, "Show only 4K files (width \xe2\x89\xa5 3840)" },
	};
	addGroup(videoColor, resGroup);

	const PillDef hdrGroup[] = {
		{ "DV",  QF_DV,  "Show only files with Dolby Vision" },
		{ "HDR", QF_HDR, "Show only files with HDR (HDR10 / HLG / HDR10+)" },
	};
	addGroup(hdrColor, hdrGroup);

	const PillDef audGroup[] = {
		{ "Atmos",  QF_Atmos,  "Show only files with Dolby Atmos" },
		{ "TrueHD", QF_TrueHD, "Show only files with Dolby TrueHD" },
		{ "DTS-HD", QF_DtsHD,  "Show only files with DTS-HD MA" },
		{ "DTS:X",  QF_DtsX,   "Show only files with DTS:X" },
	};
	addGroup(audioColor, audGroup);

	// ── Sort combo ────────────────────────────────────────────────────────────
	lay->addWidget(vSep(this));
	m_sortCombo = new QComboBox(this);
	m_sortCombo->setItemDelegate(new McFlatComboDelegate(m_sortCombo));
	m_sortCombo->addItem(tr("Sorting"),      QVariant());    // 0: header
	m_sortCombo->addItem(tr("Name"),         SortByName);   // 1
	m_sortCombo->addItem(tr("Newest first"), SortByNewest); // 2
	m_sortCombo->addItem(tr("Oldest first"), SortByOldest); // 3
	m_sortCombo->addItem(tr("Largest"),      SortByLargest); // 4
	if (auto* m = qobject_cast<QStandardItemModel*>(m_sortCombo->model()))
		if (auto* item = m->item(0))
			item->setEnabled(false);
	m_sortCombo->setCurrentIndex(1);
	lay->addWidget(m_sortCombo);
	connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, [this](int i) {
		if (i <= 0) return;
		emit sortOrderChanged(m_sortCombo->currentData().toInt());
	});
}

void McFilterPanel::onPillToggled(quint32 flag, bool on)
{
	if (on) m_activeFilters |= flag;
	else    m_activeFilters &= ~flag;
	emit quickFiltersChanged(m_activeFilters);
}

} // namespace Mc
