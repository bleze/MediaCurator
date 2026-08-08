#include "ui/McHighscoreDialog.h"

#include <algorithm>

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFont>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace Mc {

namespace {
constexpr int kMaxRows = 50;

// Mirrors McMainWindow::updateSavedLabel()'s status-bar formatting so the
// leaderboard reads in the same units as the rest of the app.
QString formatReclaimed(qint64 mb)
{
	const double gb = mb / 1024.0;
	return gb >= 1.0
	    ? QObject::tr("%1 GB").arg(gb, 0, 'f', 2)
	    : QObject::tr("%1 MB").arg(static_cast<double>(mb), 0, 'f', 1);
}

// dreamlo's "date" is last-submit time, not first-joined — label the column
// accordingly rather than implying tenure.
QString formatLastActive(const QDateTime& dt)
{
	return dt.isValid() ? dt.toString(QStringLiteral("MMM d, yyyy")) : QObject::tr("—");
}
}

McHighscoreDialog::McHighscoreDialog(const QList<HighscoreEntry>& entries,
                                      const QString& localPlayerName,
                                      QWidget* parent)
	: QDialog(parent)
	, m_localPlayerName(localPlayerName)
{
	setWindowTitle(tr("MediaCurator Leaderboard"));
	resize(480, 480);

	auto* root = new QVBoxLayout(this);

	auto* hint = new QLabel(
	    tr("Reclaimed reflects space saved by removing tracks via mkvmerge only — manually "
	       "deleted files (e.g. duplicates) are never counted here, regardless of your local "
	       "Settings preference."), this);
	hint->setWordWrap(true);
	root->addWidget(hint);

	m_table = new QTableWidget(this);
	m_table->setColumnCount(4);
	m_table->setHorizontalHeaderLabels({ tr("#"), tr("Name"), tr("Reclaimed"), tr("Last Active") });
	m_table->horizontalHeader()->setStretchLastSection(false);
	m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionsClickable(true);
	m_table->horizontalHeader()->setSortIndicatorShown(true);
	m_table->horizontalHeader()->setSortIndicator(m_sortColumn, m_sortOrder);
	connect(m_table->horizontalHeader(), &QHeaderView::sectionClicked,
	        this, &McHighscoreDialog::handleHeaderClicked);
	m_table->verticalHeader()->setVisible(false);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setSelectionMode(QAbstractItemView::NoSelection);
	root->addWidget(m_table, 1);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	m_joinButton = buttons->addButton(tr("Join Leaderboard"), QDialogButtonBox::ActionRole);
	connect(m_joinButton, &QPushButton::clicked, this, &McHighscoreDialog::joinRequested);
	auto* refreshBtn = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
	connect(refreshBtn, &QPushButton::clicked, this, &McHighscoreDialog::refreshRequested);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);

	updateJoinButtonVisibility();
	m_entries = entries;
	refreshTable();
}

void McHighscoreDialog::setEntries(const QList<HighscoreEntry>& entries)
{
	m_entries = entries;
	refreshTable();
}

void McHighscoreDialog::setLocalPlayerName(const QString& name)
{
	m_localPlayerName = name;
	updateJoinButtonVisibility();
	refreshTable();
}

void McHighscoreDialog::updateJoinButtonVisibility()
{
	m_joinButton->setVisible(m_localPlayerName.isEmpty());
}

void McHighscoreDialog::handleHeaderClicked(int column)
{
	if (column == m_sortColumn) {
		m_sortOrder = m_sortOrder == Qt::AscendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder;
	} else {
		m_sortColumn = column;
		// Reclaimed/Last Active read naturally biggest-first; Rank/Name read naturally A-first.
		m_sortOrder = (column == 2 || column == 3) ? Qt::DescendingOrder : Qt::AscendingOrder;
	}
	m_table->horizontalHeader()->setSortIndicatorShown(true);
	m_table->horizontalHeader()->setSortIndicator(m_sortColumn, m_sortOrder);
	refreshTable();
}

void McHighscoreDialog::refreshTable()
{
	rebuildTable(sortedRankedEntries());
}

QList<QPair<int, HighscoreEntry>> McHighscoreDialog::sortedRankedEntries() const
{
	const QList<HighscoreEntry> shown = m_entries.mid(0, kMaxRows);

	QList<QPair<int, HighscoreEntry>> ranked;
	ranked.reserve(shown.size());
	for (int i = 0; i < shown.size(); ++i)
		ranked.append({ i + 1, shown[i] });   // rank always reflects natural (score) order

	const int column = m_sortColumn;
	auto lessThan = [column](const QPair<int, HighscoreEntry>& a, const QPair<int, HighscoreEntry>& b) {
		switch (column) {
		case 0:  return a.first < b.first;
		case 1:  return a.second.name.compare(b.second.name, Qt::CaseInsensitive) < 0;
		case 2:  return a.second.score < b.second.score;
		case 3:  return a.second.lastActive < b.second.lastActive;
		default: return false;
		}
	};
	const bool ascending = m_sortOrder == Qt::AscendingOrder;
	std::stable_sort(ranked.begin(), ranked.end(),
	    [ascending, &lessThan](const QPair<int, HighscoreEntry>& a, const QPair<int, HighscoreEntry>& b) {
		    return ascending ? lessThan(a, b) : lessThan(b, a);
	    });
	return ranked;
}

void McHighscoreDialog::rebuildTable(const QList<QPair<int, HighscoreEntry>>& rankedEntries)
{
	m_table->setRowCount(rankedEntries.size());

	for (int row = 0; row < rankedEntries.size(); ++row) {
		const int rank              = rankedEntries[row].first;
		const HighscoreEntry& e     = rankedEntries[row].second;
		const QString rankText = rank == 1 ? QStringLiteral("🏆 1") : QString::number(rank);
		auto* rankItem       = new QTableWidgetItem(rankText);
		auto* nameItem       = new QTableWidgetItem(e.name);
		auto* scoreItem      = new QTableWidgetItem(formatReclaimed(e.score));
		auto* lastActiveItem = new QTableWidgetItem(formatLastActive(e.lastActive));
		rankItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
		scoreItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
		lastActiveItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

		if (e.name.compare(m_localPlayerName, Qt::CaseInsensitive) == 0) {
			QFont bold = m_table->font();
			bold.setBold(true);
			rankItem->setFont(bold);
			nameItem->setFont(bold);
			scoreItem->setFont(bold);
			lastActiveItem->setFont(bold);
		}

		m_table->setItem(row, 0, rankItem);
		m_table->setItem(row, 1, nameItem);
		m_table->setItem(row, 2, scoreItem);
		m_table->setItem(row, 3, lastActiveItem);
	}
}

} // namespace Mc
