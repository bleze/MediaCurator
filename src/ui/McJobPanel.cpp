#include "ui/McJobPanel.h"
#include "core/AppSettings.h"
#include "core/StorageGroupSettings.h"
#include "ui/McBulkSummaryDialog.h"
#include "ui/McFilterPanel.h"
#include "ui/McGbGoalDialog.h"
#include "ui/McGoalProgressBar.h"
#include "ui/McJobCardDelegate.h"
#include "ui/McJobListModel.h"
#include "ui/McLanguageFlags.h"
#include "ui/McStorageGroupChipToggle.h"
#include "ui/McTrackContextMenu.h"
#include "ui/McWindowGeometry.h"
#include "ui/RangeSlider.h"
#include "ui/SvgIcon.h"
#include "engine/ActionEngine.h"
#include "engine/JobQueue.h"
#include "engine/PosterManager.h"
#include "engine/SubtitleManager.h"
#include "core/DatabaseManager.h"
#include "core/ExternalTools.h"

#include <algorithm>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QToolButton>
#include <QTimer>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QCursor>
#include <QKeyEvent>
#include <QItemSelectionModel>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QSet>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTextOption>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

// ── Status pill helpers ───────────────────────────────────────────────────────
namespace {

constexpr int kPillH       = 18;
constexpr int kPadH        = 6;
constexpr int kJobPageSize = 500;

// ETA is extrapolated from an exponentially-smoothed recent throughput rate
// rather than the whole job's elapsed-time average, so a slow patch (disk
// contention, a heavy segment) doesn't permanently drag the estimate down after
// speed recovers. Unlike a hard trailing window, the EMA has no sample-set
// membership edge to jump at, so the displayed value declines smoothly instead
// of oscillating tick to tick. kEtaSmoothingTauMs is the time constant: after
// this many ms of new data, the EMA has folded in ~63% of the change.
constexpr double kEtaSmoothingTauMs = 30000.0;

static QColor pillColorForStatus(const QString& status)
{
	if (status == QLatin1String("proposed"))     return { 0x88, 0x88, 0x88 };
	if (status == QLatin1String("queued"))       return { 0xc0, 0x80, 0x00 };
	if (status == QLatin1String("running"))      return { 0x10, 0x6a, 0xc0 };
	if (status == QLatin1String("done"))         return { 0x1a, 0x86, 0x4a };
	if (status == QLatin1String("failed"))       return { 0xcc, 0x22, 0x22 };
	if (status == QLatin1String("needs_review")) return { 0xb8, 0x60, 0x00 };
	return { 0x50, 0x80, 0xa8 };
}

static void drawStatusPill(QPainter* p, const QRect& containerRect, int leftPad,
                           const QString& status, const QString& text, const QFont& baseFont)
{
	if (text.isEmpty()) return;
	QFont font = baseFont;
	font.setPointSizeF(font.pointSizeF() * 0.82);
	p->setFont(font);
	const QFontMetrics fm(font);
	const int pillW = fm.horizontalAdvance(text) + 2 * kPadH;
	const int padV  = (containerRect.height() - kPillH) / 2;
	const QRect pill(containerRect.left() + leftPad, containerRect.top() + padV, pillW, kPillH);
	p->setRenderHint(QPainter::Antialiasing);
	p->setPen(Qt::NoPen);
	p->setBrush(pillColorForStatus(status));
	p->drawRoundedRect(pill, 4, 4);
	p->setPen(Qt::white);
	p->drawText(pill, Qt::AlignCenter, text);
}

// Renders status-filter combobox dropdown items as coloured pill badges.
class McStatusComboDelegate final : public QStyledItemDelegate
{
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	void paint(QPainter* p, const QStyleOptionViewItem& opt,
	           const QModelIndex& idx) const override
	{
		// Row 0 is always the header label regardless of model enabled state.
		if (idx.row() == 0) {
			p->fillRect(opt.rect, opt.palette.base().color());
			p->setFont(opt.font);
			p->setPen(opt.palette.placeholderText().color());
			p->drawText(opt.rect.adjusted(8, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter,
			            idx.data(Qt::DisplayRole).toString());
			p->setPen(QPen(opt.palette.mid().color(), 1));
			p->drawLine(opt.rect.bottomLeft(), opt.rect.bottomRight());
			return;
		}
		const QString status = idx.data(Qt::UserRole).toString();
		const QString text   = idx.data(Qt::DisplayRole).toString();

		const bool active = (opt.state & QStyle::State_MouseOver)
		                  || (opt.state & QStyle::State_Selected);
		p->fillRect(opt.rect,
		    active ? opt.palette.highlight().color().lighter(175)
		           : opt.palette.base().color());

		drawStatusPill(p, opt.rect, 8, status, text, opt.font);
	}

	QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
	{
		return { 160, 22 };
	}
};

// QComboBox subclass that also paints a pill in the closed (button) state.
class McStatusComboBox final : public QComboBox
{
public:
	using QComboBox::QComboBox;

protected:
	void paintEvent(QPaintEvent*) override
	{
		QStylePainter sp(this);
		QStyleOptionComboBox opt;
		initStyleOption(&opt);

		// Draw the frame + arrow without any text
		opt.currentText = {};
		sp.drawComplexControl(QStyle::CC_ComboBox, opt);

		// Draw the pill inside the edit-field area
		const QRect cr = style()->subControlRect(
		    QStyle::CC_ComboBox, &opt, QStyle::SC_ComboBoxEditField, this);

		const QModelIndex cur = model()->index(currentIndex(), 0);
		if (!(model()->flags(cur) & Qt::ItemIsEnabled)) return;
		const QString status  = currentData().toString();
		const QString display = QComboBox::currentText();
		if (display.isEmpty()) return;

		QFont font = this->font();
		font.setPointSizeF(font.pointSizeF() * 0.82);
		const QFontMetrics fm(font);
		const int pillW   = fm.horizontalAdvance(display) + 2 * kPadH;
		// Match the open dropdown: items draw at opt.rect.left()+8 where the popup
		// list has a 1px frame, so effective offset = popup_left+1+8. Compensate by
		// using kPadH-1 (5) instead of kPadH (6) to land on the same pixel.
		const int leftPad = kPadH - 1;

		sp.setRenderHint(QPainter::Antialiasing);
		sp.setPen(Qt::NoPen);
		sp.setBrush(pillColorForStatus(status));
		const int padV = (cr.height() - kPillH) / 2;
		sp.drawRoundedRect(
		    QRect(cr.left() + leftPad, cr.top() + padV, pillW, kPillH), 4, 4);
		sp.setPen(Qt::white);
		sp.setFont(font);
		sp.drawText(
		    QRect(cr.left() + leftPad, cr.top() + padV, pillW, kPillH),
		    Qt::AlignCenter, display);
	}
};

static QString formatEtaDuration(double secs)
{
	const int totalSec = qMax(1, static_cast<int>(secs));
	const int h = totalSec / 3600;
	const int m = (totalSec % 3600) / 60;
	const int s = totalSec % 60;
	if (h > 0) return QStringLiteral("%1h %2m").arg(h).arg(m);
	if (m > 0) return QStringLiteral("%1m %2s").arg(m).arg(s);
	return QStringLiteral("%1s").arg(s);
}

static QFrame* vSep(QWidget* parent)
{
	auto* f = new QFrame(parent);
	f->setFrameShape(QFrame::VLine);
	f->setFrameShadow(QFrame::Sunken);
	return f;
}

static QToolButton* makePill(const QString& text, const QColor& color, QWidget* parent)
{
	auto* btn = new QToolButton(parent);
	btn->setText(text);
	btn->setCheckable(true);
	btn->setAutoRaise(true);
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

} // anonymous namespace

namespace Mc {

McJobPanel::McJobPanel(QWidget* parent)
	: QWidget(parent)
{
	setupUi();
}

void McJobPanel::setupUi()
{
	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 0);
	root->setSpacing(0);

	// ── Toolbar ───────────────────────────────────────────────────────────────
	auto* toolbar = new QToolBar(this);
	toolbar->setIconSize({ 16, 16 });
	toolbar->setStyleSheet(
	    "QPushButton {"
	    "  padding: 3px 8px;"
	    "  border-radius: 4px;"
	    "  border: 1px solid transparent;"
	    "}"
	    "QPushButton:hover:enabled {"
	    "  background: rgba(128,128,128,50);"
	    "  border-color: rgba(128,128,128,80);"
	    "}"
	    "QPushButton:pressed:enabled {"
	    "  background: rgba(128,128,128,90);"
	    "}");

	m_btnQueueSelected = new QPushButton(tr("Queue Selected"), toolbar);
	m_btnUnqueue       = new QPushButton(tr("Unqueue"),        toolbar);
	m_btnQueueAll      = new QPushButton(tr("Queue All"),      toolbar);
	m_btnRemove        = new QPushButton(tr("Remove"),         toolbar);
	m_btnSummary       = new QPushButton(tr("Summary"),        toolbar);
	m_btnStartPause    = new QPushButton(tr("Start"),          toolbar);
	m_btnCancel        = new QPushButton(tr("Cancel"),         toolbar);

	const QSize kIconSz(24, 24);
	m_btnQueueSelected->setIcon(svgIcon(":/icons/playlist_add_check.svg"));
	m_btnQueueSelected->setIconSize(kIconSz);
	m_btnUnqueue->setIcon(svgIcon(":/icons/undo.svg"));
	m_btnUnqueue->setIconSize(kIconSz);
	m_btnQueueAll->setIcon(svgIcon(":/icons/add_to_queue.svg"));
	m_btnQueueAll->setIconSize(kIconSz);
	m_btnRemove->setIcon(svgIcon(":/icons/delete.svg"));
	m_btnRemove->setIconSize(kIconSz);
	m_btnSummary->setIcon(svgIcon(":/icons/manage_search.svg"));
	m_btnSummary->setIconSize(kIconSz);
	m_btnStartPause->setIcon(svgIcon(":/icons/play_arrow.svg"));
	m_btnStartPause->setIconSize(kIconSz);
	m_btnCancel->setIcon(svgIcon(":/icons/stop_circle.svg"));
	m_btnCancel->setIconSize(kIconSz);

	// A single-arrow dropdown: deliberately NOT using setMenu()/MenuButtonPopup —
	// with a menu attached, Qt's style draws its own small "has menu" indicator on
	// top of setArrowType()'s glyph, producing a redundant double arrow. Popping
	// the menu manually on click keeps just the one arrow.
	m_btnStartGoalMenu = new QToolButton(toolbar);
	m_btnStartGoalMenu->setArrowType(Qt::DownArrow);
	m_btnStartGoalMenu->setToolTip(tr("Start processing toward a free-space goal"));
	auto* startGoalMenu = new QMenu(m_btnStartGoalMenu);
	QAction* startGoalAction = startGoalMenu->addAction(tr("Start with GB goal\xE2\x80\xA6"));
	connect(startGoalAction, &QAction::triggered, this, &McJobPanel::onStartWithGoal);
	connect(m_btnStartGoalMenu, &QToolButton::clicked, this, [this, startGoalMenu] {
		startGoalMenu->exec(m_btnStartGoalMenu->mapToGlobal(
		    QPoint(0, m_btnStartGoalMenu->height())));
	});

	m_btnQueueSelected->setEnabled(false);
	m_btnUnqueue->setEnabled(false);
	m_btnQueueAll->setEnabled(false);
	m_btnStartPause->setEnabled(false);
	m_btnStartGoalMenu->setEnabled(false);
	m_btnCancel->setEnabled(false);
	m_btnRemove->setEnabled(false);
	m_btnSummary->setEnabled(false);

	m_btnQueueSelected->setToolTip(tr("Move selected proposed jobs to the processing queue"));
	m_btnUnqueue->setToolTip(tr("Move selected queued jobs back to proposed state"));
	m_btnQueueAll->setToolTip(tr("Move all proposed jobs to the processing queue"));
	m_btnStartPause->setToolTip(tr(
		"Start processing — runs queued jobs in order, up to one active job per storage group"));
	m_btnCancel->setToolTip(tr("Stop the current job — queued jobs remain and can be restarted"));
	m_btnRemove->setToolTip(tr("Remove selected jobs from the queue"));
	m_btnSummary->setToolTip(tr("Show aggregate statistics for all proposed jobs"));

