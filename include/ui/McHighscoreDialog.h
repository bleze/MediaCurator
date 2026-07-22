#pragma once
#include <QDialog>
#include <QList>
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
	void rebuildTable(const QList<HighscoreEntry>& entries);
	void updateJoinButtonVisibility();

	QTableWidget* m_table      = nullptr;
	QPushButton*  m_joinButton = nullptr;
	QString       m_localPlayerName;
	QList<HighscoreEntry> m_entries;
};

} // namespace Mc
