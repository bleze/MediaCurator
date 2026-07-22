#include "ui/McFilterPanel.h"

#include "ui/RangeSlider.h"
#include "ui/McStorageGroupChipToggle.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"
#include "core/StorageGroupSettings.h"

#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <algorithm>

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

	m_searchTimer = new QTimer(this);
	m_searchTimer->setSingleShot(true);
	m_searchTimer->setInterval(250);
	connect(m_search, &QLineEdit::textChanged, m_searchTimer, QOverload<>::of(&QTimer::start));
	connect(m_searchTimer, &QTimer::timeout, this, [this]() {
		emit filterTextChanged(m_search->text());
	});

	// ── Status filter ─────────────────────────────────────────────────────────
	m_statusCombo = new QComboBox(this);
	m_statusCombo->setItemDelegate(new McFlatComboDelegate(m_statusCombo));
	m_statusCombo->addItem(tr("Show"),           QVariant());  // 0: header (row 0 in delegate)
	m_statusCombo->addItem(tr("All files"),      0);           // 1
	m_statusCombo->addItem(tr("With proposals"), 1);           // 2
	m_statusCombo->addItem(tr("Missing poster"), 2);           // 3
	m_statusCombo->addItem(tr("Ignored files"),  3);           // 4
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

	// ── Storage-group chips ───────────────────────────────────────────────────
	// Own container so refreshStorageGroups() can rebuild the chip row on its own
	// without disturbing the rest of the bar's layout.
	m_storageGroupContainer = new QWidget(this);
	m_storageGroupLayout = new QHBoxLayout(m_storageGroupContainer);
	m_storageGroupLayout->setContentsMargins(0, 0, 0, 0);
	m_storageGroupLayout->setSpacing(4);
	lay->addWidget(m_storageGroupContainer);
	refreshStorageGroups();

	// ── Quick-filter pills ────────────────────────────────────────────────────
	struct PillDef {
		const char* label;
		quint32     flag;
		QString     tip;
	};

	const QColor videoColor { 0xa0, 0x50, 0x00 };   // matches card video badge
	const QColor hdrColor   { 0x70, 0x30, 0xa0 };   // purple — premium HDR
	const QColor audioColor { 0x10, 0x6a, 0xc0 };   // matches card audio badge

	auto addGroup = [&](const QColor& color, auto& pills) {
		lay->addWidget(vSep(this));
		for (const auto& pd : pills) {
			auto* btn = makePill(QLatin1String(pd.label), color, this);
			btn->setToolTip(pd.tip);
			const quint32 f = pd.flag;
			connect(btn, &QToolButton::toggled, this, [this, f](bool on) { onPillToggled(f, on); });
			lay->addWidget(btn);
		}
	};

	// Media categories — own container so they can be hidden when the library has
	// no classified entries yet (all media_type = unknown). Multiple pills OR.
	// Checked colour == on-card badge (MediaTypes::badgeColor).
	{
		m_mediaCategoryContainer = new QWidget(this);
		auto* mediaLay = new QHBoxLayout(m_mediaCategoryContainer);
		mediaLay->setContentsMargins(0, 0, 0, 0);
		mediaLay->setSpacing(4);
		mediaLay->addWidget(vSep(m_mediaCategoryContainer));
		struct MediaPillDef {
			const char* label;
			quint32     flag;
			const char* type;   // MediaTypes::* for badgeColor
			QString     tip;
		};
		const MediaPillDef mediaGroup[] = {
			{ "Movies", QF_Movie,       MediaTypes::Movie,
			  QStringLiteral("Show movies only") },
			{ "TV",     QF_Tv,          MediaTypes::Tv,
			  QStringLiteral("Show TV series only") },
			{ "Docs",   QF_Documentary, MediaTypes::Documentary,
			  QStringLiteral("Show documentaries only") },
			{ "Misc",   QF_Misc,        MediaTypes::Misc,
			  QStringLiteral("Show misc / unmatched files only") },
		};
		for (const auto& pd : mediaGroup) {
			auto* btn = makePill(QLatin1String(pd.label),
			                     MediaTypes::badgeColor(QLatin1String(pd.type)),
			                     m_mediaCategoryContainer);
			btn->setToolTip(pd.tip);
			const quint32 f = pd.flag;
			connect(btn, &QToolButton::toggled, this, [this, f](bool on) { onPillToggled(f, on); });
			mediaLay->addWidget(btn);
		}
		m_mediaCategoryContainer->setVisible(false);  // until library has classified types
		lay->addWidget(m_mediaCategoryContainer);
	}

	const PillDef resGroup[] = {
		{ "4K", QF_4K, QStringLiteral("Show only 4K files (width ≥ 3840)") },
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

	// ── Rating slider ─────────────────────────────────────────────────────────
	lay->addWidget(vSep(this));
	m_ratingLabel = new QLabel(tr("Rating: All"), this);
	m_ratingLabel->setMinimumWidth(82);
	lay->addWidget(m_ratingLabel);
	auto* slider = new RangeSlider(Qt::Horizontal, RangeSlider::Option::DoubleHandles, this);
	m_ratingSlider = slider;
	slider->SetRange(0, 100);
	slider->SetLowerValue(0);
	slider->SetUpperValue(100);
	slider->setFixedWidth(110);
	m_ratingTimer = new QTimer(this);
	m_ratingTimer->setSingleShot(true);
	m_ratingTimer->setInterval(150);
	connect(slider, &RangeSlider::lowerValueChanged, this, [this]() {
		updateRatingLabel();
		m_ratingTimer->start();
	});
	connect(slider, &RangeSlider::upperValueChanged, this, [this]() {
		updateRatingLabel();
		m_ratingTimer->start();
	});
	connect(m_ratingTimer, &QTimer::timeout, this, &McFilterPanel::emitRatingFilter);
	lay->addWidget(slider);

	// ── Sort combo ────────────────────────────────────────────────────────────
	lay->addWidget(vSep(this));
	m_sortCombo = new QComboBox(this);
	m_sortCombo->setItemDelegate(new McFlatComboDelegate(m_sortCombo));
	m_sortCombo->addItem(tr("Sorting"),      QVariant());        // 0: header
	m_sortCombo->addItem(tr("Name"),         SortByName);       // 1
	m_sortCombo->addItem(tr("Newest first"), SortByNewest);     // 2
	m_sortCombo->addItem(tr("Oldest first"), SortByOldest);     // 3
	m_sortCombo->addItem(tr("Largest"),      SortByLargest);    // 4
	m_sortCombo->addItem(tr("Rating ↓"),     SortByRatingHigh); // 5
	m_sortCombo->addItem(tr("Rating ↑"),     SortByRatingLow);  // 6
	m_sortCombo->addItem(tr("Last scanned"), SortByLastScanned);// 7
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

void McFilterPanel::setMediaCategoryFiltersVisible(bool visible)
{
	if (!m_mediaCategoryContainer) return;
	if (m_mediaCategoryContainer->isVisible() == visible) return;

	m_mediaCategoryContainer->setVisible(visible);

	if (!visible) {
		// Drop any active category bits so a later hide doesn't leave a stuck filter
		// that matches nothing once types disappear (or were never present).
		const quint32 mediaMask = QF_Movie | QF_Tv | QF_Documentary | QF_Misc;
		if (m_activeFilters & mediaMask) {
			// Uncheck pills without re-emitting per-toggle; one emit at the end.
			const auto buttons = m_mediaCategoryContainer->findChildren<QToolButton*>();
			for (QToolButton* btn : buttons) {
				btn->blockSignals(true);
				btn->setChecked(false);
				btn->blockSignals(false);
			}
			m_activeFilters &= ~mediaMask;
			emit quickFiltersChanged(m_activeFilters);
		}
	}
}

void McFilterPanel::refreshStorageGroups()
{
	// The layout owns deletion here — takeAt() below deletes each chip (and the
	// separator) exactly once; this list is just a typed view for mask computation.
	m_storageGroupChips.clear();
	while (QLayoutItem* item = m_storageGroupLayout->takeAt(0)) {
		delete item->widget();
		delete item;
	}

	const QStringList roots = AppSettings::instance().value("scan/roots").toStringList();
	QList<int> groups = StorageGroupSettings::partitionRootsByGroup(roots).keys();
	if (groups.size() <= 1) {
		m_storageGroupContainer->setVisible(false);
		emitStorageGroupFilter();
		return;
	}
	std::sort(groups.begin(), groups.end());

	m_storageGroupLayout->addWidget(vSep(m_storageGroupContainer));
	for (int g : groups) {
		auto* chip = new McStorageGroupChipToggle(g, m_storageGroupContainer);
		connect(chip, &McStorageGroupChipToggle::toggled, this, [this](bool) { emitStorageGroupFilter(); });
		m_storageGroupLayout->addWidget(chip);
		m_storageGroupChips.append(chip);
	}
	m_storageGroupContainer->setVisible(true);
	emitStorageGroupFilter();
}

void McFilterPanel::emitStorageGroupFilter()
{
	quint32 mask = 0;
	for (McStorageGroupChipToggle* chip : m_storageGroupChips)
		if (chip->isChecked())
			mask |= (1u << chip->group());
	emit storageGroupFilterChanged(mask);
}

void McFilterPanel::updateRatingLabel()
{
	auto* slider = qobject_cast<RangeSlider*>(m_ratingSlider);
	if (!slider) return;
	const double lo = slider->GetLowerValue() / 10.0;
	const double hi = slider->GetUpperValue() / 10.0;
	if (lo <= 0.0 && hi >= 10.0)
		m_ratingLabel->setText(tr("Rating: All"));
	else if (lo <= 0.0)
		m_ratingLabel->setText(tr("Rating: ≤%1").arg(hi, 0, 'f', 1));
	else if (hi >= 10.0)
		m_ratingLabel->setText(tr("Rating: ≥%1").arg(lo, 0, 'f', 1));
	else
		m_ratingLabel->setText(tr("Rating: %1–%2").arg(lo, 0, 'f', 1).arg(hi, 0, 'f', 1));
}

void McFilterPanel::emitRatingFilter()
{
	auto* slider = qobject_cast<RangeSlider*>(m_ratingSlider);
	if (!slider) return;
	emit ratingFilterChanged(slider->GetLowerValue() / 10.0, slider->GetUpperValue() / 10.0);
}

} // namespace Mc
