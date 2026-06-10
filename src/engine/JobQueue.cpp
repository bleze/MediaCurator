#include "engine/JobQueue.h"
#include "engine/RemuxJob.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"
#include "core/ExternalTools.h"
#include "scanner/FfprobeScanner.h"

#include <QDebug>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStorageInfo>

namespace Mc {

JobQueue::JobQueue(QObject* parent)
	: QObject(parent)
{}

void JobQueue::start()
{
	m_paused  = false;
	m_running = true;

	if (m_currentJob) return; // already running a job

	runNext();
}

void JobQueue::pause()
{
	m_paused = true;
	// Current job keeps running; we just won't start the next one
}

void JobQueue::cancel()
{
	m_paused  = false;
	m_running = false;

	if (m_currentJob) {
		m_currentJob->cancel();
		// onJobFinished will fire, clean up the temp file, and mark the job failed.
		// Remaining queued jobs stay queued — Start resumes from the next one.
	}
}

void JobQueue::runNext()
{
	if (!m_running || m_paused) return;

	// Always re-query for a fresh sorted order — new jobs may have been added
	// while the previous job was running and must be sorted correctly.
	m_queue.clear();
	for (const JobRecord& j : DatabaseManager::instance().queuedJobs(m_sortMode))
		m_queue.append(j.id);

	if (m_queue.isEmpty()) {
		m_running = false;
		emit allFinished();
		return;
	}

	startJob(m_queue.takeFirst());
}

void JobQueue::startJob(qint64 jobId)
{
	auto& db = DatabaseManager::instance();

	// Load job record to get command args and fileId
	const auto jobs = db.queuedJobs(m_sortMode);
	QString commandArgsJson;
	QString descriptionText;
	qint64  fileId = -1;
	for (const JobRecord& j : jobs) {
		if (j.id == jobId) {
			commandArgsJson = j.commandArgsJson;
			fileId          = j.fileId;
			descriptionText = j.descriptionText;
			break;
		}
	}

	if (commandArgsJson.isEmpty()) {
		// Job no longer queued — skip
		runNext();
		return;
	}

	// Disk space guard — need at least as much free space as the source file.
	// Network drives (UNC paths, mapped drives) may return isValid()=false or
	// report inaccurate values; skip the check silently in that case.
	if (fileId > 0) {
		const auto fileOpt = db.fileById(fileId);
		if (fileOpt && fileOpt->sizeBytes > 0) {
			const QStorageInfo storage(QFileInfo(fileOpt->path).absolutePath());
			if (!storage.isValid() || storage.isReadOnly()) {
				// Network drive or read-only volume — can't query free space, proceed.
			} else if (storage.bytesAvailable() < fileOpt->sizeBytes) {
				const double needed = fileOpt->sizeBytes / 1073741824.0;
				const double avail  = storage.bytesAvailable() / 1073741824.0;
				emit warning(QStringLiteral(
				    "Not enough disk space for \"%1\" — need %2 GB, have %3 GB free. Queue stopped.")
				    .arg(fileOpt->filename)
				    .arg(needed, 0, 'f', 2)
				    .arg(avail,  0, 'f', 2));
				m_running = false;
				emit allFinished();
				return;
			}
		}
	}

	QStringList args;
	const QJsonArray arr = QJsonDocument::fromJson(commandArgsJson.toUtf8()).array();
	for (const auto& v : arr) args << v.toString();

	const QString mkvmergePath = ExternalTools::instance().mkvmergePath();
	m_currentJob    = new RemuxJob(jobId, mkvmergePath, args, descriptionText, m_writeJobLog, this);
	m_currentJobId  = jobId;
	m_currentFileId = fileId;

	connect(m_currentJob, &RemuxJob::finished,
	        this, &JobQueue::onJobFinished);
	connect(m_currentJob, &RemuxJob::progressChanged,
	        this, [this](int pct) { emit progressChanged(m_currentJobId, pct); });

	db.updateJobStatus(jobId, "running");
	emit jobStarted(jobId);

	m_currentJob->run();
}

void JobQueue::onJobFinished(int exitCode, const QString& log, qint64 savedBytes)
{
	const qint64 jobId  = m_currentJobId;
	const qint64 fileId = m_currentFileId;
	const bool   ok     = (exitCode == 0);

	auto& db = DatabaseManager::instance();
	db.updateJobStatus(jobId, ok ? "done" : "failed", exitCode, log);
	if (ok && savedBytes > 0) {
		db.updateJobSavedBytes(jobId, savedBytes);
		auto& settings = AppSettings::instance();
		const qint64 prev = settings.value(QStringLiteral("stats/totalReclaimedBytes"), 0LL).toLongLong();
		settings.setValue(QStringLiteral("stats/totalReclaimedBytes"), prev + savedBytes);
	}

	// Capture paths before deleteLater invalidates the job object
	const QString finalOutput   = m_currentJob->finalOutputPath();
	const QString originalInput = m_currentJob->inputFilePath();

	emit jobFinished(jobId, ok, savedBytes);

	m_currentJob->deleteLater();
	m_currentJob    = nullptr;
	m_currentJobId  = -1;
	m_currentFileId = -1;

	// Re-scan the file so the card reflects the new track layout.
	// For non-MKV / ISO jobs the output path differs from the input — update the
	// DB path first so rescanFile() reads the correct (new) file location.
	if (ok && fileId > 0) {
		if (!finalOutput.isEmpty() && finalOutput != originalInput) {
			const QFileInfo fi(finalOutput);
			db.updateFilePath(fileId, finalOutput, fi.fileName());
		}
		rescanFile(fileId);
	}

	if (m_running && !m_paused) {
		runNext();
	} else if (m_paused) {
		// The running job finished but the queue is paused, so we won't start
		// the next one. If the queue is also exhausted, transition to idle so
		// the UI doesn't stay stuck in "processing" with m_running=true forever.
		if (DatabaseManager::instance().queuedJobs(m_sortMode).isEmpty()) {
			m_running = false;
			emit allFinished();
		}
	}
}

void JobQueue::rescanFile(qint64 fileId)
{
	auto& db = DatabaseManager::instance();

	const auto fileOpt = db.fileById(fileId);
	if (!fileOpt) {
		qWarning() << "JobQueue::rescanFile: fileId" << fileId << "not found in DB";
		return;
	}

	const QString ffprobePath = ExternalTools::instance().ffprobePath();
	FfprobeScanner scanner(ffprobePath);

	const FfprobeScanner::ScanResult result = scanner.scanFile(fileOpt->path);
	if (!result.success) {
		qWarning() << "JobQueue::rescanFile: ffprobe failed for" << fileOpt->path
		           << "—" << result.errorMessage;
		return;
	}

	// Preserve the existing scan_run_id; update file metadata + streams
	FileRecord updated       = result.file;
	updated.scanRunId        = fileOpt->scanRunId;
	// Keep the original language that was already detected/stored
	if (updated.originalLanguage.isEmpty())
		updated.originalLanguage = fileOpt->originalLanguage;

	(void)db.upsertFile(updated);
	db.insertStreams(fileId, result.streams);

	emit fileRescanned(fileId);
}

} // namespace Mc
