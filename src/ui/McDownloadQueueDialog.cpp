#include "ui/McDownloadQueueDialog.h"
#include "core/DatabaseManager.h"
#include "scanner/EditionDetector.h"
#include "ui/McCardDelegate.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QPainter>
#include <QProgressBar>
#include <QRegularExpression>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>

namespace Mc {

namespace {
QString formatMb(qint64 mb)
{
	const double gb = mb / 1024.0;
	return gb >= 1.0
	    ? QObject::tr("%1 GB").arg(gb, 0, 'f', 2)
	    : QObject::tr("%1 MB").arg(static_cast<double>(mb), 0, 'f', 0);
}

// Same categorization as McDownloadQueueBand — kept in sync by eye since
// each is a two-branch helper, not worth sharing across a header for.
QString statusIcon(const QString& status)
{
	if (status.contains(QStringLiteral("PAUSED"), Qt::CaseInsensitive))
		return QStringLiteral("⏸");
	if (status.compare(QStringLiteral("DOWNLOADING"), Qt::CaseInsensitive) == 0)
		return QStringLiteral("\U0001F4E5");
	return QStringLiteral("⏳");   // queued, post-processing, etc.
}

bool isActivelyDownloading(const QString& status)
{
	return status.compare(QStringLiteral("DOWNLOADING"), Qt::CaseInsensitive) == 0;
}

// No stream metadata exists pre-scan (the file hasn't been ffprobed yet), so
// unlike the library cards' hasVideo4K() — which trusts real stream
// resolution — this is a filename guess, same confidence level as
// EditionDetector's heuristics.
bool guessIs4K(const QString& name)
{
	static const QRegularExpression re(
	    R"(\b(2160p|4k|uhd)\b)", QRegularExpression::CaseInsensitiveOption);
	return re.match(name).hasMatch();
}

// Paints the same 4K/3D/Edition badges the library cards use (via
// McCardDelegate's shared static drawing functions) in front of the release
// name, so the queue reads consistently with the library it's about to
// join. Data comes from Qt::UserRole+1 (bool is4K) / +2 (QString edition),
// set per-row in McDownloadQueueDialog::rebuildTable().
class NameBadgeDelegate : public QStyledItemDelegate
{
public:
	explicit NameBadgeDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

	void paint(QPainter* painter, const QStyleOptionViewItem& option,
	           const QModelIndex& index) const override
	{
		// Deliberately not calling QStyledItemDelegate::paint() — its
		// initStyleOption() re-derives the text from the model regardless of
		// what's passed in, so suppressing it that way just draws the name
		// twice, offset. This table has no selection/hover state (NoSelection
		// mode) to preserve, so painting everything ourselves is simplest.
		const QString name = index.data(Qt::DisplayRole).toString();

		painter->save();
		const QRect r      = option.rect.adjusted(4, 0, -4, 0);
		const int   badgeH = qMin(20, r.height() - 4);
		const int   badgeY = r.top() + (r.height() - badgeH) / 2;
		int         x      = r.left();

		QFont badgeFont = option.font;
		badgeFont.setPointSizeF(option.font.pointSizeF() * 0.82);

		if (index.data(Qt::UserRole + 1).toBool()) {
			x += McCardDelegate::drawBadge(painter, x, badgeY, badgeH, QStringLiteral("4K"),
			         McCardDelegate::badgeColor(QStringLiteral("video")), badgeFont) + 4;
		}
		const QString edition = index.data(Qt::UserRole + 2).toString();
		if (!edition.isEmpty())
			x += McCardDelegate::drawEditionBadges(painter, x, badgeY, badgeH, edition, badgeFont);

		const QRect textRect(x, r.top(), qMax(0, r.right() - x), r.height());
		painter->setPen(option.palette.color(QPalette::Text));
		painter->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
		    option.fontMetrics.elidedText(name, Qt::ElideRight, textRect.width()));
		painter->restore();
	}
};
}

McDownloadQueueDialog::McDownloadQueueDialog(const QList<DownloadQueueItem>& items,
                                              const QStringList& editionTokens, QWidget* parent)
	: QDialog(parent)
	, m_editionTokens(editionTokens)
{
	setWindowTitle(tr("Download Queue"));
	resize(820, 560);

	auto* root = new QVBoxLayout(this);

	m_table = new QTableWidget(this);
	m_table->setColumnCount(5);
	m_table->setHorizontalHeaderLabels(
	    { tr("Provider"), tr("Name"), tr("Progress"), tr("Size"), tr("Status") });
	m_table->horizontalHeader()->setStretchLastSection(false);
	m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
	m_table->verticalHeader()->setVisible(false);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setSelectionMode(QAbstractItemView::NoSelection);
	// Read-only display, nothing to focus — also avoids NameBadgeDelegate's
	// custom paint() (which doesn't draw one) leaving a stray focus rect from
	// the default delegate on the other columns.
	m_table->setFocusPolicy(Qt::NoFocus);
	m_table->setItemDelegateForColumn(1, new NameBadgeDelegate(m_table));
	root->addWidget(m_table, 1);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);

	rebuildTable(items);
}

void McDownloadQueueDialog::setItems(const QList<DownloadQueueItem>& items)
{
	rebuildTable(items);
}

void McDownloadQueueDialog::rebuildTable(const QList<DownloadQueueItem>& items)
{
	m_table->setRowCount(items.size());

	for (int row = 0; row < items.size(); ++row) {
		const DownloadQueueItem& item = items[row];
		const int percent = item.totalMb > 0
		    ? qBound(0, static_cast<int>((item.totalMb - item.remainingMb) * 100 / item.totalMb), 100)
		    : 0;

		FileRecord fakeRecord;
		fakeRecord.filename = item.name;

		auto* providerItem = new QTableWidgetItem(item.providerId);
		auto* nameItem     = new QTableWidgetItem(item.name);
		nameItem->setData(Qt::UserRole + 1, guessIs4K(item.name));
		nameItem->setData(Qt::UserRole + 2, EditionDetector::detect(fakeRecord, m_editionTokens));
		auto* sizeItem     = new QTableWidgetItem(formatMb(item.totalMb));
		auto* statusItem   = new QTableWidgetItem(statusIcon(item.status) + QStringLiteral(" ") + item.status);
		sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

		m_table->setItem(row, 0, providerItem);
		m_table->setItem(row, 1, nameItem);
		m_table->setItem(row, 3, sizeItem);
		m_table->setItem(row, 4, statusItem);

		// A motionless 0% bar reads as "stuck," not "idle" — only show an
		// actual progress bar while the item is being fetched; otherwise
		// just state the percentage (or a dash if nothing's downloaded yet).
		if (isActivelyDownloading(item.status)) {
			m_table->removeCellWidget(row, 2);
			auto* progressBar = new QProgressBar(m_table);
			progressBar->setRange(0, 100);
			progressBar->setValue(percent);
			m_table->setCellWidget(row, 2, progressBar);
		} else {
			m_table->removeCellWidget(row, 2);
			auto* progressItem = new QTableWidgetItem(percent > 0 ? tr("%1%").arg(percent) : QStringLiteral("—"));
			progressItem->setTextAlignment(Qt::AlignCenter);
			m_table->setItem(row, 2, progressItem);
		}
	}
}

} // namespace Mc
