#pragma once
#include "core/DatabaseManager.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QList>
#include <QProcess>
#include <QSet>

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
	// Cancel a single running job — it goes back to "queued" while any other
	// concurrently-running jobs (in other storage groups) keep going.
	void cancelJob(qint64 jobId);
	// Start a specific job immediately when its storage group is idle.
	void runJob(qint64 jobId);

	void setWriteJobLog(bool v) { m_writeJobLog = v; }
	void setMergeSidecarSubtitles(bool v) { m_mergeSidecarSubtitles = v; }
	void setDetectSubtitleLanguage(bool v) { m_detectSubtitleLanguage = v; }
	void setSortMode(JobSortMode mode) { m_sortMode = mode; }
	void setUseLocalStaging(bool v) { m_useLocalStaging = v; }
	void setLocalStagingDir(const QString& dir) { m_localStagingDir = dir; }

	void startWithGoal(qint64 goalBytes);
	void setGoalBytes(qint64 goalBytes);

	bool isGoalMode() const { return m_goalBytes > 0; }
	qint64 goalBytes() const { return m_goalBytes; }
	qint64 goalSavedBytes() const { return m_goalSavedBytes; }
	qint64 currentJobEstimatedSavings() const;
	[[nodiscard]] bool isGroupBusy(int storageGroup) const;
	[[nodiscard]] bool canRunJobImmediately(qint64 jobId) const;

	bool isRunning()   const { return m_running; }
	bool isPaused()    const { return m_paused; }
	bool hasActiveJob() const;

signals:
	void jobStarted(qint64 jobId);
	void jobFinished(qint64 jobId, bool success, qint64 savedBytes);
	void jobRequeued(qint64 jobId);
	void fileRescanned(qint64 fileId);
	void fileNeedsReanalysis(qint64 fileId);
	void allFinished();
	void progressChanged(qint64 jobId, int percent);
	void goalProgressChanged(qint64 savedBytes, qint64 goalBytes);
	void goalReached(qint64 savedBytes, qint64 goalBytes, int jobsCompleted, qint64 elapsedMs);
	void outputSizeChanged(qint64 jobId, qint64 bytes);
	void phaseChanged(qint64 jobId, const QString& label);
	void warning(const QString& message);
	void reviewRequested(qint64 jobId, const QString& filename,
	                     const QString& warningText,
	                     const QString& commandArgsJson,
	                     const QList<StreamRecord>& sourceStreams,
	                     const QList<StreamRecord>& outputStreams);

public slots:
	void commitReview(qint64 jobId);
	void rejectReview(qint64 jobId, bool reanalyze);
#ifdef QT_DEBUG
	void debugTriggerReview(qint64 jobId);
#endif

private slots:
	void onRemuxJobFinished(int storageGroup, qint64 jobId, qint64 fileId,
	                        int exitCode, const QString& log, qint64 savedBytes);
	void accumulateGoalProgress(qint64 jobId, bool success, qint64 savedBytes);

private:
	struct RunningJobSlot {
		RemuxJob* remux           = nullptr;
		QProcess* propEdit        = nullptr;
		qint64    jobId           = -1;
		qint64    fileId          = -1;
		qint64    estimatedSavings = 0;
		bool      finishBusy      = false;
	};

	struct DirTimestamp {
		QDateTime origCreated;
		QDateTime origModified;
	};

	// excludeJobId skips that one job for this pass — used right after cancelJob()
	// frees a slot, so the job it just killed isn't immediately picked back up
	// before its "queued" status has had a chance to mean anything.
	void dispatchJobs(qint64 excludeJobId = -1);
	[[nodiscard]] bool tryStartJob(const JobRecord& job);
	void occupySlot(int storageGroup, qint64 jobId, qint64 fileId, qint64 estimatedSavings);
	void releaseSlot(int storageGroup);
	[[nodiscard]] RunningJobSlot* slotForJobId(qint64 jobId);
	void rescanFile(qint64 fileId, const QString& filePath, bool triggerReanalysis = false);
	void deleteMergedSidecars(qint64 jobId);
	void finishTagEditJob(int storageGroup, qint64 jobId, qint64 fileId, bool propEditOk,
	                      const QString& log, const QString& sidecarDeletionsJson,
	                      const QString& flagChangesJson, const QList<StreamRecord>& allStreams);
	[[nodiscard]] bool consumeAbortedJob(qint64 jobId, int storageGroup);
	// Restores the containing folder's timestamp to whatever it was before this
	// job's subtitle prefetch + mux started (captured in tryStartJob(), keyed by
	// jobId rather than kept on RunningJobSlot so it survives cancelJob()'s
	// releaseSlot() call, which runs before the killed process's async cleanup —
	// and thus this restore — actually happens). Removes the stashed entry.
	// No-op if nothing was stashed for this job (e.g. needs_review hasn't
	// resolved yet — commitReview()/rejectReview() consume it later instead).
	void restoreDirTimestampForJob(qint64 jobId, qint64 fileId);

	QHash<int, RunningJobSlot> m_runningByGroup;
	QHash<qint64, DirTimestamp> m_pendingDirTimestamps;
	QSet<qint64>   m_cancelledJobIds;
	bool           m_running      = false;
	bool           m_paused       = false;
	bool           m_writeJobLog  = false;
	bool           m_mergeSidecarSubtitles = true;
	bool           m_detectSubtitleLanguage = false;
	bool           m_useLocalStaging = false;
	QString        m_localStagingDir;
	JobSortMode    m_sortMode     = JobSortMode::SmallestFirst;
	qint64         m_goalBytes                  = 0;
	qint64         m_goalSavedBytes             = 0;
	int            m_goalJobsCompleted          = 0;
	QElapsedTimer  m_goalTimer;

	qint64         m_reviewJobId    = -1;
	qint64         m_reviewFileId   = -1;
	QString        m_reviewTmpPath;
	QString        m_reviewFinalPath;
	QString        m_reviewSrcPath;
	qint64         m_reviewOrigSize = 0;
	int            m_reviewExitCode = 1;
	bool           m_reviewWasStaged = false;
};

} // namespace Mc