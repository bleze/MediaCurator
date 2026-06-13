#include "engine/JobQueue.h"
#include "engine/ActionEngine.h"
#include "engine/RemuxJob.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"
#include "core/ExternalTools.h"
#include "scanner/FfprobeScanner.h"
#include "scanner/ScanWorker.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QThreadPool>

#include <filesystem>

#ifdef Q_OS_WIN
#include <windows.h>
static void preserveCreationTime(const QString& target, const QDateTime& origCreated)
{
	if (!origCreated.isValid()) return;
	const qint64 ns100 = (origCreated.toMSecsSinceEpoch() + Q_INT64_C(11644473600000)) * 10000;
	FILETIME ft;
	ft.dwLowDateTime  = static_cast<DWORD>(ns100 & 0xFFFFFFFF);
	ft.dwHighDateTime = static_cast<DWORD>((ns100 >> 32) & 0xFFFFFFFF);
	HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(target.utf16()),
	                       FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	SetFileTime(h, &ft, nullptr, nullptr);
	CloseHandle(h);
}
#endif

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

void JobQueue::runJob(qint64 jobId)
{
	if (m_currentJob) return; // don't interrupt a running job

	auto& db = DatabaseManager::instance();
	const auto jobOpt = db.jobById(jobId);
	if (!jobOpt) return;

	if (jobOpt->status == QLatin1String("proposed"))
		db.promoteJobsToQueued({jobId});
	else if (jobOpt->status != QLatin1String("queued"))
		return;

	m_paused  = false;
	m_running = true;
	startJob(*jobOpt);
}

void JobQueue::runNext()
{
	if (!m_running || m_paused) return;

	// Always re-query for a fresh sorted order — new jobs may have been added
	// while the previous job was running and must be sorted correctly.
	const auto jobs = DatabaseManager::instance().queuedJobs(m_sortMode);

	if (jobs.isEmpty()) {
		m_running = false;
		emit allFinished();
		return;
	}

	startJob(jobs.first());
}

