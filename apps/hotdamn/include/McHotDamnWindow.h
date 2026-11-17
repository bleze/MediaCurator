#pragma once
#include <QMainWindow>
#include <QString>

class QAction;
class QLabel;

namespace Mc {

class McNfoView;

// Main window of the standalone Hot DAMN NFO Viewer: one NFO per window,
// opened from the command line (file association), File > Open or a drop.
class McHotDamnWindow : public QMainWindow
{
	Q_OBJECT
public:
	explicit McHotDamnWindow(QWidget* parent = nullptr);

	// Reads, decodes and shows the file. On failure tells the user and leaves
	// the current content alone.
	bool openFile(const QString& path);

	// Shown when started without a file.
	void showWelcome();

	// Another instance handed its file over (see main.cpp): show it and come to
	// the front. An empty path just brings the window forward.
	void openFromOtherInstance(const QString& path);

protected:
	void dragEnterEvent(QDragEnterEvent* event) override;
	void dropEvent(QDropEvent* event) override;
	bool eventFilter(QObject* watched, QEvent* event) override;
	void resizeEvent(QResizeEvent* event) override;

private:
	void buildMenus();
	void chooseFile();
	void revealInFolder();
	void showAbout();
	void fit(bool growOnly);
	void setAlwaysOnTop(bool on);
	void buildStatusBar();
	void updateStatus(const QString& modified, const QString& bytes);
	void elideStatusPath();
	void openDefaultAppsSettings();

	McNfoView* m_view       = nullptr;
	QAction*   m_reloadAct  = nullptr;
	QAction*   m_revealAct  = nullptr;
	QLabel*    m_statusPath  = nullptr;
	QLabel*    m_statusDamage = nullptr;
	QLabel*    m_statusDate  = nullptr;
	QLabel*    m_statusBytes = nullptr;
	QLabel*    m_statusGrid  = nullptr;
	QString    m_path;
	bool       m_copyOnSelect = false;
};

} // namespace Mc
