#include "ui/McScanCompleteDialog.h"
#include "core/StorageGroupSettings.h"
#include "ui/McCardDelegate.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace Mc {

namespace {
// Renders the same storage-group chip the card badges and McManageFoldersDialog's
// group picker use, so "which drive did this come from" reads identically everywhere.
QPixmap groupChipPixmap(int group, const QFont& baseFont, qreal dpr)
{
	const int w = McCardDelegate::groupChipWidth(group, baseFont);
	const int h = McCardDelegate::kBadgeH;
	QPixmap pix(QSize(w, h) * dpr);
	pix.setDevicePixelRatio(dpr);
	pix.fill(Qt::transparent);
	QPainter p(&pix);
	McCardDelegate::drawGroupChip(&p, 0, 0, h, group, baseFont, dpr);
	return pix;
}
}

McScanCompleteDialog::McScanCompleteDialog(const QStringList& newFiles, int updatedCount,
                                           int removedCount, QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("Scan Complete"));
	setAttribute(Qt::WA_DeleteOnClose);
	setMinimumSize(560, 420);
	resize(720, 560);

	auto* root = new QVBoxLayout(this);

	QStringList summaryParts;
	summaryParts << tr("%n new file(s)", "", newFiles.size());
	if (updatedCount > 0) summaryParts << tr("%n updated", "", updatedCount);
	if (removedCount > 0) summaryParts << tr("%n removed", "", removedCount);
	auto* summary = new QLabel(summaryParts.join(QStringLiteral(", ")), this);
	root->addWidget(summary);

	const bool showGroupColumn = StorageGroupSettings::multipleGroupsInUse();

	m_table = new QTableWidget(this);
	m_table->setColumnCount(showGroupColumn ? 3 : 2);
	QStringList headers{ tr("File"), tr("Folder") };
	if (showGroupColumn) headers << tr("Storage Group");
	m_table->setHorizontalHeaderLabels(headers);
	m_table->horizontalHeader()->setStretchLastSection(false);
	m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	if (showGroupColumn)
		m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	m_table->verticalHeader()->setVisible(false);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::NoSelection);
	m_table->setSortingEnabled(false);
	root->addWidget(m_table, 1);

	QStringList sorted = newFiles;
	std::sort(sorted.begin(), sorted.end(), [](const QString& a, const QString& b) {
		return QFileInfo(a).fileName().compare(QFileInfo(b).fileName(), Qt::CaseInsensitive) < 0;
	});

	m_table->setRowCount(sorted.size());
	for (int row = 0; row < sorted.size(); ++row) {
		const QFileInfo fi(sorted[row]);
		auto* nameItem   = new QTableWidgetItem(fi.fileName());
		auto* folderItem = new QTableWidgetItem(fi.path());
		nameItem->setToolTip(sorted[row]);
		folderItem->setToolTip(sorted[row]);
		m_table->setItem(row, 0, nameItem);
		m_table->setItem(row, 1, folderItem);

		if (showGroupColumn) {
			const int group = StorageGroupSettings::groupForFilePath(sorted[row]);
			auto* chip = new QLabel(m_table);
			chip->setPixmap(groupChipPixmap(group, m_table->font(), devicePixelRatioF()));
			chip->setAlignment(Qt::AlignCenter);
			chip->setToolTip(tr("Group %1").arg(group));
			m_table->setCellWidget(row, 2, chip);
		}
	}

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);
}

} // namespace Mc