	toolbar->addWidget(m_btnQueueSelected);
	toolbar->addWidget(m_btnQueueAll);
	toolbar->addSeparator();
	toolbar->addWidget(m_btnUnqueue);
	toolbar->addSeparator();
	toolbar->addWidget(m_btnRemove);
	toolbar->addSeparator();
	toolbar->addWidget(m_btnSummary);

	m_footerLabel = new QLabel(tr("No jobs"), toolbar);
	m_footerLabel->setStyleSheet("color: gray; padding: 0 6px;");
	toolbar->addWidget(m_footerLabel);

	// Spacer absorbs label width changes, keeping Start/Cancel pinned to the right
	auto* tbSpacer = new QWidget(toolbar);
	tbSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	toolbar->addWidget(tbSpacer);

	toolbar->addWidget(m_btnStartPause);
	toolbar->addWidget(m_btnStartGoalMenu);
	toolbar->addWidget(m_btnCancel);

	root->addWidget(toolbar);

	// ── Filter bar ────────────────────────────────────────────────────────────
	m_filterEdit = new QLineEdit(this);
	m_filterEdit->setPlaceholderText(tr("Search title, folder, codec…"));
	m_filterEdit->setClearButtonEnabled(true);

	m_statusFilter = new McStatusComboBox(this);
	// Row 0 is always the header; add it first so updateStatusCombo() offsets remain stable.
	m_statusFilter->addItem(tr("Status"),   QVariant());               // 0: header
	m_statusFilter->addItem(tr("All"),      QStringLiteral(""));       // 1
	m_statusFilter->addItem(tr("Proposed"), QStringLiteral("proposed")); // 2
	m_statusFilter->addItem(tr("Queued"),   QStringLiteral("queued"));   // 3
	m_statusFilter->addItem(tr("Running"),  QStringLiteral("running"));  // 4
	m_statusFilter->addItem(tr("Done"),     QStringLiteral("done"));     // 5
	m_statusFilter->addItem(tr("Failed"),   QStringLiteral("failed"));   // 6
	if (auto* m = qobject_cast<QStandardItemModel*>(m_statusFilter->model()))
		if (auto* item = m->item(0))
			item->setEnabled(false);
	m_statusFilter->setCurrentIndex(1);   // "All"
	m_statusFilter->setItemDelegate(new McStatusComboDelegate(m_statusFilter));
	m_statusFilter->setSizeAdjustPolicy(QComboBox::AdjustToContents);
	m_statusFilter->setMinimumWidth(m_statusFilter->sizeHint().width() + 31);
	m_statusFilter->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);

	m_sortCombo = new QComboBox(this);
	m_sortCombo->addItem(tr("Smallest file first"),        static_cast<int>(JobSortMode::SmallestFirst));
	m_sortCombo->addItem(tr("Largest savings first"), static_cast<int>(JobSortMode::LargestSavingsFirst));
	m_sortCombo->addItem(tr("Most recently completed"), static_cast<int>(JobSortMode::MostRecentFirst));
	m_sortCombo->setCurrentIndex(AppSettings::instance().value("jobPanel/sortMode", 0).toInt());
	m_sortCombo->setToolTip(tr("Sort order for the job list. \"Most recently completed\" is auto-selected when viewing Done jobs."));

	// ── Quick-filter pills (same set as the library panel, including media type) ─
	using QF = McFilterPanel;
	const QColor videoColor { 0xa0, 0x50, 0x00 };
	const QColor hdrColor   { 0x70, 0x30, 0xa0 };
	const QColor audioColor { 0x10, 0x6a, 0xc0 };

	auto* filterBar    = new QWidget(this);
	auto* filterLayout = new QHBoxLayout(filterBar);
	filterLayout->setContentsMargins(4, 2, 4, 2);
	filterLayout->setSpacing(4);
	filterLayout->addWidget(m_filterEdit, 1);
	filterLayout->addWidget(m_statusFilter);

	// ── Storage-group chips ────────────────────────────────────────────────────
	// Own container so refreshStorageGroups() can rebuild the chip row on its own
	// without disturbing the rest of the bar's layout.
	m_storageGroupContainer = new QWidget(filterBar);
	m_storageGroupLayout = new QHBoxLayout(m_storageGroupContainer);
	m_storageGroupLayout->setContentsMargins(0, 0, 0, 0);
	m_storageGroupLayout->setSpacing(4);
	filterLayout->addWidget(m_storageGroupContainer);
	refreshStorageGroups();

	const auto addPillTo = [&](QWidget* parent, QHBoxLayout* layout,
	                           const char* label, const QColor& color, const char* tip, quint32 flag) {
		auto* btn = makePill(QLatin1String(label), color, parent);
		btn->setToolTip(QLatin1String(tip));
		connect(btn, &QToolButton::toggled, this, [this, flag](bool on) {
			m_qfFlags = on ? (m_qfFlags | flag) : (m_qfFlags & ~flag);
			m_model->setQuickFilters(m_qfFlags);
		});
		layout->addWidget(btn);
	};
	const auto addPill = [&](const char* label, const QColor& color, const char* tip, quint32 flag) {
		addPillTo(filterBar, filterLayout, label, color, tip, flag);
	};

	// Media categories — hidden until at least one job file has a real type.
	// Checked colour == on-card badge (MediaTypes::badgeColor).
	{
		m_mediaCategoryContainer = new QWidget(filterBar);
		auto* mediaLay = new QHBoxLayout(m_mediaCategoryContainer);
		mediaLay->setContentsMargins(0, 0, 0, 0);
		mediaLay->setSpacing(4);
		mediaLay->addWidget(vSep(m_mediaCategoryContainer));
		addPillTo(m_mediaCategoryContainer, mediaLay, "Movies",
		          MediaTypes::badgeColor(QLatin1String(MediaTypes::Movie)),
		          "Movies only", QF::QF_Movie);
		addPillTo(m_mediaCategoryContainer, mediaLay, "TV",
		          MediaTypes::badgeColor(QLatin1String(MediaTypes::Tv)),
		          "TV series only", QF::QF_Tv);
		addPillTo(m_mediaCategoryContainer, mediaLay, "Docs",
		          MediaTypes::badgeColor(QLatin1String(MediaTypes::Documentary)),
		          "Documentaries only", QF::QF_Documentary);
		addPillTo(m_mediaCategoryContainer, mediaLay, "Misc",
		          MediaTypes::badgeColor(QLatin1String(MediaTypes::Misc)),
		          "Misc / unmatched only", QF::QF_Misc);
		m_mediaCategoryContainer->setVisible(false);
		filterLayout->addWidget(m_mediaCategoryContainer);
	}

	filterLayout->addWidget(vSep(filterBar));
	addPill("4K",     videoColor, "4K files only (width >= 3840)", QF::QF_4K);
	filterLayout->addWidget(vSep(filterBar));
	addPill("DV",     hdrColor,   "Dolby Vision only",                       QF::QF_DV);
	addPill("HDR",    hdrColor,   "HDR10 / HLG / HDR10+ only",              QF::QF_HDR);
	filterLayout->addWidget(vSep(filterBar));
	addPill("Atmos",  audioColor, "Dolby Atmos only",                        QF::QF_Atmos);
	addPill("TrueHD", audioColor, "Dolby TrueHD only",                      QF::QF_TrueHD);
	addPill("DTS-HD", audioColor, "DTS-HD MA only",                         QF::QF_DtsHD);
	addPill("DTS:X",  audioColor, "DTS:X only",                              QF::QF_DtsX);

	filterLayout->addWidget(vSep(filterBar));
	m_ratingLabel = new QLabel(tr("Rating: All"), filterBar);
	m_ratingLabel->setMinimumWidth(82);
	filterLayout->addWidget(m_ratingLabel);
	auto* rSlider = new RangeSlider(Qt::Horizontal, RangeSlider::Option::DoubleHandles, filterBar);
	m_ratingSlider = rSlider;
	rSlider->SetRange(0, 100);
	rSlider->SetLowerValue(0);
	rSlider->SetUpperValue(100);
	rSlider->setFixedWidth(110);
	connect(rSlider, &RangeSlider::lowerValueChanged, this, [this]() {
		auto* s = qobject_cast<RangeSlider*>(m_ratingSlider);
		if (!s) return;
		const double lo = s->GetLowerValue() / 10.0;
		const double hi = s->GetUpperValue() / 10.0;
		if (lo <= 0.0 && hi >= 10.0)  m_ratingLabel->setText(tr("Rating: All"));
		else if (lo <= 0.0)           m_ratingLabel->setText(tr("Rating: \xe2\x89\xa4%1").arg(hi, 0, 'f', 1));
		else if (hi >= 10.0)          m_ratingLabel->setText(tr("Rating: \xe2\x89\xa5%1").arg(lo, 0, 'f', 1));
		else                          m_ratingLabel->setText(tr("Rating: %1\xe2\x80\x93%2").arg(lo, 0, 'f', 1).arg(hi, 0, 'f', 1));
		m_model->setRatingFilter(lo, hi);
	});
	connect(rSlider, &RangeSlider::upperValueChanged, this, [this]() {
		auto* s = qobject_cast<RangeSlider*>(m_ratingSlider);
		if (!s) return;
		const double lo = s->GetLowerValue() / 10.0;
		const double hi = s->GetUpperValue() / 10.0;
		if (lo <= 0.0 && hi >= 10.0)  m_ratingLabel->setText(tr("Rating: All"));
		else if (lo <= 0.0)           m_ratingLabel->setText(tr("Rating: \xe2\x89\xa4%1").arg(hi, 0, 'f', 1));
		else if (hi >= 10.0)          m_ratingLabel->setText(tr("Rating: \xe2\x89\xa5%1").arg(lo, 0, 'f', 1));
		else                          m_ratingLabel->setText(tr("Rating: %1\xe2\x80\x93%2").arg(lo, 0, 'f', 1).arg(hi, 0, 'f', 1));
		m_model->setRatingFilter(lo, hi);
	});
	filterLayout->addWidget(rSlider);

	filterLayout->addWidget(vSep(filterBar));
	filterLayout->addWidget(m_sortCombo);
	root->addWidget(filterBar);

	m_goalBar = new McGoalProgressBar(this);
	m_goalBar->setVisible(false);
	root->addWidget(m_goalBar);
	connect(m_goalBar, &McGoalProgressBar::clicked, this, &McJobPanel::onEditGoal);

	connect(m_btnQueueSelected, &QPushButton::clicked, this, &McJobPanel::onQueueSelected);
	connect(m_btnUnqueue,       &QPushButton::clicked, this, &McJobPanel::onUnqueueSelected);
	connect(m_btnQueueAll,      &QPushButton::clicked, this, &McJobPanel::onQueueAll);
	connect(m_btnRemove,      &QPushButton::clicked, this, &McJobPanel::onRemoveSelected);
	connect(m_btnSummary,     &QPushButton::clicked, this, &McJobPanel::onShowSummary);
	connect(m_btnStartPause,  &QPushButton::clicked, this, &McJobPanel::onStartPause);
	connect(m_btnCancel,      &QPushButton::clicked, this, &McJobPanel::onCancel);

	// ── List view ─────────────────────────────────────────────────────────────
	m_model = new McJobListModel(this);

	// Clamp any stale "MostRecentFirst" (value 2) written by an earlier build.
	// Then seed m_lastQueueSortIdx so the restore path never reads AppSettings mid-session.
	{
		int v = AppSettings::instance().value("jobPanel/sortMode", 0).toInt();
		if (v >= static_cast<int>(JobSortMode::MostRecentFirst)) {
			v = 0;
			AppSettings::instance().setValue("jobPanel/sortMode", 0);
		}
		m_lastQueueSortIdx = v;
	}
	m_model->setSortMode(static_cast<JobSortMode>(m_lastQueueSortIdx));

