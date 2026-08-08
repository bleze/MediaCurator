#pragma once
#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>

#include "engine/HighscoreClient.h"

class QTableWidget;
class QPushButton;

namespace Mc {

// McHighscoreDialog — full leaderboard view (top ~50 entries), opened by
// clicking the McHighscoreBand. Highlights the row matching the local
// player's stored name. Shows a "Join Leaderboard" button in place of the
// highlight when the local player has no stored name (never joined, or
// previously declined) so opting back in doesn't require re-triggering the
// nag prompt.
class McHighscoreDialog : public QDialog
{
	Q_OBJECT
public:
	explicit McHighscoreDialog(const QList<HighscoreEntry>& entries,
	                            const QString& localPlayerName,
	                            QWidget* parent = nullptr);

	void setEntries(const QList<HighscoreEntry>& entries);   // live-refresh while open
	void setLocalPlayerName(const QString& name);             // after joining from within the dialog

signals:
	void refreshRequested();
	void joinRequested();

private:
	void refreshTable();   // re-sorts m_entries per current header sort and rebuilds the table
	void rebuildTable(const QList<QPair<int, HighscoreEntry>>& rankedEntries);
	void updateJoinButtonVisibility();
	void handleHeaderClicked(int column);
	QList<QPair<int, HighscoreEntry>> sortedRankedEntries() const;   // rank reflects natural (score) order, independent of display sort

	QTableWidget* m_table      = nullptr;
	QPushButton*  m_joinButton = nullptr;
	QString       m_localPlayerName;
	QList<HighscoreEntry> m_entries;
	int           m_sortColumn = 0;   // defaults to Rank ascending (natural server order)
	Qt::SortOrder m_sortOrder  = Qt::AscendingOrder;
};

} // namespace Mc
