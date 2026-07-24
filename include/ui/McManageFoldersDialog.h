#pragma once

#include <QDialog>
#include <QHash>

class QAction;
class QLabel;
class QPoint;
class QSpinBox;
class QTableWidget;

namespace Mc {

class McManageFoldersDialog : public QDialog
{
	Q_OBJECT
public:
	explicit McManageFoldersDialog(QWidget* parent = nullptr);

	bool anyRemoved() const { return m_anyRemoved; }
	bool anyAdded()   const { return m_anyAdded; }

signals:
	// Emitted as soon as the user confirms adding a folder (before exec() returns).
	// Connect this to McMainWindow::createScanWorker so the existing scan path
	// handles it — no duplicate worker inside the dialog.
	void folderAdded(const QString& path);

private slots:
	void onAddFolder();
	void onRemoveSelected();
	void onSelectionChanged();
	void showContextMenu(const QPoint& pos);

private:
	void loadFolders();
	// Re-evaluates which groups have folders assigned and updates each
	// spin-down row's enabled state/tooltip/icon color accordingly. Must be
	// called whenever a folder's group assignment or the root list changes —
	// the row is only built once in the constructor otherwise.
	void refreshSpinDownEnabled();

	QTableWidget* m_table      = nullptr;
	QAction*      m_actAdd     = nullptr;
	QAction*      m_actRemove  = nullptr;
	bool          m_anyRemoved = false;
	bool          m_anyAdded   = false;

	QHash<int, QLabel*>   m_groupIcons;
	QHash<int, QSpinBox*> m_groupSpins;
};

} // namespace Mc