	// Adjusts the sort combo when the status filter changes:
	// - enables "Most recently completed" only for Done/Failed
	// - auto-switches to MostRecentFirst when entering Done/Failed
	// - restores the saved queue sort when leaving Done/Failed with MostRecentFirst active
	// MostRecentFirst is never written to "jobPanel/sortMode" so the queue sort is
	// preserved across Done/Failed visits.
	const auto applySortForFilter = [this](const QString& filterStatus) {
		const bool hasCompletionTime = filterStatus == QLatin1String("done")
		                            || filterStatus == QLatin1String("failed");
		const int recentIdx = m_sortCombo->findData(static_cast<int>(JobSortMode::MostRecentFirst));
		if (recentIdx < 0) return;

		// Enable / disable the "Most recently completed" item.
		if (auto* m = qobject_cast<QStandardItemModel*>(m_sortCombo->model()))
			if (auto* item = m->item(recentIdx))
				item->setEnabled(hasCompletionTime);

		const auto curMode = static_cast<JobSortMode>(
			m_sortCombo->itemData(m_sortCombo->currentIndex()).toInt());

		if (hasCompletionTime) {
			// Auto-switch to MostRecentFirst when entering Done/Failed.
			if (curMode != JobSortMode::MostRecentFirst)
				m_sortCombo->setCurrentIndex(recentIdx);  // triggers signal → updates model
		} else if (curMode == JobSortMode::MostRecentFirst) {
			// Leaving Done/Failed with MostRecentFirst active → restore last queue sort.
			// Uses in-session memory (m_lastQueueSortIdx) — never stale.
			m_sortCombo->setCurrentIndex(m_lastQueueSortIdx);  // triggers signal → updates model
		}
		// If curMode is already a queue sort on a queue filter, nothing to do —
		// the model is already correct.
	};

	connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
		const auto mode = static_cast<JobSortMode>(m_sortCombo->itemData(idx).toInt());
		if (mode != JobSortMode::MostRecentFirst) {
			AppSettings::instance().setValue(QStringLiteral("jobPanel/sortMode"), idx);
			m_lastQueueSortIdx = idx;  // remember for restore when leaving Done/Failed
		}
		// setSortMode() already re-sorts m_allEntries in place and re-applies the
		// current filter — a full reload() here would be a redundant DB round-trip
		// (this is also what made the Done/Failed tabs' auto sort-switch feel like
		// it randomly stalled: it silently forced a full unpaged reload on every
		// entry/exit of those tabs).
		m_model->setSortMode(mode);
		m_listView->scrollToTop();
		if (m_queue) m_queue->setSortMode(mode);
	});

	m_listView = new QListView(this);
	m_listView->setModel(m_model);
	auto* jobDelegate = new McJobCardDelegate(m_listView);
	m_listView->setItemDelegate(jobDelegate);
	m_listView->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_listView->setMouseTracking(true);
	m_listView->viewport()->setMouseTracking(true);
	m_listView->setUniformItemSizes(false);
	m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	// AlwaysOn, not the AsNeeded default: row heights depend on viewport width
	// (badge wrapping), so if the scrollbar's own presence toggles with total
	// content height, its width changes the viewport width, which triggers
	// another resize → another relayout → possibly toggles the scrollbar again.
	// Pinning it removes that feedback loop entirely (see McCardDelegate's
	// resize-relayout handling).
	m_listView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
	m_listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	m_listView->setAlternatingRowColors(true);
	m_listView->setSpacing(0);
	// Fixed, not Adjust: with setUniformItemSizes(false), Adjust mode forces a full
	// doItemsLayout() (re-querying every row's sizeHint) on every single resize
	// tick during a live drag — badly janky with thousands of cards. McCardDelegate
	// now handles post-resize relayout itself (debounced, visible-rows-only; see
	// McCardDelegate::relayoutForResize), so Qt's own per-tick relayout is redundant.
	m_listView->setResizeMode(QListView::Fixed);
	m_listView->setContextMenuPolicy(Qt::CustomContextMenu);
	m_listView->installEventFilter(this);

	// QListView's viewport defaults to a light Base on Windows until its first
	// delegate paint — same issue McMainWindow::applyDarkBackgrounds() fixes for the
	// library view. The job panel can become visible (updateJobPanelVisibility())
	// before that first paint happens, so without this it flashes white against a
	// dark theme. Filled here directly rather than from McMainWindow so the panel
	// is correct on its own regardless of when/how it's shown.
	{
		const QColor win  = palette().color(QPalette::Window);
		const QColor base = palette().color(QPalette::Base);
		const QColor alt  = palette().color(QPalette::AlternateBase);
		auto fillDark = [](QWidget* w, const QColor& window, const QColor& baseColor,
		                    const QColor& altColor) {
			if (!w) return;
			w->setAutoFillBackground(true);
			QPalette pal = w->palette();
			pal.setColor(QPalette::Window, window);
			pal.setColor(QPalette::Base, baseColor);
			pal.setColor(QPalette::AlternateBase, altColor);
			w->setPalette(pal);
		};
		fillDark(m_listView, win, base, alt);
		fillDark(m_listView->viewport(), base, base, alt);
	}

	connect(jobDelegate, &McJobCardDelegate::playRequested,
	        this, [this](const QModelIndex& idx) {
		const QString path = idx.data(McJobListModel::FilePathRole).toString();
		if (!path.isEmpty()) emit playRequested(path);
	});

	connect(jobDelegate, &McJobCardDelegate::streamToggleRequested,
	        this, [this](const QModelIndex& idx, int streamIndex) {
		QApplication::setOverrideCursor(Qt::WaitCursor);
		m_model->toggleStream(idx, streamIndex);
		QApplication::restoreOverrideCursor();
	});

	connect(jobDelegate, &McJobCardDelegate::imdbPageRequested,
	        this, [](const QModelIndex& idx) {
		const QString id = idx.data(McJobListModel::ImdbIdRole).toString();
		if (!id.isEmpty())
			QDesktopServices::openUrl(
			    QUrl(QStringLiteral("https://www.imdb.com/title/%1/").arg(id)));
	});
	connect(jobDelegate, &McJobCardDelegate::tmdbPageRequested,
	        this, [](const QModelIndex& idx) {
		const int id = idx.data(McJobListModel::TmdbIdRole).toInt();
		if (id <= 0) return;
		const QString kind = idx.data(McJobListModel::MediaTypeRole).toString()
		                   == QLatin1String(MediaTypes::Tv) ? QStringLiteral("tv") : QStringLiteral("movie");
		QDesktopServices::openUrl(
		    QUrl(QStringLiteral("https://www.themoviedb.org/%1/%2").arg(kind).arg(id)));
	});

	// Stale size cache: the cache is keyed by row number. After a model reset the
	// row-to-data mapping changes, so old entries would return wrong heights.
	connect(m_model, &QAbstractListModel::modelReset,
	        jobDelegate, &McCardDelegate::clearSizeCache);

	connect(m_model, &QAbstractListModel::modelReset, this, [this]() {
		// selectionModel()->reset() is called on model reset but does NOT emit
		// selectionChanged, so we must disable selection-dependent buttons manually.
		m_btnQueueSelected->setEnabled(false);
		m_btnUnqueue->setEnabled(false);
		m_btnRemove->setEnabled(false);
		updateStatusCombo();
	});

	connect(m_listView->selectionModel(), &QItemSelectionModel::selectionChanged,
	        this, [this]() {
		const auto selected = m_listView->selectionModel()->selectedIndexes();
		const bool hasRemovable = std::any_of(selected.cbegin(), selected.cend(),
		    [](const QModelIndex& idx) {
		        return idx.data(McJobListModel::StatusRole).toString() != QLatin1String("running");
		    });
		m_btnRemove->setEnabled(hasRemovable);
		const bool hasProposed = std::any_of(selected.cbegin(), selected.cend(),
		    [](const QModelIndex& idx) {
		        return idx.data(McJobListModel::StatusRole).toString() == "proposed";
		    });
		m_btnQueueSelected->setEnabled(hasProposed);
		const bool hasQueued = std::any_of(selected.cbegin(), selected.cend(),
		    [](const QModelIndex& idx) {
		        return idx.data(McJobListModel::StatusRole).toString() == "queued";
		    });
		m_btnUnqueue->setEnabled(hasQueued);
	});

	connect(m_filterEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
		qint64 anchorId = -1;
		if (text.isEmpty()) {
			const auto sel = m_listView->selectionModel()->selectedIndexes();
			if (!sel.isEmpty())
				anchorId = sel.first().data(McJobListModel::JobIdRole).toLongLong();
		}
		m_model->setFilterText(text);
		if (text.isEmpty() && anchorId >= 0) {
			for (int row = 0; row < m_model->rowCount(); ++row) {
				const QModelIndex idx = m_model->index(row);
				if (idx.data(McJobListModel::JobIdRole).toLongLong() == anchorId) {
					m_listView->setCurrentIndex(idx);
					m_listView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
					break;
				}
			}
		}
	});
	connect(m_statusFilter, &QComboBox::currentIndexChanged,
	        this, [this, applySortForFilter](int i) {
		if (i <= 0) return;   // skip header row
		const QString v = m_statusFilter->itemData(i).toString();
		AppSettings::instance().setValue("jobPanel/statusFilter", v);
		m_model->setFilterStatus(v);
		applySortForFilter(v);
		if ((v == QLatin1String("running") || v == QLatin1String("queued")) && m_focusJobId > 0)
			QTimer::singleShot(0, this, [this] { focusJob(m_focusJobId); });
	});

	// Restore the previously saved status filter (block signals so the save slot
	// does not fire again, then apply the filter to the model manually).
	{
		const QString saved = AppSettings::instance().value("jobPanel/statusFilter").toString();
		if (!saved.isEmpty()) {
			m_statusFilter->blockSignals(true);
			for (int i = 0; i < m_statusFilter->count(); ++i) {
				if (m_statusFilter->itemData(i).toString() == saved) {
					m_statusFilter->setCurrentIndex(i);
					break;
				}
			}
			m_statusFilter->blockSignals(false);
			m_model->setFilterStatus(m_statusFilter->currentData().toString());
		}
		// Signals were blocked — enable/disable MostRecentFirst and apply auto-switch now.
		applySortForFilter(m_statusFilter->currentData().toString());
	}

	// Double-click: poster column → IMDb dialog; elsewhere → preview
	connect(m_listView, &QListView::doubleClicked, this, [this](const QModelIndex& idx) {
		if (!idx.isValid()) return;
		const qint64 fileId = idx.data(McJobListModel::FileIdRole).toLongLong();
		const QPoint pos    = m_listView->viewport()->mapFromGlobal(QCursor::pos());
		const QRect  iRect  = m_listView->visualRect(idx);
		const auto*  cardDelegate = qobject_cast<McCardDelegate*>(m_listView->itemDelegate());
		const int    posterW = cardDelegate ? cardDelegate->posterColumnWidth() : McJobCardDelegate::kPosterW;
		if (pos.x() <= iRect.left() + posterW)
			emit editImdbLinkRequested(fileId);
		else
			emit previewRequested(fileId);
	});

	// Context menu
	connect(m_listView, &QListView::customContextMenuRequested,
	        this, [this](const QPoint& pos) {
		const QModelIndex idx = m_listView->indexAt(pos);
		if (!idx.isValid()) return;
		const qint64  fileId   = idx.data(McJobListModel::FileIdRole).toLongLong();
		const qint64  jobId    = idx.data(McJobListModel::JobIdRole).toLongLong();
		const QString status   = idx.data(McJobListModel::StatusRole).toString();
		const QString filePath = idx.data(McJobListModel::FilePathRole).toString();

		// Detect whether the right-click landed on a specific track badge.
		// customContextMenuRequested gives pos in list-view widget coordinates;
		// hitTestBadgeStream and visualRect() both work in viewport coordinates.
		const QPoint  vpPos    = m_listView->viewport()->mapFrom(m_listView, pos);
		const QString origLang = idx.data(McJobListModel::OriginalLanguageRole).toString();
		int hitStreamIdx = -1;
		const bool isEditable = (status == QLatin1String("proposed") || status == QLatin1String("queued"));
		if (isEditable) {
			if (auto* del = qobject_cast<McCardDelegate*>(m_listView->itemDelegate())) {
				const auto streams = idx.data(McJobListModel::AllStreamsRole).value<QList<StreamRecord>>();
				const bool hasImdb = !idx.data(McJobListModel::ImdbIdRole).toString().isEmpty();
				const bool hasTmdb = idx.data(McJobListModel::TmdbIdRole).toInt() > 0;
				hitStreamIdx = del->hitTestBadgeStream(
					vpPos, m_listView->visualRect(idx), streams, m_listView->font(), hasImdb, origLang, hasTmdb);
			}
		}

		QMenu menu(this);

		// Badge right-click: show ONLY flag toggles, nothing else.
		if (hitStreamIdx >= 0) {
			const auto streams = idx.data(McJobListModel::AllStreamsRole).value<QList<StreamRecord>>();
			const StreamRecord* hitStream = nullptr;
			for (const StreamRecord& s : streams) {
				if (s.streamIndex == hitStreamIdx) { hitStream = &s; break; }
			}
			if (hitStream) {
				const StreamRecord streamCopy = *hitStream;
				buildTrackFlagMenu(menu, streamCopy, origLang, devicePixelRatioF(), /*showFlagRowsForExternal=*/true,
					[this, idx, hitStreamIdx](const QString& flag, bool value) {
						m_model->setStreamFlag(idx, hitStreamIdx, flag, value);
					},
					[this, idx, streamCopy, fileId, filePath](const QString& code) {
						if (streamCopy.isExternal) {
							if (streamCopy.externalPath.isEmpty()) return;
							const QString videoBaseName = QFileInfo(filePath).completeBaseName();
							const QString newPath = ActionEngine::insertLanguageIntoSidecarPath(
								streamCopy.externalPath, videoBaseName, code);
							if (!QFile::rename(streamCopy.externalPath, newPath)) return;
							// Patch the DB row directly instead of a full rescan — a rescan
							// unconditionally deletes any proposed/queued job for this file
							// (JobQueue::rescanFile), which would destroy the very job
							// whose badge we're trying to update.
							DatabaseManager::instance().updateStreamExternalInfo(
								fileId, streamCopy.streamIndex, code, newPath);
							m_model->updateExternalStreamInfo(fileId, streamCopy.streamIndex, code, newPath);
						} else {
							m_model->setStreamLanguage(idx, streamCopy.streamIndex, code);
						}
					});
			}
			menu.exec(m_listView->viewport()->mapToGlobal(pos));
			return;
		}

		// Collect all selected job IDs grouped by status so batch actions cover the
		// full selection rather than only the right-clicked item.
		QList<qint64> selectedProposedJobIds;
		QList<qint64> selectedQueuedJobIds;
		QList<qint64> selectedFailedJobIds;   // failed only → retryable
		QList<qint64> selectedRemovableJobIds; // all non-running selected jobs
		int firstSelRow = idx.row();
		{
			QSet<int> seen;
			for (const QModelIndex& si : m_listView->selectionModel()->selectedIndexes()) {
				if (seen.contains(si.row())) continue;
				seen.insert(si.row());
				firstSelRow = qMin(firstSelRow, si.row());
				const qint64  jid = si.data(McJobListModel::JobIdRole).toLongLong();
				const QString st  = si.data(McJobListModel::StatusRole).toString();
				if (jid <= 0) continue;
				if (st == QLatin1String("proposed")) selectedProposedJobIds << jid;
				else if (st == QLatin1String("queued")) selectedQueuedJobIds << jid;
				else if (st == QLatin1String("failed")) selectedFailedJobIds << jid;
				if (st != QLatin1String("running")) selectedRemovableJobIds << jid;
			}
			// Ensure the right-clicked item is included even if outside the selection.
			if (status == QLatin1String("proposed") && !selectedProposedJobIds.contains(jobId))
				selectedProposedJobIds.prepend(jobId);
			else if (status == QLatin1String("queued") && !selectedQueuedJobIds.contains(jobId))
				selectedQueuedJobIds.prepend(jobId);
			else if (status == QLatin1String("failed") && !selectedFailedJobIds.contains(jobId))
				selectedFailedJobIds.prepend(jobId);
			if (status != QLatin1String("running") && !selectedRemovableJobIds.contains(jobId))
				selectedRemovableJobIds.prepend(jobId);
		}

		if (status == QLatin1String("proposed")) {
			const QString queueLabel = selectedProposedJobIds.size() > 1
			    ? tr("&Queue %1 Files").arg(selectedProposedJobIds.size())
			    : tr("&Queue File");
			auto* queueAct = menu.addAction(svgIcon(":/icons/add_to_queue.svg"), queueLabel);
			connect(queueAct, &QAction::triggered, this, [this, selectedProposedJobIds, firstSelRow] {
				DatabaseManager::instance().promoteJobsToQueued(selectedProposedJobIds);
				for (qint64 jid : selectedProposedJobIds)
					m_model->updateJob(jid, "queued");
				updateFooter();
				if (!selectedProposedJobIds.isEmpty())
					markJobForFocus(selectedProposedJobIds.first());
				const QString filter = m_statusFilter->currentData().toString();
				if (filter == QLatin1String("running") || filter == QLatin1String("queued"))
					focusJob(selectedProposedJobIds.first());
				else {
					const int n = m_model->rowCount();
					if (n > 0) {
						const QModelIndex next = m_model->index(qMin(firstSelRow, n - 1), 0);
						m_listView->selectionModel()->setCurrentIndex(next, QItemSelectionModel::ClearAndSelect);
						m_listView->scrollTo(next);
					}
				}
			});
			menu.addSeparator();
		} else if (status == QLatin1String("queued")) {
			// Run this specific job immediately, bypassing queue order.
			auto* runNowAct = menu.addAction(svgIcon(":/icons/play_arrow.svg"), tr("&Start Now"));
			runNowAct->setEnabled(m_queue && m_queue->canRunJobImmediately(jobId));
			connect(runNowAct, &QAction::triggered, this, [this, jobId] {
				if (m_queue) m_queue->runJob(jobId);
			});

			const QString unqueueLabel = selectedQueuedJobIds.size() > 1
			    ? tr("&Unqueue %1 Files").arg(selectedQueuedJobIds.size())
			    : tr("&Unqueue File");
			auto* unqueueAct = menu.addAction(svgIcon(":/icons/undo.svg"), unqueueLabel);
			connect(unqueueAct, &QAction::triggered, this, [this, selectedQueuedJobIds, firstSelRow] {
				for (qint64 jid : selectedQueuedJobIds) {
					DatabaseManager::instance().updateJobStatus(jid, "proposed");
					m_model->updateJob(jid, "proposed");
				}
				updateFooter();
				const int n = m_model->rowCount();
				if (n > 0) {
					const QModelIndex next = m_model->index(qMin(firstSelRow, n - 1), 0);
					m_listView->selectionModel()->setCurrentIndex(next, QItemSelectionModel::ClearAndSelect);
					m_listView->scrollTo(next);
				}
			});
			menu.addSeparator();
		} else if (status == QLatin1String("running")) {
			auto* cancelJobAct = menu.addAction(svgIcon(":/icons/stop_circle.svg"),
			                                    tr("&Cancel This Job"));
			cancelJobAct->setToolTip(tr("Stop just this job — other running jobs are unaffected"));
			connect(cancelJobAct, &QAction::triggered, this, [this, jobId] {
				if (m_queue) m_queue->cancelJob(jobId);
			});
			menu.addSeparator();
		} else if (!selectedFailedJobIds.isEmpty()) {
			const QString retryLabel = selectedFailedJobIds.size() > 1
			    ? tr("&Retry %1 Jobs").arg(selectedFailedJobIds.size())
			    : tr("&Retry Job");
			auto* retryAct = menu.addAction(svgIcon(":/icons/refresh.svg"), retryLabel);
			connect(retryAct, &QAction::triggered, this, [this, selectedFailedJobIds, firstSelRow] {
				DatabaseManager::instance().requeueFailedJobs(selectedFailedJobIds);
				for (qint64 jid : selectedFailedJobIds)
					m_model->updateJob(jid, "queued");
				updateFooter();
				const int n = m_model->rowCount();
				if (n > 0) {
					const QModelIndex next = m_model->index(qMin(firstSelRow, n - 1), 0);
					m_listView->selectionModel()->setCurrentIndex(next, QItemSelectionModel::ClearAndSelect);
					m_listView->scrollTo(next);
				}
			});
			menu.addSeparator();
		}

		auto* previewAct = menu.addAction(svgIcon(":/icons/visibility.svg"),
		                                  tr("&Preview Tracks…"));
		connect(previewAct, &QAction::triggered, this, [this, fileId] {
			emit previewRequested(fileId);
		});

		auto* previewCmdAct = menu.addAction(svgIcon(":/icons/terminal.svg"),
		                                     tr("Preview &Command…"));
		connect(previewCmdAct, &QAction::triggered, this, [this, jobId] {
			onPreviewCommand(jobId);
		});

		menu.addSeparator();

		// Collect all selected file IDs so the context menu can trigger a batch flow.
		QList<qint64> selectedFileIds;
		{
			QSet<int> seen;
			for (const QModelIndex& si : m_listView->selectionModel()->selectedIndexes()) {
				if (seen.contains(si.row())) continue;
				seen.insert(si.row());
				const qint64 fid = si.data(McJobListModel::FileIdRole).toLongLong();
				if (fid > 0) selectedFileIds << fid;
			}
			// Ensure the right-clicked file is in the list even if it wasn't in the selection.
			if (!selectedFileIds.contains(fileId)) {
				selectedFileIds.prepend(fileId);
			}
		}

		const QString refreshPosterLabel = selectedFileIds.size() > 1
		    ? tr("Refresh %1 &Posters").arg(selectedFileIds.size())
		    : tr("Refresh &Poster");
		auto* refreshPosterAct = menu.addAction(svgIcon(":/icons/refresh.svg"), refreshPosterLabel);
		connect(refreshPosterAct, &QAction::triggered, this, [this, fileId, selectedFileIds] {
			if (selectedFileIds.size() > 1)
				emit refreshPosterBatchRequested(selectedFileIds);
			else
				emit refreshPosterRequested(fileId);
		});

		auto* dlSubsAct = menu.addAction(svgIcon(":/icons/translate.svg"),
		                                 tr("&Download Subtitles…"));
		connect(dlSubsAct, &QAction::triggered, this, [this, fileId] {
			emit downloadSubtitlesRequested(fileId);
		});

		const QString imdbLabel = selectedFileIds.size() > 1
		    ? tr("Edit &Movie Metadata (%1 files)…").arg(selectedFileIds.size())
		    : tr("Edit &Movie Metadata…");
		auto* imdbAct = menu.addAction(svgIcon(":/icons/link.svg"), imdbLabel);
		connect(imdbAct, &QAction::triggered, this, [this, fileId, selectedFileIds] {
			if (selectedFileIds.size() > 1)
				emit editImdbLinksRequested(selectedFileIds);
			else
				emit editImdbLinkRequested(fileId);
		});

		menu.addSeparator();

		auto* openFolderAct = menu.addAction(svgIcon(":/icons/folder_open.svg"),
		                                     tr("Open &Containing Folder"));
		connect(openFolderAct, &QAction::triggered, this, [fileId] {
			const auto fileOpt = DatabaseManager::instance().fileById(fileId);
			if (fileOpt)
				QDesktopServices::openUrl(
					QUrl::fromLocalFile(QFileInfo(fileOpt->path).absolutePath()));
		});

		// Show log for any job that has actually run
		const bool hasLog = (status == QLatin1String("done")
		                  || status == QLatin1String("failed")
		                  || status == QLatin1String("cancelled"));
		if (hasLog) {
			auto* viewLogAct = menu.addAction(svgIcon(":/icons/visibility.svg"),
			                                  tr("View &Log…"));
			connect(viewLogAct, &QAction::triggered, this, [this, jobId] {
				const auto rec = DatabaseManager::instance().jobById(jobId);
				if (!rec) return;

				auto* dlg = new QDialog(this);
				dlg->setWindowTitle(tr("Job Log — %1").arg(rec->summary));
				dlg->setAttribute(Qt::WA_DeleteOnClose);
				dlg->resize(760, 480);

				auto* vlay = new QVBoxLayout(dlg);

				// Header info row
				auto* infoLabel = new QLabel(dlg);
				const auto fmtTime = [](qint64 ts) -> QString {
					if (ts <= 0) return tr("—");
					return QDateTime::fromSecsSinceEpoch(ts).toString(Qt::ISODate);
				};
				infoLabel->setText(tr("Status: <b>%1</b>  |  Exit code: <b>%2</b>  |  Started: %3  |  Finished: %4")
					.arg(rec->status)
					.arg(rec->resultCode)
					.arg(fmtTime(rec->startedAt))
					.arg(fmtTime(rec->finishedAt)));
				infoLabel->setTextFormat(Qt::RichText);
				vlay->addWidget(infoLabel);

				// ── Savings + per-track breakdown ────────────────────────────
				qint64 fileSizeBytes = 0;
				if (const auto f = DatabaseManager::instance().fileById(rec->fileId))
					fileSizeBytes = f->sizeBytes;

				const auto fmtMB = [](qint64 b) {
					return QStringLiteral("%1 MB").arg(b / 1048576.0, 0, 'f', 2);
				};
				const auto fmtWithPct = [&](qint64 b, const QString& prefix) -> QString {
					if (fileSizeBytes <= 0) return prefix + fmtMB(b);
					return QStringLiteral("%1%2 (%3%)").arg(prefix, fmtMB(b))
					           .arg(100.0 * b / fileSizeBytes, 0, 'f', 1);
				};

				// Per-track breakdown of the estimate frozen at job creation. This must
				// read back the stored per-track estimatedBytes rather than re-derive it
				// from bitrate × duration: the real estimator (perStreamEstimates() in
				// TrackDecision.h) allocates each track a PROPORTIONAL SHARE of the whole
				// file's actual size (bitrate / total-file-bitrate × fileSize), using the
				// full stream list — which isn't available here (only removed tracks are
				// stored). A bitrate × duration recompute answers a different question
				// and can land far from the real estimate, which is exactly the "1.9 GB
				// recompute vs 4.8 GB actual estimate" kind of mismatch this replaces.
				const QJsonArray estArr = QJsonDocument::fromJson(
				    rec->streamEstimatesJson.toUtf8()).array();

				struct TrackGroup {
					int    count      = 0;
					qint64 totalBytes = 0;
					bool   declared   = false;
				};
				QMap<QString, TrackGroup> groups; // key: "codecType|codecName|D/F"
				qint64 groupedTotal = 0;

				for (const QJsonValue& v : estArr) {
					const QJsonObject o = v.toObject();
					const QString codecType    = o[QStringLiteral("codecType")].toString();
					const QString codecName    = o[QStringLiteral("codecName")].toString();
					const QString codecProfile = o[QStringLiteral("codecProfile")].toString();
					const qint64  declared     = o[QStringLiteral("declaredBitrate")].toVariant().toLongLong();
					const qint64  trackBytes   = o[QStringLiteral("estimatedBytes")].toVariant().toLongLong();

					groupedTotal += trackBytes;

					// Use the profile-aware format key (not raw codecName) so DTS-HD MA
					// doesn't display as indistinguishable plain "dts" — ffprobe reports
					// the same codec_name for both; codecProfile is what tells them apart.
					const QString formatKey = Mc::calibrationFormatKey(codecName, codecType, codecProfile);
					const QString label = !formatKey.isEmpty() ? formatKey
					                     : codecName.isEmpty() ? codecType : codecName;
					const QString key   = codecType + QChar('|') + label
					                    + QChar('|') + (declared > 0 ? QChar('D') : QChar('F'));
					auto& g = groups[key];
					g.count++;
					g.totalBytes += trackBytes;
					g.declared = declared > 0;
				}

				// The estimate compared against "Actual" must be the one frozen at job
				// creation — the same value the card showed before the job ran. Using a
				// recompute here would be misleading: updateCalibrationFromJob() already
				// folds this job's own actual-vs-estimated ratio into the fallback bitrate
				// table as soon as it commits, so a "fresh" recompute would be answering a
				// different question ("what would today's calibration predict?") and can
				// legitimately land far from what was actually predicted at the time.
				const qint64 origEstimate = rec->estimatedSavedBytes;
				const bool showSavings  = rec->savedBytes > 0 || origEstimate > 0;
				QPlainTextEdit* detailEdit = nullptr;
				if (showSavings) {
					auto* savingsLabel = new QLabel(dlg);
					savingsLabel->setTextFormat(Qt::RichText);

					if (rec->savedBytes > 0 && origEstimate > 0) {
						const qint64  diff  = rec->savedBytes - origEstimate;
						const QString sign  = diff >= 0 ? QStringLiteral("+") : QStringLiteral("-");
						const QString color = qAbs(diff) < 1048576 ? QStringLiteral("green")
						                   : diff > 0              ? QStringLiteral("darkorange")
						                   :                         QStringLiteral("crimson");
						savingsLabel->setText(
							tr("Estimated: <b>%1</b>  |  Actual: <b>%2</b>  |  Delta: <b><span style='color:%3'>%4%5</span></b>")
							.arg(fmtWithPct(origEstimate, QStringLiteral("~")),
							     fmtWithPct(rec->savedBytes, QString()),
							     color, sign, fmtMB(qAbs(diff))));
					} else {
						savingsLabel->setText(
							tr("Estimated: <b>%1</b>  |  Actual: <b>%2</b>")
							.arg(origEstimate > 0 ? fmtWithPct(origEstimate, QStringLiteral("~")) : tr("—"),
							     rec->savedBytes > 0 ? fmtWithPct(rec->savedBytes, QString()) : tr("—")));
					}
					vlay->addWidget(savingsLabel);

					// Per-track grouped breakdown
					if (!groups.isEmpty()) {
						QStringList lines;
						lines.reserve(groups.size() + 2);
						lines << tr("Per-track breakdown (as estimated when queued):");
						for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
							const auto& g = it.value();
							const QString label  = it.key().section(QChar('|'), 1, 1);
							const QString source = g.declared ? tr("declared") : tr("fallback");
							const QString cnt    = g.count > 1
							    ? QStringLiteral("%1 × ").arg(g.count) : QString();
							lines << QStringLiteral("  %1%2  (%3)  →  ~%4 MB")
							         .arg(cnt, label, source)
							         .arg(g.totalBytes / 1048576.0, 0, 'f', 1);
						}
						lines << QStringLiteral("  Total:  ~%1 MB")
						         .arg(groupedTotal / 1048576.0, 0, 'f', 1);

						detailEdit = new QPlainTextEdit(lines.join(QLatin1Char('\n')));
						detailEdit->setReadOnly(true);
						detailEdit->setFont(QFont(QStringLiteral("Courier New"), 9));
						detailEdit->setWordWrapMode(QTextOption::NoWrap);
					}
				}

				// Splitter — detail pane (when present) sits above the mkvmerge log
				auto* splitter = new QSplitter(Qt::Vertical, dlg);
				if (detailEdit)
					splitter->addWidget(detailEdit);

				auto* logEdit = new QPlainTextEdit(splitter);
				logEdit->setReadOnly(true);
				logEdit->setFont(QFont(QStringLiteral("Courier New"), 9));
				logEdit->setWordWrapMode(QTextOption::NoWrap);

				QString logText = rec->outputLog.isEmpty()
					? tr("(no output captured)")
					: rec->outputLog;
				// Average throughput for the job — bytes read (original size, before any
				// tracks were dropped) over wall-clock time. fileSizeBytes here is the
				// file's CURRENT (post-remux) size, so add back what was reclaimed.
				if (rec->status == QLatin1String("done") && rec->finishedAt > rec->startedAt) {
					const qint64 jobElapsedSec = rec->finishedAt - rec->startedAt;
					const qint64 originalBytes = fileSizeBytes + qMax<qint64>(0, rec->savedBytes);
					if (originalBytes > 0) {
						const double mbPerSec = originalBytes / 1048576.0 / jobElapsedSec;
						logText += tr("\nAverage speed: %1 MB/s").arg(mbPerSec, 0, 'f', 1);
					}
				}
				logEdit->setPlainText(logText);
				splitter->addWidget(logEdit);

				if (detailEdit) {
					const int lineH   = QFontMetrics(detailEdit->font()).height() + 4;
					const int detailH = lineH * (detailEdit->document()->blockCount() + 1);
					splitter->setSizes({ detailH, 300 });
				}
				vlay->addWidget(splitter);

				// Persist geometry and splitter ratio across sessions
				{
					QSettings s;
					if (const QByteArray g = s.value(QStringLiteral("jobLogDialog/geometry")).toByteArray(); !g.isEmpty()) {
						dlg->restoreGeometry(g);
						ensureGeometryFitsScreen(dlg);
					}
					if (const QByteArray sp = s.value(QStringLiteral("jobLogDialog/splitter")).toByteArray(); !sp.isEmpty())
						splitter->restoreState(sp);
				}
				connect(dlg, &QDialog::finished, dlg, [dlg, splitter] {
					QSettings s;
					s.setValue(QStringLiteral("jobLogDialog/geometry"), dlg->saveGeometry());
					s.setValue(QStringLiteral("jobLogDialog/splitter"), splitter->saveState());
				});

				auto* btnBox = new QDialogButtonBox(dlg);
				auto* copyBtn = btnBox->addButton(tr("Copy to Clipboard"), QDialogButtonBox::ActionRole);
				btnBox->addButton(QDialogButtonBox::Close);
				connect(copyBtn, &QPushButton::clicked, this, [detailEdit, logEdit] {
					QString text;
					if (detailEdit && !detailEdit->toPlainText().isEmpty())
						text = detailEdit->toPlainText() + QStringLiteral("\n\n");
					text += logEdit->toPlainText();
					QApplication::clipboard()->setText(text);
				});
				connect(btnBox, &QDialogButtonBox::rejected, dlg, &QDialog::accept);
				vlay->addWidget(btnBox);

				dlg->show();
			});
		}

		if (!selectedRemovableJobIds.isEmpty()) {
			menu.addSeparator();
			const QString removeLabel = selectedRemovableJobIds.size() > 1
			    ? tr("Remove %1 Jobs from Queue").arg(selectedRemovableJobIds.size())
			    : tr("Remove from Queue");
			auto* removeAct = menu.addAction(svgIcon(":/icons/delete.svg"), removeLabel);
			connect(removeAct, &QAction::triggered, this, [this, selectedRemovableJobIds, firstSelRow] {
				const QString msg = selectedRemovableJobIds.size() == 1
				    ? tr("Remove this job from the queue?\n\n"
				         "Any track-selection or flag changes made since the last analysis will be lost.")
				    : tr("Remove %1 jobs from the queue?\n\n"
				         "Any track-selection or flag changes made since the last analysis will be lost.")
				         .arg(selectedRemovableJobIds.size());
				if (QMessageBox::question(this, tr("Remove from Queue"), msg,
				                          QMessageBox::Yes | QMessageBox::Cancel,
				                          QMessageBox::Cancel) != QMessageBox::Yes)
					return;

				m_model->removeJobIds(selectedRemovableJobIds);
				updateFooter();
				emit jobsChanged(m_model->rowCount());
				const int n = m_model->rowCount();
				if (n > 0) {
					const QModelIndex next = m_model->index(qMin(firstSelRow, n - 1), 0);
					m_listView->selectionModel()->setCurrentIndex(next, QItemSelectionModel::ClearAndSelect);
					m_listView->scrollTo(next);
				}
				QThreadPool::globalInstance()->start([ids = selectedRemovableJobIds] {
					DatabaseManager::instance().deleteJobsBatch(ids);
				});
			});
		}

#ifdef QT_DEBUG
		menu.addSeparator();
		auto* dbgAct = menu.addAction(tr("[Debug] Simulate Track Mismatch…"));
		connect(dbgAct, &QAction::triggered, this, [this, jobId] {
			emit debugReviewRequested(jobId);
		});
#endif

		menu.exec(m_listView->viewport()->mapToGlobal(pos));
	});

	root->addWidget(m_listView, 1);

	m_etaTimer = new QTimer(this);
	m_etaTimer->setInterval(1000);
	connect(m_etaTimer, &QTimer::timeout, this, &McJobPanel::updateFooter);
}