void JobQueue::startJob(const JobRecord& job)
{
	auto& db = DatabaseManager::instance();

	const qint64  jobId           = job.id;
	const qint64  fileId          = job.fileId;
	const QString jobType         = job.jobType;
	const QString commandArgsJson = job.commandArgsJson;
	const QString flagChangesJson = job.flagChangesJson;
	const QString descriptionText = job.descriptionText;

	// tag_edit jobs have no commandArgsJson — that's expected; other types must have it.
	const bool isTagEdit = (jobType == QLatin1String("tag_edit"));
	if (!isTagEdit && commandArgsJson.isEmpty()) {
		runNext();
		return;
	}

	// ── Flag-only edit via mkvpropedit (+ optional sidecar file deletions / renames) ──
	if (isTagEdit) {
		const QString sidecarDeletionsJson = job.sidecarDeletionsJson;
		if (flagChangesJson.isEmpty() && sidecarDeletionsJson.isEmpty()) {
			db.updateJobStatus(jobId, "done", 0);
			emit jobStarted(jobId);
			emit jobFinished(jobId, true, 0);
			if (m_running && !m_paused) runNext();
			return;
		}

		db.updateJobStatus(jobId, "running");
		emit jobStarted(jobId);

		// Load streams to separate internal (mkvpropedit) vs external (rename) changes.
		const QList<StreamRecord> allStreams = fileId > 0
			? db.streamsForFile(fileId) : QList<StreamRecord>{};
		const QString internalFlags = ActionEngine::filterInternalFlagChanges(
			flagChangesJson, allStreams);

		bool ok = true;
		QString log;

		// Run mkvpropedit for internal (container) flag changes only
		if (!internalFlags.isEmpty()) {
			const auto fileOpt = db.fileById(fileId);
			if (!fileOpt) {
				db.updateJobStatus(jobId, "failed", -1);
				emit jobFinished(jobId, false, 0);
				if (m_running && !m_paused) runNext();
				return;
			}

			const QStringList args        = ActionEngine::buildPropEditArgs(fileOpt->path, internalFlags);
			const QString mkvpropeditPath = ExternalTools::instance().mkvpropeditPath();

			// mkvpropedit is near-instant; run it synchronously on this thread.
			QProcess proc;
			proc.setProcessChannelMode(QProcess::MergedChannels);
			proc.start(mkvpropeditPath, args);
			proc.waitForFinished(-1);
			const int exitCode = proc.exitCode();
			ok = (proc.exitStatus() == QProcess::NormalExit && exitCode == 0);
			const QByteArray rawLog = proc.readAllStandardOutput();
			log = rawLog.isEmpty()
				? (ok ? QStringLiteral("mkvpropedit completed (exit 0)")
				      : QStringLiteral("mkvpropedit failed (exit %1)").arg(exitCode))
				: QString::fromLocal8Bit(rawLog);
		}

		// Delete sidecar subtitle files
		if (ok && !sidecarDeletionsJson.isEmpty()) {
			const QJsonArray paths = QJsonDocument::fromJson(sidecarDeletionsJson.toUtf8()).array();
			for (const QJsonValue& v : paths) {
				const QString path = v.toString();
				if (!path.isEmpty() && !QFile::remove(path))
					qWarning() << "JobQueue: failed to delete sidecar" << path;
			}
		}

		// Rename sidecar files for external stream flag changes
		if (ok && !flagChangesJson.isEmpty() && !allStreams.isEmpty()) {
			QHash<int, QHash<QString, bool>> changesByStream;
			for (const QJsonValue& v : QJsonDocument::fromJson(flagChangesJson.toUtf8()).array()) {
				const QJsonObject o = v.toObject();
				changesByStream[o[QLatin1String("streamIndex")].toInt()]
				               [o[QLatin1String("flag")].toString()] = o[QLatin1String("value")].toBool();
			}
			for (const StreamRecord& s : allStreams) {
				if (!s.isExternal || s.externalPath.isEmpty()) continue;
				if (!changesByStream.contains(s.streamIndex)) continue;
				const auto& ch = changesByStream[s.streamIndex];
				const bool wantForced = ch.value(QLatin1String("forced"), s.isForced);
				const QString newPath = ActionEngine::computeRenamedSidecarPath(
					s.externalPath, wantForced);
				if (newPath != s.externalPath && !QFile::rename(s.externalPath, newPath))
					qWarning() << "JobQueue: failed to rename sidecar"
					           << s.externalPath << "->" << newPath;
			}
		}

		db.updateJobStatus(jobId, ok ? "done" : "failed", ok ? 0 : -1, log);
		emit jobFinished(jobId, ok, 0);

		if (ok && fileId > 0)
			rescanFile(fileId, {});  // picks up renamed sidecars automatically

		if (m_running && !m_paused) runNext();
		return;
	}

	// ── Remux job via mkvmerge ───────────────────────────────────────────────

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

	// Inject flag changes and sidecar subtitle files into the mkvmerge command.
	if (!args.isEmpty()) {
		const QList<StreamRecord> streams = db.streamsForFile(fileId);

		// Internal (container) track flag changes: inject before the input path.
		if (!flagChangesJson.isEmpty()) {
			const QString internalJson = ActionEngine::filterInternalFlagChanges(
				flagChangesJson, streams);
			if (!internalJson.isEmpty()) {
				const QString filteredJson = ActionEngine::filterFlagChangesForRemux(
					internalJson, args, streams);
				if (!filteredJson.isEmpty()) {
					const QStringList flagArgs = ActionEngine::buildFlagArgsForRemux(filteredJson);
					if (!flagArgs.isEmpty()) {
						const QString inputPath = args.takeLast();
						args << flagArgs;
						args << inputPath;
					}
				}
			}
		}

		// External (sidecar) subtitles: always absorb all of them into the remux output.
		// Flag changes (if any) are applied per-sidecar inside buildSidecarArgsForRemux.
		const QStringList sidecarArgs = ActionEngine::buildSidecarArgsForRemux(
			streams, flagChangesJson);
		args.append(sidecarArgs);
	}

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

void JobQueue::rescanFile(qint64 fileId, const QString& filePath)
{
	auto& db             = DatabaseManager::instance();
	const auto fileOpt   = db.fileById(fileId);
	if (!fileOpt) return;

	FileRecord fileCopy    = *fileOpt;
	fileCopy.path          = filePath.isEmpty() ? fileCopy.path : filePath;
	const QString ffprobePath = ExternalTools::instance().ffprobePath();

	QThreadPool::globalInstance()->start([this, fileId, fileCopy, ffprobePath]() {
		FfprobeScanner scanner(ffprobePath);
		const FfprobeScanner::ScanResult result = scanner.scanFile(fileCopy.path);
		if (!result.success) {
			qWarning() << "JobQueue::rescanFile: ffprobe failed for" << fileCopy.path
			           << "—" << result.errorMessage;
			return;
		}
		auto& db2 = DatabaseManager::instance();
		FileRecord updated       = result.file;
		updated.scanRunId        = fileCopy.scanRunId;
		if (updated.originalLanguage.isEmpty())
			updated.originalLanguage = fileCopy.originalLanguage;
		(void)db2.upsertFile(updated);
		// Re-discover sidecar subtitles alongside the (possibly renamed) file
		const auto sidecars = ScanWorker::scanSidecarSubtitles(fileCopy.path, result.streams.size());
		auto allStreams      = result.streams;
		allStreams.append(sidecars);
		db2.insertStreams(fileId, allStreams);
		emit fileRescanned(fileId);
	});
}

void JobQueue::onJobFinished(int exitCode, const QString& log, qint64 savedBytes)
{
	const qint64 jobId  = m_currentJobId;
	const qint64 fileId = m_currentFileId;
	// mkvmerge exit codes: 0 = success, 1 = warnings (output still valid), 2 = error
	const bool   ok        = (exitCode == 0 || exitCode == 1);
	const bool   mismatch  = m_currentJob->hasTrackMismatch();

	auto& db = DatabaseManager::instance();

	if (ok && mismatch) {
		// .tmp is still on disk — probe both files and ask user to verify
		const QString tmpPath    = m_currentJob->tmpOutputPath();
		const QString finalPath  = m_currentJob->finalOutputPath();
		const QString srcPath    = m_currentJob->inputFilePath();

		const auto jobOpt  = db.jobById(jobId);
		const auto fileOpt = db.fileById(fileId);
		const QString cmdArgs  = jobOpt  ? jobOpt->commandArgsJson : QString{};
		const QString filename = fileOpt ? fileOpt->filename       : QFileInfo(srcPath).fileName();

		m_reviewJobId    = jobId;
		m_reviewFileId   = fileId;
		m_reviewTmpPath  = tmpPath;
		m_reviewFinalPath = finalPath;
		m_reviewSrcPath  = srcPath;
		m_reviewOrigSize = fileOpt ? fileOpt->sizeBytes : 0;
		m_reviewExitCode = exitCode;

		db.updateJobStatus(jobId, "needs_review", exitCode, log);

		m_currentJob->deleteLater();
		m_currentJob    = nullptr;
		m_currentJobId  = -1;
		m_currentFileId = -1;
		m_running       = false; // pause queue until user decides

		emit jobFinished(jobId, false, 0);

		// Extract the first "track not found" warning line for the dialog header
		static const QRegularExpression warnRe(
			R"(A track with the ID \d+ was requested but not found[^\n]*)",
			QRegularExpression::CaseInsensitiveOption);
		QStringList warnLines;
		auto it = warnRe.globalMatch(log);
		while (it.hasNext())
			warnLines << it.next().captured().trimmed();
		const QString warningText = warnLines.isEmpty()
			? tr("A requested track was not found in the source file.")
			: warnLines.join('\n');

		// Probe both files on a background thread, then surface the review dialog
		const QString ffprobePath = ExternalTools::instance().ffprobePath();
		QThreadPool::globalInstance()->start(
		    [this, jobId, filename, warningText, cmdArgs, tmpPath, srcPath, ffprobePath]() {
			FfprobeScanner scanner(ffprobePath);
			const auto srcResult = scanner.scanFile(srcPath);
			const auto tmpResult = scanner.scanFile(tmpPath);
			const QList<StreamRecord> srcStreams = srcResult.success ? srcResult.streams : QList<StreamRecord>{};
			const QList<StreamRecord> tmpStreams = tmpResult.success ? tmpResult.streams : QList<StreamRecord>{};
			QMetaObject::invokeMethod(this,
			    [this, jobId, filename, warningText, cmdArgs, srcStreams, tmpStreams]() {
				emit reviewRequested(jobId, filename, warningText, cmdArgs, srcStreams, tmpStreams);
			}, Qt::QueuedConnection);
		});
		return;
	}

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

	// Clear the current-job pointer BEFORE emitting jobFinished so that
	// hasActiveJob() returns false inside any synchronous signal handler
	// (e.g. the close-after-pause path in McMainWindow::closeEvent).
	m_currentJob->deleteLater();
	m_currentJob    = nullptr;
	m_currentJobId  = -1;
	m_currentFileId = -1;

	emit jobFinished(jobId, ok, savedBytes);

	// Re-scan the file so the card reflects the new track layout.
	// For non-MKV / ISO jobs the output path differs from the input — update the
	// DB path first so rescanFile() reads the correct (new) file location.
	if (ok && fileId > 0) {
		db.clearStreamForcedRemovals(fileId);

		// Delete sidecar subtitle files queued alongside this remux job
		const auto jobOpt = db.jobById(jobId);
		if (jobOpt && !jobOpt->sidecarDeletionsJson.isEmpty()) {
			const QJsonArray paths = QJsonDocument::fromJson(jobOpt->sidecarDeletionsJson.toUtf8()).array();
			for (const QJsonValue& v : paths) {
				const QString path = v.toString();
				if (!path.isEmpty() && !QFile::remove(path))
					qWarning() << "JobQueue: failed to delete sidecar" << path;
			}
		}

		if (!finalOutput.isEmpty() && finalOutput != originalInput) {
			const QFileInfo fi(finalOutput);
			db.updateFilePath(fileId, finalOutput, fi.fileName());
		}
		rescanFile(fileId, {});
	}

	if (m_running && !m_paused) {
		runNext();
	} else if (m_paused) {
		// The running job finished but the queue is paused, so we won't start
		// the next one. If the queue is also exhausted, transition to idle so
		// the UI doesn't stay stuck in "processing" with m_running=true forever.
		if (DatabaseManager::instance().queuedJobCount() == 0) {
			m_running = false;
			emit allFinished();
		}
	}
}

void JobQueue::commitReview(qint64 jobId)
{
	if (jobId != m_reviewJobId) return;

	const QString tmpPath    = m_reviewTmpPath;
	const QString finalPath  = m_reviewFinalPath;
	const QString srcPath    = m_reviewSrcPath;
	const qint64  fileId     = m_reviewFileId;
	const qint64  origSize   = m_reviewOrigSize;
	const int     exitCode   = m_reviewExitCode;

	m_reviewJobId    = -1;
	m_reviewFileId   = -1;
	m_reviewTmpPath.clear();
	m_reviewFinalPath.clear();
	m_reviewSrcPath.clear();
	m_reviewOrigSize = 0;

	auto& db = DatabaseManager::instance();

	const bool isInPlace = (finalPath == srcPath);
	const QDateTime origCreated = QFileInfo(srcPath).birthTime();

	qint64 savedBytes = 0;
	try {
		const qint64 outputSize = QFileInfo(tmpPath).size();
		std::filesystem::rename(tmpPath.toStdWString(), finalPath.toStdWString());
#ifdef Q_OS_WIN
		preserveCreationTime(finalPath, origCreated);
#endif
		if (!isInPlace)
			QFile::remove(srcPath);
		savedBytes = qMax(0LL, origSize - outputSize);
	} catch (const std::filesystem::filesystem_error& e) {
		qWarning() << "JobQueue::commitReview: rename failed:" << e.what();
		db.updateJobStatus(jobId, "failed", -2);
		emit jobFinished(jobId, false, 0);
		return;
	}

	db.updateJobStatus(jobId, "done", exitCode);
	if (savedBytes > 0) {
		db.updateJobSavedBytes(jobId, savedBytes);
		auto& settings = AppSettings::instance();
		const qint64 prev = settings.value(QStringLiteral("stats/totalReclaimedBytes"), 0LL).toLongLong();
		settings.setValue(QStringLiteral("stats/totalReclaimedBytes"), prev + savedBytes);
	}

	emit jobFinished(jobId, true, savedBytes);

	if (fileId > 0) {
		if (!isInPlace) {
			const QFileInfo fi(finalPath);
			db.updateFilePath(fileId, finalPath, fi.fileName());
		}
		rescanFile(fileId, {});
	}

	if (m_running)
		runNext();
}

void JobQueue::rejectReview(qint64 jobId, bool reanalyze)
{
	if (jobId != m_reviewJobId) return;

	const QString tmpPath = m_reviewTmpPath;
	const qint64  fileId  = m_reviewFileId;

	m_reviewJobId    = -1;
	m_reviewFileId   = -1;
	m_reviewTmpPath.clear();
	m_reviewFinalPath.clear();
	m_reviewSrcPath.clear();
	m_reviewOrigSize = 0;

	QFile::remove(tmpPath);

	auto& db = DatabaseManager::instance();
	if (reanalyze) {
		db.deleteJob(jobId);
		// Rescan so the library reflects current state; user can re-analyze from there
		if (fileId > 0)
			rescanFile(fileId, {});
	} else {
		db.updateJobStatus(jobId, "failed", -3);
	}

	emit jobFinished(jobId, false, 0);

	if (m_running)
		runNext();
}

#ifdef QT_DEBUG
void JobQueue::debugTriggerReview(qint64 jobId)
{
	auto& db = DatabaseManager::instance();
	const auto jobOpt  = db.jobById(jobId);
	if (!jobOpt) return;
	const auto fileOpt = db.fileById(jobOpt->fileId);
	if (!fileOpt) return;

	const QList<StreamRecord> streams = db.streamsForFile(fileOpt->id);
	const QString filename    = fileOpt->filename;
	const QString cmdArgs     = jobOpt->commandArgsJson;
	const QString warningText =
		QStringLiteral("A track with the ID 4 was requested but not found in the file. "
		               "The corresponding option will be ignored.\n"
		               "(This is simulated data — no real .tmp exists.)");

	// Use the same streams for both panels so the user can see the layout.
	// A few streams are faked as "removed" in the output to make the comparison meaningful.
	QList<StreamRecord> fakeOutput;
	for (const StreamRecord& s : streams) {
		if (s.codecType == "subtitle") continue; // pretend subtitles were removed
		fakeOutput << s;
	}

	emit reviewRequested(jobId, filename, warningText, cmdArgs, streams, fakeOutput);
}
#endif // QT_DEBUG

} // namespace Mc
