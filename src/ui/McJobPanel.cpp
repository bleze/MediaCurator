#include "ui/McJobPanel.h"
#include "ui/McBulkSummaryDialog.h"
#include "ui/McFilterPanel.h"
#include "ui/McJobCardDelegate.h"
#include "ui/McJobListModel.h"
#include "ui/SvgIcon.h"
#include "engine/JobQueue.h"
#include "engine/PosterManager.h"
#include "core/DatabaseManager.h"
#include "core/ExternalTools.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFrame>
#include <QToolButton>
#include <QSettings>
#include <QTimer>
#include <QStyleOptionComboBox>
#include <QStylePainter>
#include <QCursor>
#include <QKeyEvent>
#include <QItemSelectionModel>
#include <algorithm>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTextOption>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

// ── Status pill helpers ───────────────────────────────────────────────────────
namespace {

constexpr int kPillH = 18;
constexpr int kPadH  = 6;

static QColor pillColorForStatus(const QString& status)
{
	if (status == QLatin1String("proposed")) return { 0x88, 0x88, 0x88 };
	if (status == QLatin1String("queued"))   return { 0xc0, 0x80, 0x00 };
	if (status == QLatin1String("running"))  return { 0x10, 0x6a, 0xc0 };
	if (status == QLatin1String("done"))     return { 0x1a, 0x86, 0x4a };
	if (status == QLatin1String("failed"))   return { 0xcc, 0x22, 0x22 };
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
	m_btnPreviewCmd    = new QPushButton(tr("Preview Command"), toolbar);
	m_btnSummary       = new QPushButton(tr("Summary"),        toolbar);
	m_btnStart         = new QPushButton(tr("Start"),          toolbar);
	m_btnPause         = new QPushButton(tr("Pause"),          toolbar);
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
	m_btnPreviewCmd->setIcon(svgIcon(":/icons/terminal.svg"));
	m_btnPreviewCmd->setIconSize(kIconSz);
	m_btnSummary->setIcon(svgIcon(":/icons/manage_search.svg"));
	m_btnSummary->setIconSize(kIconSz);
	m_btnStart->setIcon(svgIcon(":/icons/play_arrow.svg"));
	m_btnStart->setIconSize(kIconSz);
	m_btnPause->setIcon(svgIcon(":/icons/pause.svg"));
	m_btnPause->setIconSize(kIconSz);
	m_btnCancel->setIcon(svgIcon(":/icons/stop_circle.svg"));
	m_btnCancel->setIconSize(kIconSz);

	m_btnQueueSelected->setEnabled(false);
	m_btnUnqueue->setEnabled(false);
	m_btnQueueAll->setEnabled(false);
	m_btnStart->setEnabled(false);
	m_btnPause->setEnabled(false);
	m_btnCancel->setEnabled(false);
	m_btnRemove->setEnabled(false);
	m_btnPreviewCmd->setEnabled(false);
	m_btnSummary->setEnabled(false);

	m_btnQueueSelected->setToolTip(tr("Move selected proposed jobs to the processing queue"));
	m_btnUnqueue->setToolTip(tr("Move selected queued jobs back to proposed state"));
	m_btnQueueAll->setToolTip(tr("Move all proposed jobs to the processing queue"));
	m_btnStart->setToolTip(tr("Start processing — runs all queued jobs in order"));
	m_btnPause->setToolTip(tr("Pause after the current job completes"));
	m_btnCancel->setToolTip(tr("Stop the current job — queued jobs remain and can be resumed with Start"));
	m_btnRemove->setToolTip(tr("Remove selected jobs from the queue"));
	m_btnPreviewCmd->setToolTip(tr("Show the mkvmerge command for the selected job"));
	m_btnSummary->setToolTip(tr("Show aggregate statistics for all proposed jobs"));

	toolbar->addWidget(m_btnQueueSelected);
	toolbar->addWidget(m_btnQueueAll);
	toolbar->addSeparator();
	toolbar->addWidget(m_btnUnqueue);
	toolbar->addSeparator();
	toolbar->addWidget(m_btnRemove);
	toolbar->addSeparator();
	toolbar->addWidget(m_btnPreviewCmd);
	toolbar->addSeparator();
	toolbar->addWidget(m_btnSummary);

	// Spacer pushes playback controls to the right side of the toolbar
	auto* tbSpacer = new QWidget(toolbar);
	tbSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	toolbar->addWidget(tbSpacer);

	toolbar->addWidget(m_btnStart);
	toolbar->addWidget(m_btnPause);
	toolbar->addWidget(m_btnCancel);

	m_footerLabel = new QLabel(tr("No jobs"), toolbar);
	m_footerLabel->setStyleSheet("color: gray; padding: 0 6px;");
	toolbar->addWidget(m_footerLabel);

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

	m_chkAutoTrack = new QCheckBox(tr("Track running"), this);
	m_chkAutoTrack->setChecked(QSettings().value("jobPanel/followRunning", true).toBool());
	m_chkAutoTrack->setToolTip(tr(
	    "When a job starts: switch the filter to Running (if any specific filter is active) "
	    "or scroll to the running item (if All is active)"));
	connect(m_chkAutoTrack, &QCheckBox::toggled, this, [](bool on) {
		QSettings().setValue("jobPanel/followRunning", on);
	});

	// ── Quick-filter pills (same codec/quality set as the library panel) ──────
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

	const auto addPill = [&](const char* label, const QColor& color, const char* tip, quint32 flag) {
		auto* btn = makePill(QLatin1String(label), color, filterBar);
		btn->setToolTip(QLatin1String(tip));
		connect(btn, &QToolButton::toggled, this, [this, flag](bool on) {
			m_qfFlags = on ? (m_qfFlags | flag) : (m_qfFlags & ~flag);
			m_model->setQuickFilters(m_qfFlags);
		});
		filterLayout->addWidget(btn);
	};

	filterLayout->addWidget(vSep(filterBar));
	addPill("4K",     videoColor, "4K files only (width \xe2\x89\xa5 3840)", QF::QF_4K);
	filterLayout->addWidget(vSep(filterBar));
	addPill("DV",     hdrColor,   "Dolby Vision only",                       QF::QF_DV);
	addPill("HDR",    hdrColor,   "HDR10 / HLG / HDR10+ only",              QF::QF_HDR);
	filterLayout->addWidget(vSep(filterBar));
	addPill("Atmos",  audioColor, "Dolby Atmos only",                        QF::QF_Atmos);
	addPill("TrueHD", audioColor, "Dolby TrueHD only",                      QF::QF_TrueHD);
	addPill("DTS-HD", audioColor, "DTS-HD MA only",                         QF::QF_DtsHD);
	addPill("DTS:X",  audioColor, "DTS:X only",                              QF::QF_DtsX);

	filterLayout->addWidget(vSep(filterBar));
	filterLayout->addWidget(m_chkAutoTrack);
	root->addWidget(filterBar);

	connect(m_btnQueueSelected, &QPushButton::clicked, this, &McJobPanel::onQueueSelected);
	connect(m_btnUnqueue,       &QPushButton::clicked, this, &McJobPanel::onUnqueueSelected);
	connect(m_btnQueueAll,      &QPushButton::clicked, this, &McJobPanel::onQueueAll);
	connect(m_btnRemove,      &QPushButton::clicked, this, &McJobPanel::onRemoveSelected);
	connect(m_btnPreviewCmd,  &QPushButton::clicked, this, &McJobPanel::onPreviewCommand);
	connect(m_btnSummary,     &QPushButton::clicked, this, &McJobPanel::onShowSummary);
	connect(m_btnStart,       &QPushButton::clicked, this, &McJobPanel::onStart);
	connect(m_btnPause,       &QPushButton::clicked, this, &McJobPanel::onPause);
	connect(m_btnCancel,      &QPushButton::clicked, this, &McJobPanel::onCancel);

	// ── List view ─────────────────────────────────────────────────────────────
	m_model = new McJobListModel(this);

	m_listView = new QListView(this);
	m_listView->setModel(m_model);
	auto* jobDelegate = new McJobCardDelegate(m_listView);
	m_listView->setItemDelegate(jobDelegate);
	m_listView->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_listView->setMouseTracking(true);
	m_listView->viewport()->setMouseTracking(true);
	m_listView->setUniformItemSizes(false);
	m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	m_listView->setAlternatingRowColors(true);
	m_listView->setSpacing(0);
	m_listView->setContextMenuPolicy(Qt::CustomContextMenu);
	m_listView->installEventFilter(this);

	connect(jobDelegate, &McJobCardDelegate::playRequested,
	        this, [this](const QModelIndex& idx) {
		const QString path = idx.data(McJobListModel::FilePathRole).toString();
		if (!path.isEmpty()) emit playRequested(path);
	});

	connect(jobDelegate, &McJobCardDelegate::streamToggleRequested,
	        m_model, &McJobListModel::toggleStream);

	connect(jobDelegate, &McJobCardDelegate::imdbPageRequested,
	        this, [](const QModelIndex& idx) {
		const QString id = idx.data(McJobListModel::ImdbIdRole).toString();
		if (!id.isEmpty())
			QDesktopServices::openUrl(
			    QUrl(QStringLiteral("https://www.imdb.com/title/%1/").arg(id)));
	});

	// pressed fires unconditionally — routes clicks to handlePress for badge toggle + play
	connect(m_listView, &QAbstractItemView::pressed,
	        this, [this, jobDelegate](const QModelIndex& idx) {
		if (!idx.isValid()) return;
		const QPoint pos = m_listView->viewport()->mapFromGlobal(QCursor::pos());
		jobDelegate->handlePress(pos, m_listView->visualRect(idx), m_listView->font(), idx);
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
		m_btnPreviewCmd->setEnabled(false);
		updateStatusCombo();
	});

	connect(m_listView->selectionModel(), &QItemSelectionModel::selectionChanged,
	        this, [this]() {
		const auto selected = m_listView->selectionModel()->selectedIndexes();
		const bool has = !selected.isEmpty();
		m_btnRemove->setEnabled(has);
		m_btnPreviewCmd->setEnabled(has);
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
	        this, [this](int i) {
		if (i <= 0) return;   // skip header row
		const QString v = m_statusFilter->itemData(i).toString();
		QSettings().setValue("jobPanel/statusFilter", v);
		m_model->setFilterStatus(v);
	});

	// Restore the previously saved status filter (block signals so the save slot
	// does not fire again, then apply the filter to the model manually).
	{
		const QString saved = QSettings().value("jobPanel/statusFilter").toString();
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
	}

	// Double-click: poster column → IMDb dialog; elsewhere → preview
	connect(m_listView, &QListView::doubleClicked, this, [this](const QModelIndex& idx) {
		if (!idx.isValid()) return;
		const qint64 fileId = idx.data(McJobListModel::FileIdRole).toLongLong();
		const QPoint pos    = m_listView->viewport()->mapFromGlobal(QCursor::pos());
		const QRect  iRect  = m_listView->visualRect(idx);
		if (pos.x() <= iRect.left() + McJobCardDelegate::kPosterW)
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

		QMenu menu(this);

		if (status == QLatin1String("proposed")) {
			auto* queueAct = menu.addAction(svgIcon(":/icons/add_to_queue.svg"),
			                                tr("&Queue File"));
			connect(queueAct, &QAction::triggered, this, [this, jobId] {
				DatabaseManager::instance().promoteJobsToQueued({ jobId });
				m_model->updateJob(jobId, "queued");
				updateFooter();
			});
			menu.addSeparator();
		} else if (status == QLatin1String("queued")) {
			auto* unqueueAct = menu.addAction(svgIcon(":/icons/undo.svg"),
			                                  tr("&Unqueue File"));
			connect(unqueueAct, &QAction::triggered, this, [this, jobId] {
				DatabaseManager::instance().updateJobStatus(jobId, "proposed");
				m_model->updateJob(jobId, "proposed");
				updateFooter();
			});
			menu.addSeparator();
		}

		auto* previewAct = menu.addAction(svgIcon(":/icons/visibility.svg"),
		                                  tr("&Preview Tracks…"));
		connect(previewAct, &QAction::triggered, this, [this, fileId] {
			emit previewRequested(fileId);
		});

		menu.addSeparator();

		auto* refreshPosterAct = menu.addAction(svgIcon(":/icons/refresh.svg"),
		                                        tr("Refresh &Poster"));
		connect(refreshPosterAct, &QAction::triggered, this, [this, fileId] {
			emit refreshPosterRequested(fileId);
		});

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

		const QString imdbLabel = selectedFileIds.size() > 1
		    ? tr("Edit &IMDb Links (%1 files)…").arg(selectedFileIds.size())
		    : tr("Edit &IMDb Link…");
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

	connect(queue, &JobQueue::jobStarted, this, [this](qint64 jobId) {
		onJobStatusChanged(jobId, "running");
		m_jobTimer.start();
		m_etaTimer->start();

		if (!m_chkAutoTrack->isChecked()) return;

		const QString filter = m_statusFilter->currentData().toString();
		if (filter.isEmpty()) {
			for (int row = 0; row < m_model->rowCount(); ++row) {
				const QModelIndex idx = m_model->index(row);
				if (idx.data(McJobListModel::StatusRole).toString() == QLatin1String("running")) {
					m_listView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
					break;
				}
			}
		} else {
			for (int i = 0; i < m_statusFilter->count(); ++i) {
				if (m_statusFilter->itemData(i).toString() == QLatin1String("running")) {
					m_statusFilter->setCurrentIndex(i);
					break;
				}
			}
		}
	});
	connect(queue, &JobQueue::jobFinished, this,
	        [this](qint64 jobId, bool ok, qint64 savedBytes) {
		m_etaTimer->stop();
		m_jobTimer.invalidate();
		m_model->updateJob(jobId, ok ? "done" : "failed", savedBytes);
		updateFooter();
	});
	connect(queue, &JobQueue::allFinished, this, [this] {
		refresh();
		// When auto-track left the filter on "Running" and there's nothing left
		// to run, jump to Proposed so the user can queue more without manual steps.
		if (m_chkAutoTrack->isChecked()
		    && m_statusFilter->currentData().toString() == QLatin1String("running")) {
			const auto counts = m_model->countsByStatus();
			const QString target = counts.value("proposed", 0) > 0
			    ? QStringLiteral("proposed") : QString();
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

	connect(&DatabaseManager::instance(), &DatabaseManager::jobStatusChanged,
	        this, &McJobPanel::onJobStatusChanged);

	connect(&PosterManager::instance(), &PosterManager::posterReady,
	        m_model, &McJobListModel::onPosterReady);
	connect(&PosterManager::instance(), &PosterManager::posterReady,
	        this, &McJobPanel::repaintCards);
	connect(&PosterManager::instance(), &PosterManager::imdbIdSaved,
	        m_model, &McJobListModel::updateImdbId);
}

void McJobPanel::refresh()
{
	m_model->reload();
	updateFooter();
	emit jobsChanged(m_model->rowCount());
}

void McJobPanel::refreshPaged(int limit)
{
	m_model->reloadPaged(limit);
	updateFooter();
	emit jobsChanged(m_model->rowCount());
}

void McJobPanel::scrollToFileJob(qint64 fileId)
{
	// Switch to the "Proposed" filter so the new job is visible.
	for (int i = 0; i < m_statusFilter->count(); ++i) {
		if (m_statusFilter->itemData(i).toString() == QLatin1String("proposed")) {
			m_statusFilter->setCurrentIndex(i);
			break;
		}
	}

	for (int row = 0; row < m_model->rowCount(); ++row) {
		const QModelIndex idx = m_model->index(row);
		if (idx.data(McJobListModel::FileIdRole).toLongLong() == fileId) {
			m_listView->setCurrentIndex(idx);
			m_listView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
			break;
		}
	}
}

void McJobPanel::onJobStatusChanged(qint64 jobId, const QString& status)
{
	m_model->updateJob(jobId, status);
	updateFooter();
}

void McJobPanel::repaintCards()
{
	m_listView->viewport()->repaint();
}

void McJobPanel::onQueueSelected()
{
	// Promote selected proposed jobs to queued
	const auto selected = m_listView->selectionModel()->selectedIndexes();
	QList<qint64> ids;
	QSet<int> seen;
	for (const QModelIndex& idx : selected) {
		if (seen.contains(idx.row())) continue;
		seen.insert(idx.row());
		if (idx.data(McJobListModel::StatusRole).toString() == "proposed")
			ids << idx.data(McJobListModel::JobIdRole).toLongLong();
	}
	if (ids.isEmpty()) return;

	DatabaseManager::instance().promoteJobsToQueued(ids);

	// Update each card in-place (no model reset) so the selection stays intact.
	for (qint64 id : ids)
		m_model->updateJob(id, "queued");
	updateFooter();

	// Disable Queue Selected — there are no more proposed items in the selection.
	m_btnQueueSelected->setEnabled(false);
}

void McJobPanel::onQueueAll()
{
	const QList<qint64> ids = m_model->jobIdsByStatus("proposed");
	if (ids.isEmpty()) return;

	DatabaseManager::instance().promoteJobsToQueued(ids);

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
	for (const QModelIndex& idx : selected) {
		if (seen.contains(idx.row())) continue;
		seen.insert(idx.row());
		const QString status = idx.data(McJobListModel::StatusRole).toString();
		if (status != "running")
			toDelete << idx.data(McJobListModel::JobIdRole).toLongLong();
	}
	auto& db = DatabaseManager::instance();
	for (qint64 id : toDelete) db.deleteJob(id);
	refresh();
}

void McJobPanel::onStart()
{
	if (!m_queue) return;
	m_queue->start();
}

void McJobPanel::onPause()
{
	if (!m_queue) return;
	if (m_queue->isPaused()) {
		m_queue->start();
		m_btnPause->setIcon(svgIcon(":/icons/pause.svg"));
		m_btnPause->setText(tr("Pause"));
	} else {
		m_queue->pause();
		m_btnPause->setIcon(svgIcon(":/icons/play_arrow.svg"));
		m_btnPause->setText(tr("Resume"));
	}
}

void McJobPanel::onCancel()
{
	if (m_queue) m_queue->cancel();
	m_btnPause->setIcon(svgIcon(":/icons/pause.svg"));
	m_btnPause->setText(tr("Pause"));
	refresh();
}

void McJobPanel::onPreviewCommand()
{
	// Use the first selected row, or the first job if nothing is selected
	QModelIndex idx;
	const auto selected = m_listView->selectionModel()->selectedIndexes();
	if (!selected.isEmpty())
		idx = selected.first();
	else if (m_model->rowCount() > 0)
		idx = m_model->index(0);

	if (!idx.isValid()) return;

	const qint64 jobId = idx.data(McJobListModel::JobIdRole).toLongLong();

	// Fetch the full command args from DB
	QString commandArgsJson;
	QString filename;
	const auto jobs = DatabaseManager::instance().allJobsForPanel();
	for (const auto& r : jobs) {
		if (r.jobId == jobId) { filename = r.filename; break; }
	}
	const auto allJobs = DatabaseManager::instance().allJobs();
	for (const auto& j : allJobs) {
		if (j.id == jobId) { commandArgsJson = j.commandArgsJson; break; }
	}

	if (commandArgsJson.isEmpty()) return;

	// Build a human-readable command line string
	const QJsonArray arr = QJsonDocument::fromJson(commandArgsJson.toUtf8()).array();
	QStringList args;
	for (const auto& v : arr) args << v.toString();

	const QString exePath = ExternalTools::instance().mkvmergePath();
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
	auto* dlg = new McBulkSummaryDialog(this);
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
	for (int i = 0; i < items.size(); ++i) {
		const QString& key   = items[i].first;
		const QString& label = items[i].second;
		const int n = counts.value(key, 0);
		const QString text = n > 0 ? QStringLiteral("%1 (%2)").arg(label).arg(n) : label;
		m_statusFilter->setItemText(i + 2, text);
	}

	// Restore selection
	for (int i = 0; i < m_statusFilter->count(); ++i) {
		if (m_statusFilter->itemData(i).toString() == currentData) {
			m_statusFilter->setCurrentIndex(i);
			break;
		}
	}

	m_statusFilter->blockSignals(false);
}

void McJobPanel::updateFooter()
{
	int total = 0, done = 0, queued = 0, running = 0, proposed = 0;
	qint64 savedTotal = 0;

	const auto jobs = DatabaseManager::instance().allJobsForPanel();
	for (const auto& r : jobs) {
		++total;
		if (r.status == "done")     { ++done; savedTotal += r.savedBytes; }
		if (r.status == "queued")   ++queued;
		if (r.status == "running")  ++running;
		if (r.status == "proposed") ++proposed;
	}

	// Button state — one source of truth
	m_btnQueueAll->setEnabled(proposed > 0);
	m_btnSummary->setEnabled(proposed > 0);
	m_btnStart->setEnabled(queued > 0 && running == 0);
	m_btnPause->setEnabled(running > 0 && queued > 0);
	m_btnCancel->setEnabled(running > 0);

	if (total == 0) { m_footerLabel->setText(tr("No jobs")); return; }

	QString text = tr("%1 job(s)").arg(total);
	if (queued > 0)     text += tr(" · %1 queued").arg(queued);
	if (running > 0)    text += tr(" · %1 running").arg(running);
	if (done    > 0)    text += tr(" · %1 done").arg(done);
	if (savedTotal > 0) text += tr(" · %1 reclaimed").arg(formatSaved(savedTotal));

	// ETA — computed from elapsed time and current progress % while a job is running
	if (running > 0 && m_jobTimer.isValid() && m_model) {
		const qint64 elapsedMs = m_jobTimer.elapsed();
		for (int row = 0; row < m_model->rowCount(); ++row) {
			const QModelIndex idx = m_model->index(row, 0);
			if (idx.data(McJobListModel::StatusRole).toString() != QLatin1String("running")) continue;

			const int    progress = idx.data(McJobListModel::ProgressRole).toInt();
			const qint64 fileSize = idx.data(McJobListModel::FileSizeRole).toLongLong();

			if (progress >= 5 && elapsedMs >= 500) {
				const double totalEstMs = static_cast<double>(elapsedMs) * 100.0 / progress;
				double etaSec = (totalEstMs - elapsedMs) / 1000.0;

				// Extend with queued jobs using the current byte rate
				if (fileSize > 0 && queued > 0) {
					const double bytesPerMs = static_cast<double>(fileSize) / totalEstMs;
					qint64 queuedBytes = 0;
					for (int r = 0; r < m_model->rowCount(); ++r) {
						const QModelIndex qi = m_model->index(r, 0);
						if (qi.data(McJobListModel::StatusRole).toString() == QLatin1String("queued"))
							queuedBytes += qi.data(McJobListModel::FileSizeRole).toLongLong();
					}
					if (bytesPerMs > 0)
						etaSec += static_cast<double>(queuedBytes) / (bytesPerMs * 1000.0);
				}

				const int totalSec = qMax(1, static_cast<int>(etaSec));
				const int h = totalSec / 3600;
				const int m = (totalSec % 3600) / 60;
				const int s = totalSec % 60;
				QString etaStr;
				if (h > 0)      etaStr = QStringLiteral("%1h %2m").arg(h).arg(m);
				else if (m > 0) etaStr = QStringLiteral("%1m %2s").arg(m).arg(s);
				else            etaStr = QStringLiteral("%1s").arg(s);
				text += tr(" · ~%1 remaining").arg(etaStr);
			}
			break;
		}
	}

	m_footerLabel->setText(text);
	updateStatusCombo();
}

} // namespace Mc
