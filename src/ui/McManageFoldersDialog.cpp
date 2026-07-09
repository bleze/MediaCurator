#include "ui/McManageFoldersDialog.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"
#include "core/StorageGroupSettings.h"
#include "ui/SvgIcon.h"

#include <algorithm>

#include <QAction>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QStyleOptionHeader>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolBar>
#include <QVBoxLayout>

namespace Mc {

namespace {
class NoFocusDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;
	void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
	{
		QStyleOptionViewItem o = opt;
		o.state &= ~QStyle::State_HasFocus;
		QStyledItemDelegate::paint(p, o, idx);
	}
};

// QHeaderView bolds section labels when the table has focus (Windows/modern style).
class StableHeaderView : public QHeaderView {
public:
	explicit StableHeaderView(Qt::Orientation orientation, QWidget* parent = nullptr)
	    : QHeaderView(orientation, parent)
	{
		QFont f = font();
		f.setBold(false);
		setFont(f);
	}

protected:
	void paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const override
	{
		QStyleOptionHeader option;
		initStyleOption(&option);
		option.rect          = rect;
		option.section       = logicalIndex;
		option.text          = model()->headerData(logicalIndex, orientation(),
		                                           Qt::DisplayRole).toString();
		option.textAlignment = defaultAlignment();
		option.state &= ~(QStyle::State_Active | QStyle::State_HasFocus);

		if (isSortIndicatorShown() && logicalIndex == sortIndicatorSection()) {
			option.sortIndicator = (sortIndicatorOrder() == Qt::AscendingOrder)
			    ? QStyleOptionHeader::SortDown
			    : QStyleOptionHeader::SortUp;
		}

		style()->drawControl(QStyle::CE_Header, &option, painter, this);
	}
};
} // namespace

McManageFoldersDialog::McManageFoldersDialog(QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("Manage Library Folders"));
	setMinimumSize(700, 350);
	resize(850, 460);

	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 10);
	root->setSpacing(0);

	// ── Toolbar ───────────────────────────────────────────────────────────────
	auto* tb = new QToolBar(this);
	tb->setIconSize({24, 24});
	tb->setMovable(false);
	tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

	m_actAdd = new QAction(svgIcon(":/icons/folder_open.svg"), tr("Add Folder…"), this);
	m_actAdd->setToolTip(tr("Add a new folder to the library"));
	connect(m_actAdd, &QAction::triggered, this, &McManageFoldersDialog::onAddFolder);
	tb->addAction(m_actAdd);

	tb->addSeparator();

	m_actRemove = new QAction(svgIcon(":/icons/delete.svg"), tr("Remove"), this);
	m_actRemove->setToolTip(tr("Remove selected folders and all their files from the library database"));
	m_actRemove->setEnabled(false);
	connect(m_actRemove, &QAction::triggered, this, &McManageFoldersDialog::onRemoveSelected);
	tb->addAction(m_actRemove);

	root->addWidget(tb);

	// ── Table ─────────────────────────────────────────────────────────────────
	m_table = new QTableWidget(this);
	m_table->setColumnCount(3);
	m_table->setHorizontalHeader(new StableHeaderView(Qt::Horizontal, m_table));
	m_table->setHorizontalHeaderLabels({tr("Folder"), tr("Media Files"), tr("Storage Group")});
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setContextMenuPolicy(Qt::CustomContextMenu);
	m_table->setShowGrid(false);
	m_table->setAlternatingRowColors(true);
	m_table->verticalHeader()->setVisible(false);
	m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setMinimumSectionSize(100);
	m_table->setItemDelegate(new NoFocusDelegate(m_table));

	connect(m_table, &QTableWidget::customContextMenuRequested,
	        this, &McManageFoldersDialog::showContextMenu);
	connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged,
	        this, &McManageFoldersDialog::onSelectionChanged);

	root->addWidget(m_table, 1);

	// ── Disclaimer ────────────────────────────────────────────────────────────
	auto* note = new QLabel(
		tr("Removing a folder only removes its entries from the MediaCurator library database."
		   " No files on disk are deleted.\n"
		   "Storage groups group folders that share the same drive or NAS volume."
		   " Different groups can scan and remux in parallel; folders in the same group"
		   " are processed one at a time."), this);
	note->setWordWrap(true);
	note->setContentsMargins(8, 4, 8, 4);
	QPalette pal = note->palette();
	pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
	note->setPalette(pal);
	root->addWidget(note);

	// ── Close button ──────────────────────────────────────────────────────────
	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	buttons->setContentsMargins(10, 6, 10, 0);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);

	loadFolders();
}