void McJobPanel::setJobQueue(JobQueue* queue)
{
	m_queue = queue;
	queue->setSortMode(static_cast<JobSortMode>(
		AppSettings::instance().value("jobPanel/sortMode", 0).toInt()));

	connect(queue, &JobQueue::jobStarted, this, [this](qint64 jobId) {
		// Capture selection before model resets fire (updateJob and/or filter switch
		// both call applyFilter → beginResetModel which clears the selection model).
		QSet<qint64> prevSelected;
		for (const QModelIndex& si : m_listView->selectionModel()->selectedIndexes())
			prevSelected.insert(si.data(McJobListModel::JobIdRole).toLongLong());

		onJobStatusChanged(jobId, "running");
		JobEtaState st;
		st.startedAtMs = QDateTime::currentMSecsSinceEpoch();
		m_etaByJob.insert(jobId, st);
		if (!m_jobTimer.isValid())
			m_jobTimer.start();
		if (!m_etaTimer->isActive())
			m_etaTimer->start();

		const bool followRunning   = AppSettings::instance().value("jobPanel/followRunning", true).toBool();
		const QString currentFilter = m_statusFilter->currentData().toString();
		const bool onRunningFilter  = (currentFilter == QLatin1String("running"));

		if (!followRunning && !onRunningFilter) {
			// followRunning is off and user isn't watching the Running filter — restore
			// selection only, no filter switch, no auto-scroll.
			for (int row = 0; row < m_model->rowCount(); ++row) {
				const QModelIndex idx = m_model->index(row);
				if (prevSelected.contains(idx.data(McJobListModel::JobIdRole).toLongLong()))
					m_listView->selectionModel()->select(idx, QItemSelectionModel::Select);
			}
			return;
		}

		// Auto-switch non-Running filters to Running when followRunning is enabled.
		if (followRunning && !currentFilter.isEmpty()) {
			for (int i = 0; i < m_statusFilter->count(); ++i) {
				if (m_statusFilter->itemData(i).toString() == QLatin1String("running")) {
					m_statusFilter->setCurrentIndex(i);
					break;
				}
			}
		}

		// Restore selection for any previously selected items still visible — never
		// auto-select a runner just because processing started.
		for (int row = 0; row < m_model->rowCount(); ++row) {
			const QModelIndex idx = m_model->index(row);
			if (prevSelected.contains(idx.data(McJobListModel::JobIdRole).toLongLong()))
				m_listView->selectionModel()->select(idx, QItemSelectionModel::Select);
		}

		// Scroll the newly running job into view (selection is left untouched above).
		// Deferred a tick: the filter switch above resets the model synchronously, and
		// the view needs an event-loop turn to finish laying out before scrollTo works.
		QTimer::singleShot(0, this, [this, jobId] {
			for (int row = 0; row < m_model->rowCount(); ++row) {
				const QModelIndex idx = m_model->index(row);
				if (idx.data(McJobListModel::JobIdRole).toLongLong() == jobId) {
					m_listView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
					break;
				}
			}
		});
	});
	connect(queue, &JobQueue::jobRequeued, this, [this](qint64 jobId) {
		m_etaByJob.remove(jobId);
		m_model->updateJob(jobId, QStringLiteral("queued"));
		if (DatabaseManager::instance().jobStatusCounts().value(QStringLiteral("running"), 0) == 0) {
			m_etaTimer->stop();
			m_jobTimer.invalidate();
		}
		updateFooter();
	});
	connect(queue, &JobQueue::jobFinished, this,
	        [this](qint64 jobId, bool ok, qint64 savedBytes) {
		m_etaByJob.remove(jobId);
		QString status;
		if (ok) {
			status = QStringLiteral("done");
		} else {
			const auto jobOpt = DatabaseManager::instance().jobById(jobId);
			status = (jobOpt && jobOpt->status == QLatin1String("queued"))
			    ? QStringLiteral("queued") : QStringLiteral("failed");
		}
		m_model->updateJob(jobId, status, savedBytes);
		if (DatabaseManager::instance().jobStatusCounts().value(QStringLiteral("running"), 0) == 0) {
			m_etaTimer->stop();
			m_jobTimer.invalidate();
			m_etaByJob.clear();
		}
		updateFooter();
	});
	connect(queue, &JobQueue::allFinished, this, [this] {
		refresh();
		// When auto-track left the filter on "Running" and there's nothing left
		// to run, jump to Queued (or Proposed) so the user can restart without manual steps.
		if (AppSettings::instance().value("jobPanel/followRunning", true).toBool()
		    && m_statusFilter->currentData().toString() == QLatin1String("running")) {
			const auto counts = m_model->countsByStatus();
			const QString target = counts.value(QStringLiteral("queued"), 0) > 0
			    ? QStringLiteral("queued")
			    : (counts.value(QStringLiteral("proposed"), 0) > 0
			        ? QStringLiteral("proposed") : QString());
			for (int i = 0; i < m_statusFilter->count(); ++i) {
				if (m_statusFilter->itemData(i).toString() == target) {
					m_statusFilter->setCurrentIndex(i);
					break;
				}
			}
		}
	});
	connect(queue, &JobQueue::progressChanged,
	        m_model, &McJobListModel::updateProgress);
	connect(queue, &JobQueue::outputSizeChanged,
	        m_model, &McJobListModel::updateOutputSize);
	connect(queue, &JobQueue::phaseChanged,
	        m_model, &McJobListModel::updatePhase);

	// Aggregate goal progress bar — completed bytes update per finished job, the
	// live sliver fills in for whichever job is currently running.
	connect(queue, &JobQueue::goalProgressChanged, this,
	        [this](qint64 savedBytes, qint64 goalBytes) {
		if (!m_goalBar) return;
		m_goalBar->setGoal(goalBytes);
		m_goalBar->setCompleted(savedBytes);
		m_goalBar->setLive(0);
		m_goalBar->setVisible(true);
	});
	connect(queue, &JobQueue::progressChanged, this, [this](qint64, int percent) {
		if (!m_goalBar || !m_queue || !m_queue->isGoalMode()) return;
		m_goalBar->setLive(m_queue->currentJobEstimatedSavings() * percent / 100);
	});
	connect(queue, &JobQueue::goalReached, this,
	        [this](qint64 savedBytes, qint64 goalBytes, int jobsCompleted, qint64 elapsedMs) {
		QMessageBox::information(this, tr("Goal Reached"),
		    tr("Freed %1 (goal was %2) across %n file(s) in %3.", "", jobsCompleted)
		        .arg(formatSaved(savedBytes), formatSaved(goalBytes),
		             formatEtaDuration(elapsedMs / 1000.0)));
	});

	// A rescan (e.g. after renaming a sidecar subtitle from the badge menu) runs
	// asynchronously and updates streams/jobs in the DB out from under the model —
	// reload so badges reflect the new state instead of showing stale pre-rescan data.
	// Skip the reload when there's no live (proposed/queued/running) job for this file:
	// that's the routine post-completion rescan, whose "done" card reads from a frozen
	// stream snapshot anyway, so a full reload would just be wasted work.
	connect(queue, &JobQueue::fileRescanned, this, [this](qint64 fileId) {
		if (DatabaseManager::instance().hasActiveJobForFile(fileId))
			refresh();
	});

	connect(&DatabaseManager::instance(), &DatabaseManager::jobStatusChanged,
	        this, &McJobPanel::onJobStatusChanged);

	connect(&PosterManager::instance(), &PosterManager::posterReady,
	        m_model, &McJobListModel::onPosterReady);
	connect(&PosterManager::instance(), &PosterManager::fanartReady,
	        m_model, &McJobListModel::onFanartReady);
	connect(&PosterManager::instance(), &PosterManager::imdbIdSaved,
	        m_model, &McJobListModel::updateImdbId);
	connect(&PosterManager::instance(), &PosterManager::tmdbIdSaved,
	        m_model, &McJobListModel::updateTmdbId);

	// m_model didn't exist yet when the storage-group chip row was built above —
	// apply the chips' (all-checked) initial state now that it does.
	applyStorageGroupFilter();
}

