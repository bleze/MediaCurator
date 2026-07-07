#pragma once

#include <QLabel>
#include <QListView>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_WIN
struct ITaskbarList3;
#endif

namespace Mc {

class AnalyzeWorker;
class JobQueue;
class LibraryLoader;
class McFileListModel;
class McFilterPanel;
class McJobPanel;
class McWhatIfDialog;
class SimulateWorker;
class ScanWorker;
class UserProfile;
struct FileRecord;
struct StreamRecord;

class McMainWindow : public QMainWindow
{
	Q_OBJECT
public:
	explicit McMainWindow(QWidget* parent = nullptr);
	~McMainWindow() override;

protected:
	void closeEvent(QCloseEvent* event) override;
	void showEvent(QShowEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;

private slots:
	void onScanFolder();
	void onScanLibrary();
	void onQuickScan();
	void onRemoveFolder();
	void onScanProgress(int current, int total, const QString& currentFile);
	void onScanFinished(int scanned, int added, int updated, int failed, int skipped, int removed, QStringList newFiles);
	void onRefreshView();
	void onAnalyzeLibrary();
	void onAnalyzeProgress(int current, int total, const QString& filename);
	void onAnalyzeJobProposed(qint64 fileId);
	void onAnalyzeFinished(int analyzed, int created);
	void onAbout();
	void onDonate();
	void onSettings();
	void onShowPreview(qint64 fileId);
	void onSimulate();
	void onSimulateFinished(int analyzed, int filesAffected);
	void onCheckForUpdates();
	void onUpdateAvailable(QString version, QString htmlUrl, QString releaseNotes, bool silent);
	void onUpdateUpToDate(bool silent);
	void onUpdateCheckFailed(QString error, bool silent);

private:
	void setupUi();
	void setupActions();
	void setupToolBar();
	void setupMenuBar();
	void setupStatusBar();
	void startLibraryLoader();
	void createScanWorker(const QString& folderPath, bool quickScan = false);
	void setScanningState(bool scanning);
	void updateSavedLabel();
	void updateActionStates();         // enable/disable scan-lib and analyze based on roots + file count
	void updateJobPanelVisibility(bool forceShow = false);   // show/hide job panel based on whether any jobs exist
	void launchInVlc(const QString& rawPath);
	bool analyzeSingleFile(qint64 fileId);
	void setSubtitleLanguage(const FileRecord& file, const StreamRecord& stream, const QString& langCode);
#ifdef Q_OS_WIN
	void setTaskbarProgress(int value, int total = 100);
	void clearTaskbarProgress();
#endif

	// Actions (created once, shared between toolbar and menu)
	QAction*     m_actScanFolder    = nullptr;
	QAction*     m_actScanLibrary   = nullptr;
	QAction*     m_actQuickScan     = nullptr;
	QAction*     m_actRemoveFolder  = nullptr;
	QAction*     m_actAnalyze       = nullptr;
	QAction*     m_actSimulate      = nullptr;
	QAction*     m_actSettings      = nullptr;
	QAction*     m_actRefresh       = nullptr;
	QAction*  m_actToggleQueue   = nullptr;
	QWidget*  m_menuQueueBtn     = nullptr;  // view-menu McQueueToggle for the queue toggle
	QAction*  m_actCheckUpdates  = nullptr;

	UserProfile*     m_profile      = nullptr;
	McFileListModel* m_listModel    = nullptr;
	QListView*       m_listView     = nullptr;
	McFilterPanel*   m_filterPanel  = nullptr;
	McJobPanel*      m_jobPanel     = nullptr;
	JobQueue*        m_jobQueue    = nullptr;
	QLabel*          m_statusLabel  = nullptr;
	QLabel*          m_savedLabel   = nullptr;
	QProgressBar*    m_progressBar  = nullptr;
	QPushButton*     m_btnCancelScan     = nullptr;
	QPushButton*     m_btnCancelAnalyze  = nullptr;
	QSplitter*       m_splitter          = nullptr;
	QStringList      m_pendingRoots;
	bool             m_quickScanPending = false;   // whether m_pendingRoots is being drained as a quick scan
	QStringList      m_newFilesFound;   // accumulated across chained roots within one scan session
	int              m_scannedSoFar = 0;   // files scanned in already-finished roots this session
	QTimer*          m_analyzeRefreshTimer = nullptr;
	QThread*         m_loadThread      = nullptr;
	LibraryLoader*   m_loader          = nullptr;
	QThread*         m_scanThread      = nullptr;
	ScanWorker*      m_scanWorker      = nullptr;
	QThread*         m_analyzeThread   = nullptr;
	AnalyzeWorker*   m_analyzeWorker   = nullptr;
	QThread*         m_simulateThread  = nullptr;
	SimulateWorker*  m_simulateWorker  = nullptr;
	McWhatIfDialog*  m_whatIfDialog    = nullptr;
	int              m_analyzeJobCount     = 0;
	int              m_savedJobPanelHeight = 0;
	bool             m_jobPanelPinned     = false;
	bool             m_firstShowDone     = false;
	bool             m_splitterRestored  = false;
	QString          m_currentJobFilename;
	int              m_queuedAtStart   = 0;
#ifdef Q_OS_WIN
	ITaskbarList3*   m_taskbar = nullptr;
#endif
};

} // namespace Mc