void McManageFoldersDialog::loadFolders()
{
	const QStringList roots = AppSettings::instance().value("scan/roots").toStringList();

	m_table->setRowCount(roots.size());

	for (int i = 0; i < roots.size(); ++i) {
		const QString path  = StorageGroupSettings::normalizedRoot(roots[i]);
		const int     count = DatabaseManager::instance().fileCountUnderPath(path);

		auto* pathItem = new QTableWidgetItem(path);
		pathItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);

		auto* countItem = new QTableWidgetItem(QLocale().toString(count));
		countItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
		countItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

		m_table->setItem(i, 0, pathItem);
		m_table->setItem(i, 1, countItem);

		auto* groupCombo = new QComboBox(m_table);
		groupCombo->setToolTip(tr(
			"Assign folders on the same drive or NAS to the same storage group."
			" Different groups can scan and remux in parallel."));
		for (int g = StorageGroupSettings::MinGroup; g <= StorageGroupSettings::uiMaxGroup(); ++g)
			groupCombo->addItem(tr("Group %1").arg(g), g);
		const int idx = groupCombo->findData(StorageGroupSettings::groupForRoot(path));
		groupCombo->blockSignals(true);
		groupCombo->setCurrentIndex(idx >= 0 ? idx : 0);
		groupCombo->blockSignals(false);
		connect(groupCombo, &QComboBox::currentIndexChanged, this,
		        [path, groupCombo](int) {
			StorageGroupSettings::setGroupForRoot(path, groupCombo->currentData().toInt());
		});
		m_table->setCellWidget(i, 2, groupCombo);
	}
}

void McManageFoldersDialog::onAddFolder()
{
	QStringList roots = AppSettings::instance().value("scan/roots").toStringList();
	const QString hint = roots.isEmpty() ? QString() : roots.last();

	const QString raw    = QFileDialog::getExistingDirectory(
		this, tr("Add Media Folder"), hint,
		QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
	if (raw.isEmpty()) return;
	const QString folder = StorageGroupSettings::normalizedRoot(raw);

	if (roots.contains(folder)) {
		QMessageBox::information(this, tr("Already Added"),
			tr("\"%1\" is already in the library.").arg(folder));
		return;
	}

	roots << folder;
	AppSettings::instance().setValue("scan/roots", roots);
	m_anyAdded = true;

	// Add the row immediately — count starts at 0 and rises as the scan
	// (running in McMainWindow) writes files to the database.
	const int row = m_table->rowCount();
	m_table->insertRow(row);

	auto* pathItem = new QTableWidgetItem(folder);
	pathItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
	m_table->setItem(row, 0, pathItem);

	auto* countItem = new QTableWidgetItem(QStringLiteral("0"));
	countItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
	countItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
	m_table->setItem(row, 1, countItem);

	auto* groupCombo = new QComboBox(m_table);
	groupCombo->setToolTip(tr(
		"Assign folders on the same drive or NAS to the same storage group."
		" Different groups can scan and remux in parallel."));
	for (int g = StorageGroupSettings::MinGroup; g <= StorageGroupSettings::uiMaxGroup(); ++g)
		groupCombo->addItem(tr("Group %1").arg(g), g);
	groupCombo->blockSignals(true);
	groupCombo->setCurrentIndex(0);
	groupCombo->blockSignals(false);
	StorageGroupSettings::setGroupForRoot(folder, groupCombo->currentData().toInt());
	connect(groupCombo, &QComboBox::currentIndexChanged, this,
	        [folder, groupCombo](int) {
		StorageGroupSettings::setGroupForRoot(folder, groupCombo->currentData().toInt());
	});
	m_table->setCellWidget(row, 2, groupCombo);

	// Let McMainWindow's existing scan infrastructure handle this —
	// no duplicate worker here.
	emit folderAdded(folder);
}

void McManageFoldersDialog::onSelectionChanged()
{
	m_actRemove->setEnabled(!m_table->selectionModel()->selectedRows().isEmpty());
}

void McManageFoldersDialog::onRemoveSelected()
{
	const QList<QModelIndex> selected = m_table->selectionModel()->selectedRows(0);
	if (selected.isEmpty()) return;

	QStringList folders;
	for (const QModelIndex& idx : selected)
		folders << m_table->item(idx.row(), 0)->text();

	QString msg;
	if (folders.size() == 1) {
		msg = tr("Remove \"%1\" and all its files from the library?\n\n"
		         "This only removes database entries — no files on disk will be deleted.")
		          .arg(folders.first());
	} else {
		QStringList lines;
		for (const QString& f : std::as_const(folders))
			lines << QStringLiteral("  • ") + f;
		msg = tr("Remove %1 folders and all their files from the library?\n\n%2\n\n"
		         "This only removes database entries — no files on disk will be deleted.")
		          .arg(folders.size())
		          .arg(lines.join('\n'));
	}

	if (QMessageBox::question(this, tr("Remove Folders"), msg,
	        QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
	    != QMessageBox::Yes)
		return;

	QStringList roots = AppSettings::instance().value("scan/roots").toStringList();

	for (const QString& folder : std::as_const(folders)) {
		const QString norm = StorageGroupSettings::normalizedRoot(folder);
		DatabaseManager::instance().removeFilesUnderPath(norm);
		StorageGroupSettings::removeRoot(norm);
		roots.removeAll(norm);
	}

	AppSettings::instance().setValue("scan/roots", roots);
	m_anyRemoved = true;

	QList<int> rows;
	rows.reserve(selected.size());
	for (const QModelIndex& idx : selected)
		rows << idx.row();
	std::sort(rows.rbegin(), rows.rend());
	for (int row : std::as_const(rows))
		m_table->removeRow(row);
}

void McManageFoldersDialog::showContextMenu(const QPoint& pos)
{
	if (m_table->selectionModel()->selectedRows().isEmpty()) return;
	QMenu menu(this);
	menu.addAction(m_actRemove);
	menu.exec(m_table->viewport()->mapToGlobal(pos));
}

} // namespace Mc
