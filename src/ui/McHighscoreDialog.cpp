#include "ui/McHighscoreDialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFont>
#include <QHeaderView>
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

	m_table = new QTableWidget(this);
	m_table->setColumnCount(4);
	m_table->setHorizontalHeaderLabels({ tr("#"), tr("Name"), tr("Reclaimed"), tr("Last Active") });
	m_table->horizontalHeader()->setStretchLastSection(false);
	m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
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
	rebuildTable(m_entries);
}

void McHighscoreDialog::setEntries(const QList<HighscoreEntry>& entries)
{
	m_entries = entries;
	rebuildTable(m_entries);
}

void McHighscoreDialog::setLocalPlayerName(const QString& name)
{
	m_localPlayerName = name;
	updateJoinButtonVisibility();
	rebuildTable(m_entries);
}

void McHighscoreDialog::updateJoinButtonVisibility()
{
	m_joinButton->setVisible(m_localPlayerName.isEmpty());
}

void McHighscoreDialog::rebuildTable(const QList<HighscoreEntry>& entries)
{
	const QList<HighscoreEntry> shown = entries.mid(0, kMaxRows);
	m_table->setRowCount(shown.size());

	for (int row = 0; row < shown.size(); ++row) {
		const HighscoreEntry& e = shown[row];
		auto* rankItem       = new QTableWidgetItem(QString::number(row + 1));
		auto* nameItem       = new QTableWidgetItem(e.name);
		auto* scoreItem      = new QTableWidgetItem(formatReclaimed(e.score));
		auto* lastActiveItem = new QTableWidgetItem(formatLastActive(e.lastActive));
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