void McJobPanel::setTmdbConfigured(bool configured)
{
	if (auto* d = qobject_cast<McCardDelegate*>(m_listView->itemDelegate()))
		d->setTmdbConfigured(configured);
}

void McJobPanel::setMultiGroupBadgeEnabled(bool enabled)
{
	if (auto* d = qobject_cast<McCardDelegate*>(m_listView->itemDelegate()))
		d->setMultiGroupBadgeEnabled(enabled);
}

void McJobPanel::setFanartOpacity(double opacity)
{
	if (auto* d = qobject_cast<McCardDelegate*>(m_listView->itemDelegate()))
		d->setFanartOpacity(opacity);
}

void McJobPanel::refreshStorageGroups()
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
		applyStorageGroupFilter();
		return;
	}
	std::sort(groups.begin(), groups.end());

	m_storageGroupLayout->addWidget(vSep(m_storageGroupContainer));
	for (int g : groups) {
		auto* chip = new McStorageGroupChipToggle(g, m_storageGroupContainer);
		connect(chip, &McStorageGroupChipToggle::toggled, this, [this](bool) { applyStorageGroupFilter(); });
		m_storageGroupLayout->addWidget(chip);
		m_storageGroupChips.append(chip);
	}
	m_storageGroupContainer->setVisible(true);
	applyStorageGroupFilter();
}

void McJobPanel::applyStorageGroupFilter()
{
	// The chip row is built (in the filter bar) before m_model exists (created later
	// in setupUi()) — the initial refreshStorageGroups() call during construction is a
	// no-op here; setupUi() applies the built chip state to m_model once it exists.
	if (!m_model) return;
	quint32 mask = 0;
	for (McStorageGroupChipToggle* chip : m_storageGroupChips)
		if (chip->isChecked())
			mask |= (1u << chip->group());
	m_model->setStorageGroupFilter(mask);
}

void McJobPanel::refresh()
{
	m_model->reload();
	setMediaCategoryFiltersVisible(m_model->hasClassifiedMediaTypes());
	updateFooter();
	emit jobsChanged(m_model->rowCount());
}

void McJobPanel::refreshPaged(int limit)
{
	m_model->reloadPaged(limit);
	setMediaCategoryFiltersVisible(m_model->hasClassifiedMediaTypes());
	updateFooter();
	emit jobsChanged(m_model->rowCount());
}

QList<qint64> McJobPanel::visibleFileIds() const
{
	QList<qint64> ids;
	ids.reserve(m_model->rowCount());
	for (int row = 0; row < m_model->rowCount(); ++row)
		ids << m_model->index(row).data(McJobListModel::FileIdRole).toLongLong();
	return ids;
}

void McJobPanel::markJobForFocus(qint64 jobId)
{
	if (jobId > 0)
		m_focusJobId = jobId;
}

void McJobPanel::focusJob(qint64 jobId)
{
	if (jobId <= 0) return;

	for (int row = 0; row < m_model->rowCount(); ++row) {
		const QModelIndex idx = m_model->index(row);
		if (idx.data(McJobListModel::JobIdRole).toLongLong() != jobId)
			continue;
		m_listView->setCurrentIndex(idx);
		m_listView->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect);
		m_listView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
		if (m_focusJobId == jobId)
			m_focusJobId = -1;
		return;
	}
}

void McJobPanel::scrollToFileJob(qint64 fileId)
{
	// Only select+scroll when the current filter already shows proposed jobs.
	// "All" (empty data) also qualifies. Don't force a filter change — the user
	// may be looking at done/failed jobs and a sudden filter switch is disruptive.
	const QString currentFilter = m_statusFilter->currentData().toString();
	if (!currentFilter.isEmpty() && currentFilter != QLatin1String("proposed"))
		return;

	for (int row = 0; row < m_model->rowCount(); ++row) {
		const QModelIndex idx = m_model->index(row);
		if (idx.data(McJobListModel::FileIdRole).toLongLong() == fileId) {
			m_listView->setCurrentIndex(idx);
			m_listView->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect);
			m_listView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
			break;
		}
	}
}

void McJobPanel::syncExternalStreamLanguage(qint64 fileId, int streamIndex,
                                             const QString& language, const QString& externalPath)
{
	m_model->updateExternalStreamInfo(fileId, streamIndex, language, externalPath);
}

void McJobPanel::removeJobsForFile(qint64 fileId)
{
	const auto ids = m_model->jobIdsForFile(fileId);
	if (ids.isEmpty()) return;
	m_model->removeJobIds(ids);
	updateFooter();
	emit jobsChanged(m_model->rowCount());
}

