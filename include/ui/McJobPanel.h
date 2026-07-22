#pragma once
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QTimer;
class QToolButton;

namespace Mc {

class JobQueue;
class McGoalProgressBar;
class McJobListModel;
class McStorageGroupChipToggle;

class McJobPanel : public QWidget
{
	Q_OBJECT
public:
	explicit McJobPanel(QWidget* parent = nullptr);

	void setJobQueue(JobQueue* queue);
	void refresh();
	void refreshPaged(int limit);

	// Forwarded to the card delegate — see McCardDelegate::setTmdbConfigured.
	void setTmdbConfigured(bool configured);

	// Forwarded to the card delegate — see McCardDelegate::setMultiGroupBadgeEnabled.
	void setMultiGroupBadgeEnabled(bool enabled);

	// Forwarded to the card delegate — see McCardDelegate::setFanartOpacity.
	void setFanartOpacity(double opacity);

	// Rebuilds the storage-group chip row from the current folder→group assignment
	// and reapplies its filter to the underlying model. Call after folders are
	// reassigned (e.g. Manage Folders dialog closes) so the chip set stays live
	// without requiring an app restart. Renders no chips (and takes no space)
	// when a single storage group is in use.
	void refreshStorageGroups();

	// Hide Movies/TV/Docs/Misc pills until the queue has at least one classified
	// media type. Also called from refresh() so it tracks enrichment live.
	void setMediaCategoryFiltersVisible(bool visible);

	QComboBox* statusCombo() const { return m_statusFilter; }
	QComboBox* sortCombo()   const { return m_sortCombo;    }

	// After analyzeSingleFile creates a proposed job, call this to switch the
	// filter to Proposed, select the new row and scroll it into view.
	void scrollToFileJob(qint64 fileId);

	// Call after directly updating an external sidecar stream's language + path in the
	// DB (e.g. from the Library view's badge menu) so this panel's job card, if any
	// exists for the file, reflects it immediately without a full reload.
	void syncExternalStreamLanguage(qint64 fileId, int streamIndex,
	                                 const QString& language, const QString& externalPath);
	QList<qint64> visibleFileIds() const;

	// Call after a file is deleted from the library (its jobs are already gone from
	// the DB) so this panel drops that file's card(s) without a full reload — the jobs
	// table can hold a large history, and reload() re-queries and re-derives all of it.
	void removeJobsForFile(qint64 fileId);

public slots:
	void onJobStatusChanged(qint64 jobId, const QString& status);
	void repaintCards();
	void setRatingForFile(qint64 fileId, double rating);
	void setTitleForFile(qint64 fileId, const QString& title, int year);
	void setMediaTypeForFile(qint64 fileId, const QString& mediaType);

signals:
	void jobsChanged(int count);   // emitted after any load/remove; count = total visible rows
	void previewRequested(qint64 fileId);
	void playRequested(const QString& filePath);
	void editImdbLinkRequested(qint64 fileId);
	void editImdbLinksRequested(const QList<qint64>& fileIds);  // batch: multiple selected
	void refreshPosterRequested(qint64 fileId);
	void refreshPosterBatchRequested(const QList<qint64>& fileIds);  // batch: multiple selected
	void downloadSubtitlesRequested(qint64 fileId);
#ifdef QT_DEBUG
	void debugReviewRequested(qint64 jobId);
#endif

private slots:
	void onQueueSelected();
	void onQueueAll();
	void onUnqueueSelected();
	void onRemoveSelected();
	void onPreviewCommand(qint64 jobId);
	void onShowSummary();
	void onStartPause();
	void onCancel();
	void onStartWithGoal();
	void onEditGoal();

private:
	void setupUi();
	void updateFooter();
	void updateStatusCombo();
	void focusJob(qint64 jobId);
	void markJobForFocus(qint64 jobId);
	void applyStorageGroupFilter();
	bool eventFilter(QObject* obj, QEvent* event) override;
	static QString formatSaved(qint64 bytes);

	McJobListModel* m_model               = nullptr;
	QListView*      m_listView            = nullptr;
	QLabel*         m_footerLabel         = nullptr;
	QPushButton*    m_btnQueueSelected    = nullptr;
	QPushButton*    m_btnUnqueue          = nullptr;
	QPushButton*    m_btnQueueAll         = nullptr;
	QPushButton*    m_btnStartPause        = nullptr;
	QToolButton*    m_btnStartGoalMenu    = nullptr;
	QPushButton*    m_btnCancel           = nullptr;
	QPushButton*    m_btnRemove           = nullptr;
	QPushButton*    m_btnSummary          = nullptr;
	McGoalProgressBar* m_goalBar          = nullptr;
	QLineEdit*      m_filterEdit          = nullptr;
	QComboBox*      m_statusFilter        = nullptr;
	QComboBox*      m_sortCombo           = nullptr;
	QWidget*        m_ratingSlider        = nullptr;
	QLabel*         m_ratingLabel         = nullptr;
	quint32         m_qfFlags             = 0;      // active quick-filter bitmask
	int             m_lastQueueSortIdx    = 0;      // last non-MostRecentFirst combo index (in-session)
	JobQueue*       m_queue               = nullptr;
	QElapsedTimer   m_jobTimer;
	QTimer*         m_etaTimer            = nullptr;

	// Per-running-job smoothed throughput for ETA/speed — see updateFooter().
	struct JobEtaState {
		qint64  startedAtMs   = 0;
		qint64  lastSampleMs  = -1;
		int     lastProgress  = -1;
		double  emaRatePerMs  = -1.0;
		QString lastPhase;
	};
	QHash<qint64, JobEtaState> m_etaByJob;
	// Set when a job is promoted to queued; consumed when Running/Queued filter is shown.
	qint64 m_focusJobId = -1;

	// Movies/TV/Docs/Misc pills + separator — hidden until a job file is classified.
	QWidget*     m_mediaCategoryContainer = nullptr;

	// Storage-group chip row — own container so refreshStorageGroups() can rebuild
	// it independently of the rest of the filter bar's layout.
	QWidget*     m_storageGroupContainer = nullptr;
	QHBoxLayout* m_storageGroupLayout    = nullptr;
	QList<McStorageGroupChipToggle*> m_storageGroupChips;
};

} // namespace Mc
