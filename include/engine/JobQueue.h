#pragma once
#include "core/DatabaseManager.h"
#include <QElapsedTimer>
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
	void setMergeSidecarSubtitles(bool v) { m_mergeSidecarSubtitles = v; }
	void setSortMode(JobSortMode mode) { m_sortMode = mode; }
	void setUseLocalStaging(bool v) { m_useLocalStaging = v; }
	void setLocalStagingDir(const QString& dir) { m_localStagingDir = dir; }

	// Runs queued jobs largest-savings-first (regardless of the current sort mode)
	// until cumulative actual savings reach goalBytes, then stops the queue.
	void startWithGoal(qint64 goalBytes);
	// Raises the active goal target while a goal run is in progress. Values at or
	// below the current target are ignored — a goal can only be raised, never
	// lowered, since jobs already picked toward it can't be un-started.
	void setGoalBytes(qint64 goalBytes);

	bool isGoalMode() const { return m_goalBytes > 0; }
	qint64 goalBytes() const { return m_goalBytes; }
	qint64 goalSavedBytes() const { return m_goalSavedBytes; }
	// Live estimate for whichever job is currently running, used by the UI to show
	// partial progress toward the goal before the job actually finishes.
	qint64 currentJobEstimatedSavings() const { return m_currentJobEstimatedSavings; }

	bool isRunning()   const { return m_running; }
	bool isPaused()    const { return m_paused; }
	// True while mkvmerge is actively executing OR a job is in its terminal
	// file-I/O phase (rename/delete of the source or output file) — use this for
	// close guards. isRunning() stays true while paused-with-queued-jobs, which is
	// not "running".
	bool hasActiveJob() const { return m_currentJob != nullptr || m_finishBusy; }

signals:
	void jobStarted(qint64 jobId);
	void jobFinished(qint64 jobId, bool success, qint64 savedBytes);
	void fileRescanned(qint64 fileId);
	// Emitted after the post-mismatch rescan completes — main window auto-analyzes this file.
	void fileNeedsReanalysis(qint64 fileId);
	void allFinished();
	void progressChanged(qint64 jobId, int percent);
	// Emitted whenever accumulated actual savings toward an active goal change
	// (i.e. after each job finishes while startWithGoal() is active).
	void goalProgressChanged(qint64 savedBytes, qint64 goalBytes);
	// Emitted once, right when a goal run's cumulative savings reach the target —
	// fires just before the queue stops itself. jobsCompleted counts successful
	// jobs run during this goal run; elapsedMs is wall-clock time since startWithGoal().
	void goalReached(qint64 savedBytes, qint64 goalBytes, int jobsCompleted, qint64 elapsedMs);
	// Current size on disk of the .tmp output file, sampled synchronously right when
	// RemuxJob parses a new percent from mkvmerge's stdout — lets the UI show a live
	// output-size-vs-original bar while running.
	void outputSizeChanged(qint64 jobId, qint64 bytes);
	// Emitted when a running job enters a distinct sub-phase (e.g. copying a
	// staged output back to its real destination) — UI relabels the progress
	// pill using this instead of the default "Running" text.
	void phaseChanged(qint64 jobId, const QString& label);
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
	// Connected to this object's own jobFinished() — accumulates actual savings
	// toward an active goal without needing a hook at every jobFinished emit site.
	void accumulateGoalProgress(qint64 jobId, bool success, qint64 savedBytes);

private:
	void runNext();
	void startJob(const JobRecord& job);
	void rescanFile(qint64 fileId, const QString& filePath, bool triggerReanalysis = false);
	// Deletes sidecar subtitle files that were merged into this job's remux output,
	// and flips their entries in the job's frozen stream snapshot from external to
	// internal so a "done" card stops showing them as standalone sidecar files.
	void deleteMergedSidecars(qint64 jobId);
	// Tail of the tag-edit path once mkvpropedit (if any) has finished — deletes/renames
	// sidecar files, updates the DB, emits jobFinished, rescans, and advances the queue.
	// Runs on a background thread; clears m_finishBusy just before handing back to runNext().
	void finishTagEditJob(qint64 jobId, qint64 fileId, bool propEditOk, const QString& log,
	                       const QString& sidecarDeletionsJson, const QString& flagChangesJson,
	                       const QList<StreamRecord>& allStreams);

	RemuxJob*      m_currentJob   = nullptr;
	qint64         m_currentJobId = -1;
	qint64         m_currentFileId = -1;
	bool           m_running      = false;
	bool           m_paused       = false;
	bool           m_writeJobLog  = false;
	bool           m_mergeSidecarSubtitles = true;
	bool           m_useLocalStaging = false;
	QString        m_localStagingDir;
	JobSortMode    m_sortMode     = JobSortMode::SmallestFirst;
	// GB-goal run state — see startWithGoal(). m_goalBytes == 0 means no goal is active.
	qint64         m_goalBytes                  = 0;
	qint64         m_goalSavedBytes             = 0;
	qint64         m_currentJobEstimatedSavings = 0;
	int            m_goalJobsCompleted          = 0;
	QElapsedTimer  m_goalTimer;
	// True whenever a job (remux, tag-edit, or review commit) is in its terminal
	// file-I/O phase — set at the start of that phase, cleared right before runNext().
	bool           m_finishBusy   = false;

	// State held while waiting for user to review a track-mismatch result
	qint64         m_reviewJobId    = -1;
	qint64         m_reviewFileId   = -1;
	QString        m_reviewTmpPath;
	QString        m_reviewFinalPath;
	QString        m_reviewSrcPath;
	qint64         m_reviewOrigSize = 0;
	int            m_reviewExitCode = 1;
	// True when m_reviewTmpPath is a local staging file rather than sitting next
	// to m_reviewFinalPath — commitReview() must copy it over instead of renaming.
	bool           m_reviewWasStaged = false;
};

} // namespace Mc