void McJobPanel::onJobStatusChanged(qint64 jobId, const QString& status)
{
	m_model->updateJob(jobId, status);
	updateFooter();
	// If a job just became running, it may be selected — re-check Remove availability
	// since selectionChanged won't fire when only the data changes.
	if (status == QLatin1String("running")) {
		const auto selected = m_listView->selectionModel()->selectedIndexes();
		const bool hasRemovable = std::any_of(selected.cbegin(), selected.cend(),
		    [](const QModelIndex& idx) {
		        return idx.data(McJobListModel::StatusRole).toString() != QLatin1String("running");
		    });
		m_btnRemove->setEnabled(hasRemovable);
	}
}

void McJobPanel::repaintCards()
{
	m_listView->viewport()->repaint();
}

void McJobPanel::setRatingForFile(qint64 fileId, double rating)
{
	m_model->setRatingForFile(fileId, rating);
}

void McJobPanel::setTitleForFile(qint64 fileId, const QString& title, int year)
{
	m_model->setDisplayTitleForFile(fileId, title, year);
}

void McJobPanel::setMediaTypeForFile(qint64 fileId, const QString& mediaType)
{
	m_model->setMediaTypeForFile(fileId, mediaType);
	setMediaCategoryFiltersVisible(m_model->hasClassifiedMediaTypes());
}

void McJobPanel::setMediaCategoryFiltersVisible(bool visible)
{
	if (!m_mediaCategoryContainer) return;
	if (m_mediaCategoryContainer->isVisible() == visible) return;

	m_mediaCategoryContainer->setVisible(visible);

	if (!visible) {
		using QF = McFilterPanel;
		const quint32 mediaMask = QF::QF_Movie | QF::QF_Tv | QF::QF_Documentary | QF::QF_Misc;
		if (m_qfFlags & mediaMask) {
			const auto buttons = m_mediaCategoryContainer->findChildren<QToolButton*>();
			for (QToolButton* btn : buttons) {
				btn->blockSignals(true);
				btn->setChecked(false);
				btn->blockSignals(false);
			}
			m_qfFlags &= ~mediaMask;
			m_model->setQuickFilters(m_qfFlags);
		}
	}
}

void McJobPanel::onQueueSelected()
{
	// Promote selected proposed jobs to queued
	const auto selected = m_listView->selectionModel()->selectedIndexes();
	QList<qint64> ids;
	QList<qint64> fileIds;
	QSet<int> seen;
	for (const QModelIndex& idx : selected) {
		if (seen.contains(idx.row())) continue;
		seen.insert(idx.row());
		if (idx.data(McJobListModel::StatusRole).toString() == "proposed") {
			ids << idx.data(McJobListModel::JobIdRole).toLongLong();
			const qint64 fid = idx.data(McJobListModel::FileIdRole).toLongLong();
			if (!fileIds.contains(fid)) fileIds << fid;
		}
	}
	if (ids.isEmpty()) return;

	DatabaseManager::instance().promoteJobsToQueued(ids);
	// A job may have been proposed days ago while subtitles were still missing —
	// retry them now that it's about to sit in the processing queue.
	SubtitleManager::instance().enqueueBatch(fileIds);

	// Update each card in-place (no model reset) so the selection stays intact.
	for (qint64 id : ids)
		m_model->updateJob(id, "queued");
	updateFooter();

	if (!ids.isEmpty()) {
		markJobForFocus(ids.first());
		const QString filter = m_statusFilter->currentData().toString();
		if (filter == QLatin1String("running") || filter == QLatin1String("queued"))
			QTimer::singleShot(0, this, [this, id = ids.first()] { focusJob(id); });
	}

	// Disable Queue Selected — there are no more proposed items in the selection.
	m_btnQueueSelected->setEnabled(false);
}

void McJobPanel::onQueueAll()
{
	// DB-wide, not m_model->jobIdsByStatus(): the model only knows the currently
	// filtered/paged subset (e.g. status filter on "Done", or the startup page
	// before the full library has loaded), which previously made "Queue All"
	// promote only a fraction of what its enabled state (DB-wide proposed count)
	// and tooltip promised. updateJob() below pulls each job into the model's
	// master list on demand, so entries outside today's filter/page still get
	// their in-memory state (and this panel's view of it) updated correctly.
	auto& db = DatabaseManager::instance();
	const QList<qint64> ids = db.jobIdsByStatus("proposed");
	if (ids.isEmpty()) return;

	const QSet<qint64> fileIdSet = db.proposedJobFileIds();
	const QList<qint64> fileIds(fileIdSet.begin(), fileIdSet.end());
	db.promoteJobsToQueued(ids);
	SubtitleManager::instance().enqueueBatch(fileIds);

	for (qint64 id : ids)
		m_model->updateJob(id, "queued");
	updateFooter();
}

void McJobPanel::onUnqueueSelected()
{
	const auto selected = m_listView->selectionModel()->selectedIndexes();
	QList<qint64> ids;
	QSet<int> seen;
	for (const QModelIndex& idx : selected) {
		if (seen.contains(idx.row())) continue;
		seen.insert(idx.row());
		if (idx.data(McJobListModel::StatusRole).toString() == "queued")
			ids << idx.data(McJobListModel::JobIdRole).toLongLong();
	}
	if (ids.isEmpty()) return;

	auto& db = DatabaseManager::instance();
	for (qint64 id : ids) {
		db.updateJobStatus(id, "proposed");
		m_model->updateJob(id, "proposed");
	}
	updateFooter();

	m_btnUnqueue->setEnabled(false);
}

void McJobPanel::onRemoveSelected()
{
	const auto selected = m_listView->selectionModel()->selectedIndexes();
	QList<qint64> toDelete;
	QSet<int> seen;
	int firstSelRow = selected.isEmpty() ? 0 : selected.first().row();
	for (const QModelIndex& idx : selected) {
		if (seen.contains(idx.row())) continue;
		seen.insert(idx.row());
		firstSelRow = qMin(firstSelRow, idx.row());
		const QString status = idx.data(McJobListModel::StatusRole).toString();
		if (status != "running")
			toDelete << idx.data(McJobListModel::JobIdRole).toLongLong();
	}
	if (toDelete.isEmpty()) return;

	const QString msg = toDelete.size() == 1
	    ? tr("Remove this job from the queue?\n\n"
	         "Any track-selection or flag changes made since the last analysis will be lost.")
	    : tr("Remove %1 jobs from the queue?\n\n"
	         "Any track-selection or flag changes made since the last analysis will be lost.")
	         .arg(toDelete.size());
	if (QMessageBox::question(this, tr("Remove from Queue"), msg,
	                          QMessageBox::Yes | QMessageBox::Cancel,
	                          QMessageBox::Cancel) != QMessageBox::Yes)
		return;

	m_model->removeJobIds(toDelete);
	updateFooter();
	emit jobsChanged(m_model->rowCount());
	const int n = m_model->rowCount();
	if (n > 0) {
		const QModelIndex next = m_model->index(qMin(firstSelRow, n - 1), 0);
		m_listView->selectionModel()->setCurrentIndex(next, QItemSelectionModel::ClearAndSelect);
		m_listView->scrollTo(next);
	}
	QThreadPool::globalInstance()->start([ids = toDelete] {
		DatabaseManager::instance().deleteJobsBatch(ids);
	});
}

void McJobPanel::onStartPause()
{
	if (!m_queue) return;
	if (m_queue->isPaused()) {
		m_queue->start();
	} else if (m_queue->isRunning()) {
		m_queue->pause();
	} else {
		// Files may have been queued a while ago and are still missing subtitles
		// (e.g. the OpenSubtitles quota ran out mid-scan) — retry them now that
		// processing is about to start, rather than only at scan time.
		SubtitleManager::instance().enqueueBatch(m_model->fileIdsByStatus("queued"));
		// The disk-space check in startJob() calls QStorageInfo, which can stall
		// for several seconds when the target drive is asleep (e.g. NAS spin-up).
		// Show a wait cursor so the UI doesn't look frozen during that window.
		QApplication::setOverrideCursor(Qt::WaitCursor);
		m_queue->start();
		QApplication::restoreOverrideCursor();
	}
	updateFooter();
}

void McJobPanel::onCancel()
{
	if (m_queue) m_queue->cancel();
	updateFooter();
}

void McJobPanel::onStartWithGoal()
{
	if (!m_queue) return;

	const int lastGB = AppSettings::instance().value(QStringLiteral("jobPanel/lastGoalGB"), 40).toInt();
	McGbGoalDialog dlg(lastGB, /*minGB=*/1, this);
	if (dlg.exec() != QDialog::Accepted) return;

	AppSettings::instance().setValue(QStringLiteral("jobPanel/lastGoalGB"), dlg.goalGB());

	// Seed the bar immediately — it otherwise only learns the goal from
	// goalProgressChanged, which doesn't fire until the first job finishes,
	// leaving it blank (goalBytes()==0) for the whole first file.
	if (m_goalBar) {
		m_goalBar->setGoal(dlg.goalBytes());
		m_goalBar->setCompleted(0);
		m_goalBar->setLive(0);
		m_goalBar->setVisible(true);
	}

	SubtitleManager::instance().enqueueBatch(m_model->fileIdsByStatus("queued"));

	// Same stall risk as onStartPause() — the disk-space check calls QStorageInfo,
	// which can block for seconds on a sleeping/NAS drive.
	QApplication::setOverrideCursor(Qt::WaitCursor);
	m_queue->startWithGoal(dlg.goalBytes());
	QApplication::restoreOverrideCursor();

	updateFooter();
}

void McJobPanel::onEditGoal()
{
	if (!m_queue || !m_queue->isGoalMode()) return;

	constexpr qint64 kGiB = 1024LL * 1024 * 1024;
	const int minGB = qMax<int>(1, static_cast<int>(
	    std::ceil(double(m_queue->goalSavedBytes()) / double(kGiB))));
	const int curGB = static_cast<int>(std::ceil(double(m_queue->goalBytes()) / double(kGiB)));

	McGbGoalDialog dlg(curGB, minGB, this);
	if (dlg.exec() != QDialog::Accepted) return;

	m_queue->setGoalBytes(dlg.goalBytes());
}

void McJobPanel::onPreviewCommand(qint64 jobId)
{
	const auto jobOpt = DatabaseManager::instance().jobById(jobId);
	if (!jobOpt) return;
	const JobRecord& job = *jobOpt;

	const auto fileOpt = DatabaseManager::instance().fileById(job.fileId);
	const QString filename = fileOpt ? fileOpt->filename : QString();

	QString exePath;
	QStringList args;

	if (job.jobType == QLatin1String("tag_edit")) {
		if (!fileOpt) return;
		exePath = ExternalTools::instance().mkvpropeditPath();
		args = ActionEngine::buildPropEditArgs(fileOpt->path, job.flagChangesJson);
	} else {
		if (job.commandArgsJson.isEmpty()) return;
		exePath = ExternalTools::instance().mkvmergePath();
		const QJsonArray arr = QJsonDocument::fromJson(job.commandArgsJson.toUtf8()).array();
		for (const auto& v : arr) args << v.toString();
		if (!job.flagChangesJson.isEmpty() && !args.isEmpty()) {
			const QList<StreamRecord> streams =
			    DatabaseManager::instance().streamsForFile(job.fileId);
			const QString filteredJson = ActionEngine::filterFlagChangesForRemux(
			    job.flagChangesJson, args, streams);
			if (!filteredJson.isEmpty()) {
				const QStringList flagArgs = ActionEngine::buildFlagArgsForRemux(filteredJson);
				if (!flagArgs.isEmpty()) {
					const QString inputPath = args.takeLast();
					args << flagArgs;
					args << inputPath;
				}
			}
		}
	}
	QStringList quoted;
	for (const QString& a : args)
		quoted << (a.contains(' ') ? QStringLiteral("\"%1\"").arg(a) : a);

	const QString cmdLine = QStringLiteral("\"%1\" %2").arg(exePath, quoted.join(' '));

	// Show in a simple read-only dialog
	auto* dlg = new QDialog(this);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle(tr("Command — %1").arg(filename));
	dlg->resize(780, 180);

	auto* layout = new QVBoxLayout(dlg);
	auto* edit = new QPlainTextEdit(cmdLine, dlg);
	edit->setReadOnly(true);
	edit->setWordWrapMode(QTextOption::WrapAnywhere);
	QFont mono("Courier New", 9);
	mono.setStyleHint(QFont::Monospace);
	edit->setFont(mono);
	layout->addWidget(edit);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
	connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
	layout->addWidget(buttons);

	dlg->show();
}

