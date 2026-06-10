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

private slots:
	void onJobFinished(int exitCode, const QString& log, qint64 savedBytes);

private:
	void runNext();
	void startJob(const JobRecord& job);

	RemuxJob*      m_currentJob   = nullptr;
	qint64         m_currentJobId = -1;
	qint64         m_currentFileId = -1;
	bool           m_running      = false;
	bool           m_paused       = false;
	bool           m_writeJobLog  = false;
	JobSortMode    m_sortMode     = JobSortMode::SmallestFirst;
};

} // namespace Mc
