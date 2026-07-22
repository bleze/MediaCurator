#pragma once

#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStringList>
#include <QThread>
#include <QTimer>

#include <functional>

#include "engine/HighscoreClient.h"

class QPaintEvent;
class QProgressDialog;
class QSplashScreen;

#ifdef Q_OS_WIN
struct ITaskbarList3;
#endif

namespace Mc {

class AnalyzeWorker;
class JobQueue;
class LibraryLoader;
class McFileListModel;
class McFilterPanel;
class McHighscoreBand;
class McHighscoreDialog;
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

	// main() hands off the splash; window stays hidden until dismissSplash().
	void attachSplash(QSplashScreen* splash, const QIcon& appIcon = {});

	// main() hands this off right after construction. closeEvent() invokes it the
	// moment it commits to actually closing — before the slow teardown below —
	// so the single-instance named pipe stops answering immediately instead of
	// staying alive for this process's entire (possibly multi-second) shutdown.
	// Otherwise a MediaCurator.exe launched by the update installer's finish page
	// while this process is still unwinding sees a "live" instance, silently pings
	// it, and exits — leaving nothing running until the user starts it manually.
	void setSingleInstanceLockReleaser(std::function<void()> releaser) { m_releaseSingleInstanceLock = std::move(releaser); }

	// Set once closeEvent() has actually accepted the close after the "Shut Down
	// After" job-wait path. main() checks this after app.exec() returns and only
	// then runs the OS shutdown command, so it never races our own teardown.
	bool shutdownRequested() const { return m_shutdownOnClose; }

protected:
	void closeEvent(QCloseEvent* event) override;
	void showEvent(QShowEvent* event) override;
	void paintEvent(QPaintEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;
	bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private slots:
	void dismissSplash();
	void completeStartup();
	void onScanFolder();
	void onScanLibrary();
	void onQuickScan();
	void onRemoveFolder();

	void onRefreshView();
	void onAnalyzeLibrary();
	void onQuickAnalyze();
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
	void onUpdateAvailable(QString version, QString htmlUrl, QString releaseNotes,
	                        QString installerUrl, bool silent);
	void onUpdateUpToDate(bool silent);
	void onUpdateCheckFailed(QString error, bool silent);
	void onUpdateDownloadProgress(qint64 received, qint64 total);
	void onUpdateDownloadFailed(QString error);
	void onUpdateInstallerLaunched();

private:
	void setupUi();
	void setupActions();
	void setupToolBar();
	void setupMenuBar();
	void setupStatusBar();
	void applyDarkBackgrounds();
	void startLibraryLoader();
	void startBackgroundLibraryLoad();
	void ensureOnScreen();
	void setNativeWindowBackground();
	void startScanRoots(const QStringList& roots, bool quickScan);
	void enqueueScanRoot(const QString& root, bool quickScan);
	void createScanWorkerForGroup(int groupId, const QString& folderPath, bool quickScan);
	void onScanProgressForGroup(int groupId, int current, int total, const QString& currentFile);
	void onScanFinishedForGroup(int groupId, int scanned, int added, int updated, int failed,
	                            int skipped, int removed, QStringList newFiles);
	void stopAllScanWorkers(bool waitForThreads);
	void updateScanStatusLabel();
	[[nodiscard]] bool isScanning() const;
	void setScanningState(bool scanning, bool quickScan = false);
	void updateSavedLabel();
	void updateActionStates();         // enable/disable scan-lib and analyze based on roots + file count
	void updateJobPanelVisibility(bool forceShow = false);   // show/hide job panel based on whether any jobs exist
	void updateRemuxStatusBar();
	void updateHighscoreVisibility();
	void submitHighscoreIfDue();
	QString promptForHighscoreName(qint64 mb);
	void onHighscoreLeaderboardReady(QList<HighscoreEntry> entries);
	[[nodiscard]] qint64 currentReclaimedMb() const;
	void launchInDefaultPlayer(const QString& rawPath);
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
	QAction*     m_actLibraryFolders  = nullptr;
	QAction*     m_actAnalyze       = nullptr;
	QAction*     m_actQuickAnalyze  = nullptr;
	QAction*     m_actSimulate      = nullptr;
	QAction*     m_actSettings      = nullptr;
	QAction*     m_actRefresh       = nullptr;
	QAction*  m_actToggleQueue   = nullptr;
	QWidget*  m_menuQueueBtn     = nullptr;  // view-menu McQueueToggle for the queue toggle
	QAction*  m_actCheckUpdates  = nullptr;
	QAction*  m_actToggleHighscore = nullptr;
	QWidget*  m_menuHighscoreBtn   = nullptr;  // view-menu McQueueToggle for the highscore toggle

	UserProfile*     m_profile      = nullptr;
	McFileListModel* m_listModel    = nullptr;
	QListView*       m_listView     = nullptr;
	McFilterPanel*   m_filterPanel  = nullptr;
	McJobPanel*      m_jobPanel     = nullptr;
	JobQueue*        m_jobQueue    = nullptr;
	QLabel*          m_statusLabel  = nullptr;
	QLabel*          m_savedLabel   = nullptr;
	QProgressBar*    m_progressBar  = nullptr;
	QProgressBar*    m_posterProgressBar   = nullptr;
	QPushButton*     m_btnCancelScan       = nullptr;
	QPushButton*     m_btnCancelAnalyze    = nullptr;
	QPushButton*     m_btnCancelSubtitles  = nullptr;
	QPushButton*     m_btnCancelPosterRefresh = nullptr;
	QProgressDialog* m_updateProgressDlg   = nullptr;
	QSplitter*       m_splitter          = nullptr;
	struct ScanGroupState {
		QThread*    thread       = nullptr;
		ScanWorker* worker       = nullptr;
		QStringList pendingRoots;
		bool        quickScan    = false;
		int         progressCurrent = 0;
		QString     currentFile;
	};

	QHash<int, ScanGroupState> m_scanGroups;
	QStringList      m_newFilesFound;   // accumulated across chained roots within one scan session
	int              m_scannedSoFar = 0;   // files scanned in already-finished roots this session
	// Accumulated across chained roots this scan session — only an updated or removed
	// file can invalidate an existing job (see ScanWorker::deletePendingJobsForFile),
	// so the job panel only needs refreshing when either is nonzero. A pure "found new
	// files" scan (the common Quick Scan case) can skip re-rendering hundreds of
	// unrelated job cards entirely.
	int              m_updatedSoFar = 0;
	int              m_removedSoFar = 0;
	QTimer*          m_analyzeRefreshTimer = nullptr;
	QThread*         m_loadThread      = nullptr;
	LibraryLoader*   m_loader          = nullptr;
	QThread*         m_analyzeThread   = nullptr;
	AnalyzeWorker*   m_analyzeWorker   = nullptr;
	QThread*         m_simulateThread  = nullptr;
	SimulateWorker*  m_simulateWorker  = nullptr;
	McWhatIfDialog*  m_whatIfDialog    = nullptr;
	int              m_analyzeJobCount     = 0;
	int              m_savedJobPanelHeight = 0;
	bool             m_jobPanelPinned     = false;
	bool             m_shutdownOnClose    = false;   // set by the "Quit && Shut Down" job-wait path
	bool             m_closeOnJobFinishPending = false; // guards against stacking duplicate jobFinished->close() hooks
	bool             m_closeHandled       = false;   // true once closeEvent() has run its teardown once
	std::function<void()> m_releaseSingleInstanceLock;   // see setSingleInstanceLockReleaser()
	McHighscoreBand*   m_highscoreBand       = nullptr;
	McHighscoreDialog* m_highscoreDialog     = nullptr;
	QTimer*            m_highscoreDebounce   = nullptr;
	bool               m_highscoreBandPinned = false;
	QList<HighscoreEntry> m_lastHighscoreEntries;
	bool             m_firstShowDone       = false;
	bool             m_startupCompleteDone = false;
	bool             m_splashDismissed     = false;
	bool             m_backgroundLoadStarted = false;
	bool             m_splitterRestored  = false;
	QSplashScreen*   m_splash            = nullptr;
	QIcon            m_startupIcon;
	QHash<qint64, QString> m_runningJobNames;
	QHash<qint64, int>     m_runningJobProgress;
	int              m_queuedAtStart   = 0;
#ifdef Q_OS_WIN
	ITaskbarList3*   m_taskbar = nullptr;
	void*            m_nativeBgBrush = nullptr;   // HBRUSH — deleted before replace / in dtor
#endif
};

} // namespace Mc