void McJobPanel::onShowSummary()
{
	QApplication::setOverrideCursor(Qt::WaitCursor);
	auto* dlg = new McBulkSummaryDialog(this);
	QApplication::restoreOverrideCursor();
	dlg->exec();
}

bool McJobPanel::eventFilter(QObject* obj, QEvent* event)
{
	if (obj == m_listView && event->type() == QEvent::KeyPress) {
		const auto* ke = static_cast<QKeyEvent*>(event);
		if (ke->key() == Qt::Key_Delete) {
			onRemoveSelected();
			return true;
		}
	}
	return QWidget::eventFilter(obj, event);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

QString McJobPanel::formatSaved(qint64 bytes)
{
	if (bytes <= 0) return {};
	const double gb = bytes / 1073741824.0;
	if (gb >= 1.0) return QStringLiteral("%1 GB").arg(gb, 0, 'f', 2);
	return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
}

void McJobPanel::updateStatusCombo()
{
	if (!m_statusFilter) return;
	const int total = m_model->totalCount();
	const auto counts = m_model->countsByStatus();

	// Item 1 is "All" — always the total (item 0 is the "Status" header label)
	const QString allLabel = total > 0
	    ? tr("All (%1)").arg(total)
	    : tr("All");

	// Preserve the current selection by data value, not index
	const QString currentData = m_statusFilter->currentData().toString();

	m_statusFilter->blockSignals(true);

	m_statusFilter->setItemText(1, allLabel);

	// Remaining items map to specific statuses (offset +2 to skip header + "All")
	const QList<QPair<QString,QString>> items = {
	    { QStringLiteral("proposed"), tr("Proposed") },
	    { QStringLiteral("queued"),   tr("Queued")   },
	    { QStringLiteral("running"),  tr("Running")  },
	    { QStringLiteral("done"),     tr("Done")     },
	    { QStringLiteral("failed"),   tr("Failed")   },
	};
	const int queuedCount = counts.value(QStringLiteral("queued"), 0);
	for (int i = 0; i < items.size(); ++i) {
		const QString& key   = items[i].first;
		const QString& label = items[i].second;
		const int n = counts.value(key, 0);
		QString text;
		if (n <= 0) {
			text = label;
		} else if (key == QLatin1String("running") && queuedCount > 0) {
			text = QStringLiteral("%1 (%2/%3)").arg(label).arg(n).arg(n + queuedCount);
		} else {
			text = QStringLiteral("%1 (%2)").arg(label).arg(n);
		}
		m_statusFilter->setItemText(i + 2, text);
	}

	// Restore selection — start at 1 to skip the disabled header at index 0,
	// whose QVariant() data also serialises to "" and would shadow the "All" item.
	for (int i = 1; i < m_statusFilter->count(); ++i) {
		if (m_statusFilter->itemData(i).toString() == currentData) {
			m_statusFilter->setCurrentIndex(i);
			break;
		}
	}

	m_statusFilter->blockSignals(false);
}

void McJobPanel::updateFooter()
{
	const auto statusCounts = DatabaseManager::instance().jobStatusCounts();
	const int proposed = statusCounts.value(QStringLiteral("proposed"), 0);
	const int queued   = statusCounts.value(QStringLiteral("queued"),   0);
	const int running  = statusCounts.value(QStringLiteral("running"),  0);
	const int done     = statusCounts.value(QStringLiteral("done"),     0);
	int total = 0;
	for (int n : statusCounts) total += n;

	const qint64 savedTotal = AppSettings::instance().reclaimedBytes();

	// Button state — one source of truth
	m_btnQueueAll->setEnabled(proposed > 0);
	m_btnSummary->setEnabled(proposed > 0);
	m_btnCancel->setEnabled(running > 0);

	if (m_queue && m_queue->isPaused()) {
		m_btnStartPause->setIcon(svgIcon(":/icons/play_arrow.svg"));
		m_btnStartPause->setText(tr("Resume"));
		m_btnStartPause->setToolTip(tr("Resume processing the queue"));
		m_btnStartPause->setEnabled(true);
	} else if (running > 0) {
		m_btnStartPause->setIcon(svgIcon(":/icons/pause.svg"));
		m_btnStartPause->setText(tr("Pause"));
		m_btnStartPause->setToolTip(tr("Pause after the current job completes"));
		m_btnStartPause->setEnabled(true);
	} else {
		m_btnStartPause->setIcon(svgIcon(":/icons/play_arrow.svg"));
		m_btnStartPause->setText(tr("Start"));
		m_btnStartPause->setToolTip(tr(
		"Start processing — runs queued jobs in order, up to one active job per storage group"));
		m_btnStartPause->setEnabled(queued > 0);
	}

	// "Start with GB goal…" only makes sense from a fully idle queue — once running
	// or paused, raising the goal (if any) happens via the aggregate progress bar.
	const bool queueIdle = running == 0 && !(m_queue && m_queue->isPaused());
	m_btnStartGoalMenu->setEnabled(queueIdle && queued > 0);

	if (m_goalBar) m_goalBar->setVisible(m_queue && m_queue->isGoalMode());

	if (total == 0) { m_footerLabel->setText(tr("No jobs")); return; }

	QString text = tr("%1 job(s)").arg(total);
	if (queued > 0)     text += tr(" · %1 queued").arg(queued);
	if (running > 0)    text += tr(" · %1 running").arg(running);
	if (done    > 0)    text += tr(" · %1 done").arg(done);
	if (savedTotal > 0) text += tr(" · %1 reclaimed").arg(formatSaved(savedTotal));

	if (m_queue && m_queue->isGoalMode() && m_goalBar) {
		// formatSaved() returns an empty string for 0 bytes (fine for the "reclaimed"
		// stat above, which is hidden when zero) — but this readout must always show
		// a value, including "0 GB" right after a goal run starts, so format directly
		// in GB rather than reusing it.
		const auto formatGoalGB = [](qint64 bytes) {
			return tr("%1 GB").arg(qMax<qint64>(0, bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
		};
		text += tr(" · Goal: %1 / %2 freed")
		    .arg(formatGoalGB(m_goalBar->currentBytes()), formatGoalGB(m_goalBar->goalBytes()));
	}

	// ETA/speed — per-job EMA; with parallel storage groups the active-batch ETA
	// is the longest running job and throughput is the sum across all runners.
	if (running > 0 && m_model) {
		const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
		double maxActiveEtaSec    = 0.0;
		double aggregateBytesPerMs = 0.0;
		bool   anyRateReady       = false;

		for (int row = 0; row < m_model->rowCount(); ++row) {
			const QModelIndex idx = m_model->index(row, 0);
			if (idx.data(McJobListModel::StatusRole).toString() != QLatin1String("running"))
				continue;

			const qint64  jobId    = idx.data(McJobListModel::JobIdRole).toLongLong();
			const int     progress = idx.data(McJobListModel::ProgressRole).toInt();
			const qint64  fileSize = idx.data(McJobListModel::FileSizeRole).toLongLong();
			const QString phase    = idx.data(McJobListModel::PhaseLabelRole).toString();

			JobEtaState& st = m_etaByJob[jobId];
			if (st.startedAtMs <= 0)
				st.startedAtMs = nowMs;

			if (phase != st.lastPhase) {
				st.lastPhase    = phase;
				st.lastSampleMs = -1;
				st.lastProgress = -1;
				st.emaRatePerMs = -1.0;
			}

			const qint64 elapsedMs = nowMs - st.startedAtMs;

			if (st.lastProgress >= 0 && progress != st.lastProgress
			    && st.lastSampleMs >= 0 && nowMs > st.lastSampleMs) {
				const double dt          = static_cast<double>(nowMs - st.lastSampleMs);
				const double dProgress   = static_cast<double>(progress - st.lastProgress);
				const double instantRate = dProgress / dt;
				if (st.emaRatePerMs < 0.0) {
					st.emaRatePerMs = instantRate;
				} else {
					const double alpha = 1.0 - std::exp(-dt / kEtaSmoothingTauMs);
					st.emaRatePerMs = alpha * instantRate + (1.0 - alpha) * st.emaRatePerMs;
				}
			}
			if (progress != st.lastProgress) {
				st.lastProgress = progress;
				st.lastSampleMs = nowMs;
			}

			if (progress >= 1 && elapsedMs >= 1500) {
				const double ratePerMs = st.emaRatePerMs >= 0.0
				    ? st.emaRatePerMs
				    : static_cast<double>(progress) / static_cast<double>(elapsedMs);
				const double bytesPerMs = fileSize > 0 ? fileSize * ratePerMs / 100.0 : 0.0;

				if (ratePerMs > 0.0) {
					const double fileEtaSec = (100.0 - progress) / ratePerMs / 1000.0;
					maxActiveEtaSec = qMax(maxActiveEtaSec, fileEtaSec);
					aggregateBytesPerMs += bytesPerMs;
					anyRateReady = true;
				}
			}
		}

		for (auto it = m_etaByJob.begin(); it != m_etaByJob.end(); ) {
			const auto jobOpt = DatabaseManager::instance().jobById(it.key());
			if (!jobOpt || jobOpt->status != QLatin1String("running"))
				it = m_etaByJob.erase(it);
			else
				++it;
		}

		if (anyRateReady && maxActiveEtaSec > 0.0) {
			double queueEtaSec = maxActiveEtaSec;
			const bool goalMode = m_queue && m_queue->isGoalMode() && m_goalBar;

			if (aggregateBytesPerMs > 0 && queued > 0) {
				if (goalMode) {
					const qint64 liveBytes = m_goalBar->currentBytes() - m_goalBar->completedBytes();
					const qint64 currentFileRemainingSavings =
					    qMax<qint64>(0, m_queue->currentJobEstimatedSavings() - liveBytes);
					qint64 remainingGoal = m_goalBar->goalBytes() - m_goalBar->currentBytes()
					    - currentFileRemainingSavings;

					qint64 neededBytes = 0;
					if (remainingGoal > 0) {
						auto& db = DatabaseManager::instance();
						const auto candidates = db.queuedJobs(JobSortMode::LargestSavingsFirst);
						for (const JobRecord& job : candidates) {
							if (remainingGoal <= 0) break;
							remainingGoal -= job.estimatedSavedBytes;
							if (const auto fileOpt = db.fileById(job.fileId))
								neededBytes += fileOpt->sizeBytes;
						}
					}
					queueEtaSec += static_cast<double>(neededBytes)
					    / (aggregateBytesPerMs * 1000.0);
				} else {
					qint64 queuedBytes = 0;
					for (int r = 0; r < m_model->rowCount(); ++r) {
						const QModelIndex qi = m_model->index(r, 0);
						if (qi.data(McJobListModel::StatusRole).toString() == QLatin1String("queued"))
							queuedBytes += qi.data(McJobListModel::FileSizeRole).toLongLong();
					}
					queueEtaSec += static_cast<double>(queuedBytes)
					    / (aggregateBytesPerMs * 1000.0);
				}
			}

			if (queued > 0) {
				const QString activeLabel = running > 1
				    ? tr("active jobs") : tr("this file");
				text += tr(" · ~%1 remaining (%2)").arg(formatEtaDuration(maxActiveEtaSec), activeLabel);
				text += goalMode
				    ? tr(" · ~%1 remaining (goal)").arg(formatEtaDuration(queueEtaSec))
				    : tr(" · ~%1 remaining (queue)").arg(formatEtaDuration(queueEtaSec));
			} else {
				text += tr(" · ~%1 remaining").arg(formatEtaDuration(maxActiveEtaSec));
			}

			if (aggregateBytesPerMs > 0) {
				const double mbPerSec = aggregateBytesPerMs * 1000.0 / 1048576.0;
				text += tr(" · %1 MB/s").arg(mbPerSec, 0, 'f', 1);
			}
		}
	}

	m_footerLabel->setText(text);
	updateStatusCombo();
}

} // namespace Mc
