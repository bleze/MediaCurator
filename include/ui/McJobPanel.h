#pragma once
#include <QElapsedTimer>
#include <QList>
#include <QWidget>

class QCheckBox;
class QComboBox;
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

public slots:
	void onJobStatusChanged(qint64 jobId, const QString& status);
	void repaintCards();
	void setRatingForFile(qint64 fileId, double rating);

signals:
	void jobsChanged(int count);   // emitted after any load/remove; count = total visible rows
	void previewRequested(qint64 fileId);
	void playRequested(const QString& filePath);
	void editImdbLinkRequested(qint64 fileId);
	void editImdbLinksRequested(const QList<qint64>& fileIds);  // batch: multiple selected
	void refreshPosterRequested(qint64 fileId);
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

	// Exponentially-smoothed throughput rate (% progress per ms) for the currently
	// running job, used to compute ETA from recent throughput rather than the
	// whole-job average — see updateFooter(). -1 means "not yet established".
	qint64  m_etaSampleJobId  = -1;
	qint64  m_etaLastSampleMs = -1;
	int     m_etaLastProgress = -1;
	double  m_etaEmaRatePerMs = -1.0;
	// Sub-phase label (e.g. "Copying to NAS") the current sample window was
	// established under — a change here (mux -> copy) resets the EMA the same
	// way a job-ID change does, since progress resets to 0 and the two phases
	// have unrelated throughput.
	QString m_etaLastPhase;
};

} // namespace Mc
