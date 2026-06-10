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

namespace Mc {

class JobQueue;
class McJobListModel;

class McJobPanel : public QWidget
{
	Q_OBJECT
public:
	explicit McJobPanel(QWidget* parent = nullptr);

	void setJobQueue(JobQueue* queue);
	void refresh();
	void refreshPaged(int limit);

	QComboBox* statusCombo() const { return m_statusFilter; }
	QComboBox* sortCombo()   const { return m_sortCombo;    }

	// After analyzeSingleFile creates a proposed job, call this to switch the
	// filter to Proposed, select the new row and scroll it into view.
	void scrollToFileJob(qint64 fileId);

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

private slots:
	void onQueueSelected();
	void onQueueAll();
	void onUnqueueSelected();
	void onRemoveSelected();
	void onPreviewCommand();
	void onShowSummary();
	void onStart();
	void onPause();
	void onCancel();

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
	QPushButton*    m_btnStart            = nullptr;
	QPushButton*    m_btnPause            = nullptr;
	QPushButton*    m_btnCancel           = nullptr;
	QPushButton*    m_btnRemove           = nullptr;
	QPushButton*    m_btnPreviewCmd       = nullptr;
	QPushButton*    m_btnSummary          = nullptr;
	QLineEdit*      m_filterEdit          = nullptr;
	QComboBox*      m_statusFilter        = nullptr;
	QComboBox*      m_sortCombo           = nullptr;
	QWidget*        m_ratingSlider        = nullptr;
	QLabel*         m_ratingLabel         = nullptr;
	quint32         m_qfFlags             = 0;      // active quick-filter bitmask
	JobQueue*       m_queue               = nullptr;
	QElapsedTimer   m_jobTimer;
	QTimer*         m_etaTimer            = nullptr;
};

} // namespace Mc
