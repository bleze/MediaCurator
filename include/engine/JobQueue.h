#pragma once
#include "core/DatabaseManager.h"
#include <QObject>
#include <QList>

namespace Mc {

class RemuxJob;

class JobQueue : public QObject
{
	Q_OBJECT
public:
	explicit JobQueue(QObject* parent = nullptr);

	void start();
	void pause();
	void cancel();
	// Start a specific job immediately, bypassing the normal sort order.
	// No-op if a job is already running.
	void runJob(qint64 jobId);

	void setWriteJobLog(bool v) { m_writeJobLog = v; }
	void setSortMode(JobSortMode mode) { m_sortMode = mode; }

	bool isRunning()   const { return m_running; }
	bool isPaused()    const { return m_paused; }
	// True only when mkvmerge is actively executing — use this for close guards.
	// isRunning() stays true while paused-with-queued-jobs, which is not "running".
	bool hasActiveJob() const { return m_currentJob != nullptr; }

signals:
	void jobStarted(qint64 jobId);
	void jobFinished(qint64 jobId, bool success, qint64 savedBytes);
	void fileRescanned(qint64 fileId);
	void allFinished();
	void progressChanged(qint64 jobId, int percent);
	void warning(const QString& message);

	// Emitted when mkvmerge completed but reported a "track not found" mismatch.
	// The .tmp output is held on disk pending user confirmation.
	void reviewRequested(qint64 jobId, const QString& filename,
	                     const QString& warningText,
	                     const QString& commandArgsJson,
	                     const QList<StreamRecord>& sourceStreams,
	                     const QList<StreamRecord>& outputStreams);

public slots:
	// Called by McJobReviewDialog result — commit the held .tmp as the final file.
	void commitReview(qint64 jobId);
	// Called by McJobReviewDialog result — discard the held .tmp.
	// reanalyze=true  → delete stale job and rescan so user can re-analyze
	// reanalyze=false → mark job failed, leave user to retry manually
	void rejectReview(qint64 jobId, bool reanalyze);
#ifdef QT_DEBUG
	// Test helper — opens the review dialog for any job using its current DB streams.
	void debugTriggerReview(qint64 jobId);
#endif

private slots:
	void onJobFinished(int exitCode, const QString& log, qint64 savedBytes);

private:
	void runNext();
	void startJob(const JobRecord& job);
	void rescanFile(qint64 fileId, const QString& filePath);

	RemuxJob*      m_currentJob   = nullptr;
	qint64         m_currentJobId = -1;
	qint64         m_currentFileId = -1;
	bool           m_running      = false;
	bool           m_paused       = false;
	bool           m_writeJobLog  = false;
	JobSortMode    m_sortMode     = JobSortMode::SmallestFirst;

	// State held while waiting for user to review a track-mismatch result
	qint64         m_reviewJobId    = -1;
	qint64         m_reviewFileId   = -1;
	QString        m_reviewTmpPath;
	QString        m_reviewFinalPath;
	QString        m_reviewSrcPath;
	qint64         m_reviewOrigSize = 0;
	int            m_reviewExitCode = 1;
};

} // namespace Mc
