#include "engine/JobQueue.h"
#include "engine/ActionEngine.h"
#include "engine/RemuxJob.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"
#include "core/StorageGroupSettings.h"
#include "core/ExternalTools.h"
#include "scanner/FfprobeScanner.h"
#include "scanner/ScanWorker.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStorageInfo>
#include <QThreadPool>

#include <algorithm>
#include <filesystem>
#include <optional>

#ifdef Q_OS_WIN
#include <windows.h>
static FILETIME toFileTime(const QDateTime& dt)
{
	const qint64 ns100 = (dt.toMSecsSinceEpoch() + Q_INT64_C(11644473600000)) * 10000;
	FILETIME ft;
	ft.dwLowDateTime  = static_cast<DWORD>(ns100 & 0xFFFFFFFF);
	ft.dwHighDateTime = static_cast<DWORD>((ns100 >> 32) & 0xFFFFFFFF);
	return ft;
}
static void preserveTimestamps(const QString& target, const QDateTime& origCreated, const QDateTime& origModified)
{
	HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(target.utf16()),
	                       FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	FILETIME ftCreated  = origCreated.isValid()  ? toFileTime(origCreated)  : FILETIME{};
	FILETIME ftModified = origModified.isValid() ? toFileTime(origModified) : FILETIME{};
	SetFileTime(h,
	            origCreated.isValid()  ? &ftCreated  : nullptr,
	            nullptr,
	            origModified.isValid() ? &ftModified : nullptr);
	CloseHandle(h);
}
#endif

namespace Mc {

// A queued job's DB-frozen estimated_saved_bytes was computed at proposal time —
// if the estimate formula has changed since (see calibration/estimate tuning), that
// value goes stale while the job panel keeps showing a freshly recomputed number
// (McJobListModel::sortEntriesByDisplaySavings). Recomputing the same live estimate
// here keeps the queue's actual pick order matching what "Largest savings first"
// shows on screen, instead of drifting apart whenever the formula moves on.
// Live-recomputed estimate for a single job — shared by the sort below and by
// startJob() (which needs the current job's estimate for goal-progress tracking).
static qint64 estimateJobSavingBytesFromData(const FileRecord& file,
                                              const QList<StreamRecord>& allStreams,
                                              const QString& commandArgsJson)
{
	const QList<StreamRecord> keptStreams =
		ActionEngine::computeKeptStreams(allStreams, commandArgsJson);
	QSet<int> keptIdx;
	for (const auto& s : keptStreams) keptIdx.insert(s.streamIndex);
	QSet<int> removed;
	for (const auto& s : allStreams)
		if (!keptIdx.contains(s.streamIndex)) removed.insert(s.streamIndex);

	return estimateSavingBytes(allStreams, removed, file.sizeBytes, file.durationSec);
}

static qint64 estimateJobSavingBytes(const JobRecord& job)
{
	auto& db = DatabaseManager::instance();
	const auto fileOpt = db.fileById(job.fileId);
	if (!fileOpt) return 0;

	return estimateJobSavingBytesFromData(*fileOpt, db.streamsForFile(job.fileId), job.commandArgsJson);
}

// Called on every dispatchJobs() pass while sorting by live savings (or while a
// byte goal is active) — with a deep queue this used to do two synchronous DB
// round trips (fileById + streamsForFile) PER QUEUED JOB, on the UI thread, since
// JobQueue lives on the main thread. With hundreds of queued jobs that's easily
// 1000+ separate SQL round trips every time a job starts or finishes, which is
// exactly when jobs are processing back-to-back — freezing card selection for
// seconds. Batch-fetching all files/streams up front turns that into two queries
// (chunked internally) regardless of queue depth.
static void sortQueuedJobsByLiveSavings(QList<JobRecord>& jobs)
{
	if (jobs.size() < 2) return;

	QList<qint64> fileIds;
	fileIds.reserve(jobs.size());
	for (const auto& j : jobs) fileIds << j.fileId;

	auto& db = DatabaseManager::instance();
	const QHash<qint64, FileRecord> files          = db.filesByIds(fileIds);
	const QHash<qint64, QList<StreamRecord>> streamsByFile = db.streamsForFiles(fileIds);

	QList<qint64> saved(jobs.size());
	for (int i = 0; i < jobs.size(); ++i) {
		const auto fileIt = files.constFind(jobs[i].fileId);
		saved[i] = (fileIt == files.constEnd())
		    ? 0
		    : estimateJobSavingBytesFromData(*fileIt, streamsByFile.value(jobs[i].fileId),
		                                      jobs[i].commandArgsJson);
	}

	QList<int> perm(jobs.size());
	for (int i = 0; i < jobs.size(); ++i) perm[i] = i;
	std::stable_sort(perm.begin(), perm.end(), [&](int a, int b) { return saved[a] > saved[b]; });

	QList<JobRecord> sorted;
	sorted.reserve(jobs.size());
	for (int i : perm) sorted.append(std::move(jobs[i]));
	jobs = std::move(sorted);
}

JobQueue::JobQueue(QObject* parent)
	: QObject(parent)
{
	connect(this, &JobQueue::jobFinished, this, &JobQueue::accumulateGoalProgress);
}

bool JobQueue::isGroupBusy(int storageGroup) const
{
	const auto it = m_runningByGroup.constFind(storageGroup);
	if (it == m_runningByGroup.cend())
		return false;
	return it->remux != nullptr || it->finishBusy;
}

bool JobQueue::hasActiveJob() const
{
	if (m_reviewJobId >= 0)
		return true;
	for (auto it = m_runningByGroup.cbegin(); it != m_runningByGroup.cend(); ++it) {
		if (it->remux || it->finishBusy)
			return true;
	}
	return false;
}

bool JobQueue::canRunJobImmediately(qint64 jobId) const
{
	if (m_reviewJobId >= 0)
		return false;
	const auto jobOpt = DatabaseManager::instance().jobById(jobId);
	if (!jobOpt)
		return false;
	return !isGroupBusy(StorageGroupSettings::groupForFileId(jobOpt->fileId));
}

qint64 JobQueue::currentJobEstimatedSavings() const
{
	qint64 total = 0;
	for (auto it = m_runningByGroup.cbegin(); it != m_runningByGroup.cend(); ++it) {
		if (it->remux)
			total += it->estimatedSavings;
	}
	return total;
}

void JobQueue::occupySlot(int storageGroup, qint64 jobId, qint64 fileId, qint64 estimatedSavings)
{
	RunningJobSlot& slot    = m_runningByGroup[storageGroup];
	slot.jobId              = jobId;
	slot.fileId             = fileId;
	slot.estimatedSavings   = estimatedSavings;
}

void JobQueue::releaseSlot(int storageGroup)
{
	m_runningByGroup.remove(storageGroup);
}

JobQueue::RunningJobSlot* JobQueue::slotForJobId(qint64 jobId)
{
	for (auto it = m_runningByGroup.begin(); it != m_runningByGroup.end(); ++it) {
		if (it->jobId == jobId)
			return &(*it);
	}
	return nullptr;
}

void JobQueue::start()
{
	m_paused  = false;
	m_running = true;
	dispatchJobs();
}

void JobQueue::pause()
{
	m_paused = true;
	// Current job keeps running; we just won't start the next one
}

void JobQueue::cancel()
{
	QList<qint64> runningJobIds;
	runningJobIds.reserve(m_runningByGroup.size());
	for (auto it = m_runningByGroup.cbegin(); it != m_runningByGroup.cend(); ++it) {
		if (it->jobId > 0)
			runningJobIds << it->jobId;
	}

	m_paused  = false;
	m_running = false;
	m_goalBytes         = 0;
	m_goalSavedBytes    = 0;
	m_goalJobsCompleted = 0;

	if (!runningJobIds.isEmpty()) {
		for (qint64 jobId : runningJobIds)
			m_cancelledJobIds.insert(jobId);
		DatabaseManager::instance().requeueRunningJobs(runningJobIds);
		for (qint64 jobId : runningJobIds)
			emit jobRequeued(jobId);
	}

	for (auto it = m_runningByGroup.begin(); it != m_runningByGroup.end(); ++it) {
		if (it->remux)
			it->remux->cancel();
		if (it->propEdit && it->propEdit->state() != QProcess::NotRunning)
			it->propEdit->kill();
	}
	m_runningByGroup.clear();

	if (!runningJobIds.isEmpty())
		emit allFinished();
}

void JobQueue::cancelJob(qint64 jobId)
{
	int storageGroup = -1;
	for (auto it = m_runningByGroup.cbegin(); it != m_runningByGroup.cend(); ++it) {
		if (it->jobId == jobId) { storageGroup = it.key(); break; }
	}
	if (storageGroup < 0)
		return; // not currently running — nothing to cancel

	m_cancelledJobIds.insert(jobId);
	DatabaseManager::instance().requeueRunningJobs({ jobId });
	emit jobRequeued(jobId);

	RunningJobSlot& slot = m_runningByGroup[storageGroup];
	if (slot.remux)
		slot.remux->cancel();
	if (slot.propEdit && slot.propEdit->state() != QProcess::NotRunning)
		slot.propEdit->kill();
	releaseSlot(storageGroup);

	// The storage group this job occupied is free now — let another queued job
	// for that group (or any other) pick up immediately instead of waiting for
	// the next unrelated job event to trigger a dispatch. Exclude this job
	// itself: it just went back to "queued", and without the exclusion it would
	// be the only/best candidate for its now-idle group and get restarted in the
	// very same tick, making Cancel look like it did nothing.
	if (m_running && !m_paused)
		dispatchJobs(jobId);
}

void JobQueue::startWithGoal(qint64 goalBytes)
{
	m_goalBytes         = goalBytes;
	m_goalSavedBytes    = 0;
	m_goalJobsCompleted = 0;
	m_goalTimer.start();
	start();
}

void JobQueue::setGoalBytes(qint64 goalBytes)
{
	// Only raises the target — jobs already picked toward the old target can't be
	// un-started, so lowering it wouldn't actually change anything mid-run.
	if (goalBytes > m_goalBytes) {
		m_goalBytes = goalBytes;
		emit goalProgressChanged(m_goalSavedBytes, m_goalBytes);
	}
}

void JobQueue::accumulateGoalProgress(qint64 /*jobId*/, bool success, qint64 savedBytes)
{
	if (m_goalBytes <= 0 || !success) return;
	m_goalSavedBytes += savedBytes;
	++m_goalJobsCompleted;
	emit goalProgressChanged(m_goalSavedBytes, m_goalBytes);
}

void JobQueue::runJob(qint64 jobId)
{
	auto& db = DatabaseManager::instance();
	const auto jobOpt = db.jobById(jobId);
	if (!jobOpt) return;

	if (!canRunJobImmediately(jobId))
		return;

	if (jobOpt->status == QLatin1String("proposed"))
		db.promoteJobsToQueued({jobId});
	else if (jobOpt->status != QLatin1String("queued"))
		return;

	m_paused  = false;
	m_running = true;
	(void)tryStartJob(*jobOpt);
}

void JobQueue::dispatchJobs(qint64 excludeJobId)
{
	if (!m_running || m_paused) return;
	if (m_reviewJobId >= 0) return;

	if (m_goalBytes > 0 && m_goalSavedBytes >= m_goalBytes) {
		const qint64 elapsedMs = m_goalTimer.isValid() ? m_goalTimer.elapsed() : 0;
		emit goalReached(m_goalSavedBytes, m_goalBytes, m_goalJobsCompleted, elapsedMs);

		m_running           = false;
		m_goalBytes         = 0;
		m_goalSavedBytes    = 0;
		m_goalJobsCompleted = 0;
		emit allFinished();
		return;
	}

	auto jobs = DatabaseManager::instance().queuedJobs(m_sortMode);

	if (jobs.isEmpty()) {
		if (!hasActiveJob()) {
			m_running = false;
			emit allFinished();
		}
		return;
	}

	if (m_sortMode == JobSortMode::LargestSavingsFirst || m_goalBytes > 0)
		sortQueuedJobsByLiveSavings(jobs);

	QSet<int> startedThisRound;
	for (const JobRecord& job : jobs) {
		if (job.id == excludeJobId)
			continue;
		const int group = StorageGroupSettings::groupForFileId(job.fileId);
		if (isGroupBusy(group) || startedThisRound.contains(group))
			continue;
		if (tryStartJob(job))
			startedThisRound.insert(group);
	}
}

bool JobQueue::tryStartJob(const JobRecord& job)
{
	auto& db = DatabaseManager::instance();

	const qint64  jobId           = job.id;
	const qint64  fileId          = job.fileId;
	const QString jobType         = job.jobType;
	const QString commandArgsJson = job.commandArgsJson;
	const QString flagChangesJson = job.flagChangesJson;
	const QString descriptionText = job.descriptionText;
	const int     storageGroup    = StorageGroupSettings::groupForFileId(fileId);

	if (isGroupBusy(storageGroup))
		return false;

	const qint64 estimatedSavings = estimateJobSavingBytes(job);

	// tag_edit jobs have no commandArgsJson — that's expected; other types must have it.
	const bool isTagEdit = (jobType == QLatin1String("tag_edit"));
	if (!isTagEdit && commandArgsJson.isEmpty()) {
		qWarning() << "JobQueue: job" << jobId << "has empty command_args_json — marking failed";
		db.updateJobStatus(jobId, "failed", -1);
		emit jobFinished(jobId, false, 0);
		if (m_running && !m_paused) dispatchJobs();
		return false;
	}

	// ── Flag-only edit via mkvpropedit (+ optional sidecar file deletions / renames) ──
	if (isTagEdit) {
		const QString sidecarDeletionsJson = job.sidecarDeletionsJson;
		if (flagChangesJson.isEmpty() && sidecarDeletionsJson.isEmpty()) {
			db.updateJobStatus(jobId, "done", 0);
			emit jobStarted(jobId);
			emit jobFinished(jobId, true, 0);
			if (m_running && !m_paused) dispatchJobs();
			return true;
		}

		occupySlot(storageGroup, jobId, fileId, 0);
		m_runningByGroup[storageGroup].finishBusy = true;

		db.updateJobStatus(jobId, "running");
		emit jobStarted(jobId);

		// Load streams to separate internal (mkvpropedit) vs external (rename) changes.
		const QList<StreamRecord> allStreams = fileId > 0
			? db.streamsForFile(fileId) : QList<StreamRecord>{};
		const QString internalFlags = ActionEngine::filterInternalFlagChanges(
			flagChangesJson, allStreams);

		// Run mkvpropedit for internal (container) flag changes only, asynchronously —
		// finishTagEditJob() picks up from here once it (if any) has exited.
		if (internalFlags.isEmpty()) {
			finishTagEditJob(storageGroup, jobId, fileId, true, QString(), sidecarDeletionsJson,
			                  flagChangesJson, allStreams);
			return true;
		}

		const auto fileOpt = db.fileById(fileId);
		if (!fileOpt) {
			db.updateJobStatus(jobId, "failed", -1);
			releaseSlot(storageGroup);
			emit jobFinished(jobId, false, 0);
			if (m_running && !m_paused) dispatchJobs();
			return true;
		}

		const QStringList args        = ActionEngine::buildPropEditArgs(fileOpt->path, internalFlags);
		const QString mkvpropeditPath = ExternalTools::instance().mkvpropeditPath();

		auto* proc = new QProcess(this);
		m_runningByGroup[storageGroup].propEdit = proc;
		proc->setProcessChannelMode(QProcess::MergedChannels);
		connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
		        this, [this, proc, jobId, fileId, storageGroup, sidecarDeletionsJson,
		               flagChangesJson, allStreams]
		              (int exitCode, QProcess::ExitStatus status) {
			const bool ok = (status == QProcess::NormalExit && exitCode == 0);
			const QByteArray rawLog = proc->readAllStandardOutput();
			const QString log = rawLog.isEmpty()
				? (ok ? QStringLiteral("mkvpropedit completed (exit 0)")
				      : QStringLiteral("mkvpropedit failed (exit %1)").arg(exitCode))
				: QString::fromLocal8Bit(rawLog);
			proc->deleteLater();
			finishTagEditJob(storageGroup, jobId, fileId, ok, log, sidecarDeletionsJson,
			                 flagChangesJson, allStreams);
		});
		proc->start(mkvpropeditPath, args);
		return true;
	}

	// ── Remux job via mkvmerge ───────────────────────────────────────────────

	// Disk space guard — need at least as much free space as the source file.
	// Network drives (UNC paths, mapped drives) may return isValid()=false or
	// report inaccurate values; skip the check silently in that case.
	std::optional<FileRecord> fileOpt;
	if (fileId > 0) {
		fileOpt = db.fileById(fileId);
		if (fileOpt && fileOpt->sizeBytes > 0) {
			const QStorageInfo storage(QFileInfo(fileOpt->path).absolutePath());
			if (!storage.isValid() || storage.isReadOnly()) {
				// Network drive or read-only volume — can't query free space, proceed.
			} else if (storage.bytesAvailable() < fileOpt->sizeBytes) {
				const double needed = fileOpt->sizeBytes / 1073741824.0;
				const double avail  = storage.bytesAvailable() / 1073741824.0;
				emit warning(QStringLiteral(
				    "Not enough disk space for \"%1\" — need %2 GB, have %3 GB free. Skipping for now.")
				    .arg(fileOpt->filename)
				    .arg(needed, 0, 'f', 2)
				    .arg(avail,  0, 'f', 2));
				return false;
			}
		}
	}

	QStringList args;
	const QJsonArray arr = QJsonDocument::fromJson(commandArgsJson.toUtf8()).array();
	for (const auto& v : arr) args << v.toString();

	// Inject flag changes and sidecar subtitle files into the mkvmerge command.
	// Capture the source path before sidecar args are appended — each sidecar block
	// ends with its file path, which would otherwise corrupt args.last().
	QString sourcePath;
	if (!args.isEmpty()) {
		const QList<StreamRecord> streams = db.streamsForFile(fileId);

		// Refresh the "before" snapshot from the current live streams right before
		// this remux actually runs — a proposed job can sit queued long enough for
		// the on-disk track/sidecar layout to change, so a snapshot frozen at
		// proposal time would go stale. This is what the "done" card renders from.
		db.updateJobOriginalStreams(jobId, ActionEngine::serializeStreamSnapshot(streams));

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

		// args.last() is the primary source file at this point — record it before
		// sidecar args push their own file paths onto the end of the list.
		sourcePath = args.last();

		// External (sidecar) subtitles: absorb all of them into the remux output,
		// unless the user has opted out. Flag changes (if any) are applied
		// per-sidecar inside buildSidecarArgsForRemux.
		if (m_mergeSidecarSubtitles) {
			const QStringList sidecarArgs = ActionEngine::buildSidecarArgsForRemux(
				streams, flagChangesJson);
			args.append(sidecarArgs);

			// Record exactly which sidecar files are being absorbed so they can be
			// deleted from disk once the merge actually succeeds.
			QJsonArray sidecarPaths;
			for (const StreamRecord& s : streams)
				if (s.isExternal && !s.externalPath.isEmpty()) sidecarPaths.append(s.externalPath);
			if (!sidecarPaths.isEmpty()) {
				db.updateJobSidecarDeletions(jobId,
					QString::fromUtf8(QJsonDocument(sidecarPaths).toJson(QJsonDocument::Compact)));
			}
		}
	}

	// Local staging — redirect mkvmerge's -o target to a local folder so the NAS
	// (or whatever volume the source lives on) is only read, not written to, while
	// muxing. The finished file is copied back to its real destination afterward
	// (see RemuxJob's staged-finish path). Falls back to muxing in place whenever
	// staging is off, unconfigured, or there isn't enough local free space —
	// this never blocks the queue.
	QString finalOutputPathOverride;
	if (m_useLocalStaging && !m_localStagingDir.isEmpty()
	        && args.size() >= 2 && args.first() == QLatin1String("-o")) {
		const QString proposedOutput = args.at(1);
		const QString realFinalPath  = proposedOutput.endsWith(QLatin1String(".tmp"))
			? proposedOutput.left(proposedOutput.length() - 4)
			: proposedOutput;

		const QStorageInfo stagingStorage(m_localStagingDir);
		const qint64 neededBytes = fileOpt ? fileOpt->sizeBytes : 0;
		if (stagingStorage.isValid() && !stagingStorage.isReadOnly()
		        && (neededBytes <= 0 || stagingStorage.bytesAvailable() >= neededBytes)) {
			const QString localTmp = QDir(m_localStagingDir).filePath(
				QString::number(jobId) + QLatin1Char('_') + QFileInfo(realFinalPath).fileName()
				+ QStringLiteral(".tmp"));
			args[1] = localTmp;
			finalOutputPathOverride = realFinalPath;
		} else {
			emit warning(QStringLiteral(
			    "Not enough space in the local staging folder for \"%1\" — muxing in place instead.")
			    .arg(QFileInfo(realFinalPath).fileName()));
		}
	}

	const QString mkvmergePath = ExternalTools::instance().mkvmergePath();
	occupySlot(storageGroup, jobId, fileId, estimatedSavings);
	RunningJobSlot& slot = m_runningByGroup[storageGroup];
	slot.remux = new RemuxJob(jobId, mkvmergePath, args, sourcePath, descriptionText,
	                          m_writeJobLog, finalOutputPathOverride, this);

	connect(slot.remux, &RemuxJob::finished, this,
	        [this, storageGroup, jobId, fileId](int exitCode, const QString& log, qint64 savedBytes) {
		onRemuxJobFinished(storageGroup, jobId, fileId, exitCode, log, savedBytes);
	});
	connect(slot.remux, &RemuxJob::progressChanged, this,
	        [this, jobId](int pct, qint64 outputBytes) {
		emit progressChanged(jobId, pct);
		emit outputSizeChanged(jobId, outputBytes);
	});
	connect(slot.remux, &RemuxJob::phaseChanged, this,
	        [this, jobId](const QString& label) {
		emit phaseChanged(jobId, label);
	});

	db.updateJobStatus(jobId, "running");
	emit jobStarted(jobId);

	slot.remux->run();
	return true;
}

bool JobQueue::consumeAbortedJob(qint64 jobId, int storageGroup)
{
	const bool aborted = m_cancelledJobIds.remove(jobId)
	    || [&] {
		const auto opt = DatabaseManager::instance().jobById(jobId);
		// cancel() requeues runners before mkvmerge/mkvpropedit exits — a late
		// completion handler must not overwrite that back to failed/done.
		return opt && opt->status == QLatin1String("queued");
	}();
	if (!aborted)
		return false;

	// Match on jobId, not just storageGroup — cancelJob() frees this slot and
	// immediately redispatches, so by the time the killed process's async
	// finished()/QProcess::finished signal actually arrives, a different job may
	// already be occupying this same storage-group slot. Tearing that down would
	// cancel a job nobody asked to cancel.
	if (auto it = m_runningByGroup.find(storageGroup);
	        it != m_runningByGroup.end() && it->jobId == jobId) {
		if (it->remux)
			it->remux->deleteLater();
		if (it->propEdit) {
			if (it->propEdit->state() != QProcess::NotRunning)
				it->propEdit->kill();
			it->propEdit->deleteLater();
		}
		releaseSlot(storageGroup);
	} else if (auto* senderRemux = qobject_cast<RemuxJob*>(sender())) {
		senderRemux->deleteLater();
	}
	return true;
}

void JobQueue::finishTagEditJob(int storageGroup, qint64 jobId, qint64 fileId, bool propEditOk,
                                 const QString& log, const QString& sidecarDeletionsJson,
                                 const QString& flagChangesJson,
                                 const QList<StreamRecord>& allStreams)
{
	// Sidecar delete/rename and the DB status update are filesystem/SQL work with no
	// bearing on this object's own state — run them off the UI thread, same pattern
	// as the remux-completion path in RemuxJob::onProcessFinished().
	QThreadPool::globalInstance()->start(
	    [this, storageGroup, jobId, fileId, propEditOk, log, sidecarDeletionsJson,
	     flagChangesJson, allStreams]() {
		bool ok = propEditOk;

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

		// Status write happens on the UI thread below, after the cancellation check.
		QMetaObject::invokeMethod(this, [this, storageGroup, jobId, fileId, ok, log] {
			if (consumeAbortedJob(jobId, storageGroup))
				return;
			DatabaseManager::instance().updateJobStatus(jobId, ok ? "done" : "failed", ok ? 0 : -1, log);
			releaseSlot(storageGroup);
			emit jobFinished(jobId, ok, 0);

			if (ok && fileId > 0)
				rescanFile(fileId, {});  // picks up renamed sidecars automatically

			if (m_running && !m_paused) dispatchJobs();
		}, Qt::QueuedConnection);
	});
}

void JobQueue::rescanFile(qint64 fileId, const QString& filePath, bool triggerReanalysis)
{
	auto& db             = DatabaseManager::instance();
	const auto fileOpt   = db.fileById(fileId);
	if (!fileOpt) return;

	FileRecord fileCopy    = *fileOpt;
	fileCopy.path          = filePath.isEmpty() ? fileCopy.path : filePath;
	const QString ffprobePath = ExternalTools::instance().ffprobePath();

	QThreadPool::globalInstance()->start([this, fileId, fileCopy, ffprobePath, triggerReanalysis]() {
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
		const auto sidecars = ScanWorker::scanSidecarSubtitles(
		    fileCopy.path, ScanWorker::nextSidecarStreamIndex(result.streams));
		auto allStreams      = result.streams;
		allStreams.append(sidecars);
		db2.insertStreams(fileId, allStreams);
		// Any pending job built before this rescan used the old stream layout and
		// will cause a track-mismatch error if run now — delete it.
		db2.deletePendingJobsForFile(fileId);
		emit fileRescanned(fileId);
		if (triggerReanalysis)
			emit fileNeedsReanalysis(fileId);
	});
}

void JobQueue::onRemuxJobFinished(int storageGroup, qint64 jobId, qint64 fileId,
                                  int exitCode, const QString& log, qint64 savedBytes)
{
	if (consumeAbortedJob(jobId, storageGroup))
		return;

	RunningJobSlot* slot = m_runningByGroup.contains(storageGroup)
	    ? &m_runningByGroup[storageGroup] : nullptr;
	RemuxJob* remux = slot ? slot->remux : nullptr;
	if (!remux) {
		if (auto* senderRemux = qobject_cast<RemuxJob*>(sender()))
			remux = senderRemux;
	}
	if (!remux)
		return;

	// mkvmerge exit codes: 0 = success, 1 = warnings (output still valid), 2 = error
	const bool ok       = (exitCode == 0 || exitCode == 1);
	const bool mismatch = remux->hasTrackMismatch();

	auto& db = DatabaseManager::instance();

	if (ok && mismatch) {
		const QString tmpPath    = remux->tmpOutputPath();
		const QString finalPath  = remux->finalOutputPath();
		const QString srcPath    = remux->inputFilePath();

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
		m_reviewWasStaged = remux->wasStaged();

		db.updateJobStatus(jobId, "needs_review", exitCode, log);

		remux->deleteLater();
		releaseSlot(storageGroup);
		m_running = false; // pause queue until user decides

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

		// Source streams: use the snapshot stored at job-creation time so the source
		// card is always correct even when the file was modified by an earlier job run.
		const QList<StreamRecord> srcStreams = (jobOpt && !jobOpt->originalStreamsJson.isEmpty())
			? ActionEngine::deserializeStreamSnapshot(jobOpt->originalStreamsJson)
			: QList<StreamRecord>{};

		// Probe only the .tmp output file on a background thread, then open the dialog
		const QString ffprobePath = ExternalTools::instance().ffprobePath();
		QThreadPool::globalInstance()->start(
		    [this, jobId, filename, warningText, cmdArgs, tmpPath, srcStreams, ffprobePath]() {
			FfprobeScanner scanner(ffprobePath);
			const auto tmpResult = scanner.scanFile(tmpPath);
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
		db.updateCalibrationFromJob(jobId);
		auto& settings = AppSettings::instance();
		const qint64 prev = settings.value(QStringLiteral("stats/totalReclaimedBytes"), 0LL).toLongLong();
		settings.setValue(QStringLiteral("stats/totalReclaimedBytes"), prev + savedBytes);
	}

	const QString finalOutput   = remux->finalOutputPath();
	const QString originalInput = remux->inputFilePath();

	remux->deleteLater();
	releaseSlot(storageGroup);

	emit jobFinished(jobId, ok, savedBytes);

	// Re-scan the file so the card reflects the new track layout.
	// For non-MKV / ISO jobs the output path differs from the input — update the
	// DB path first so rescanFile() reads the correct (new) file location.
	if (ok && fileId > 0) {
		db.clearStreamForcedRemovals(fileId);

		deleteMergedSidecars(jobId);

		if (!finalOutput.isEmpty() && finalOutput != originalInput) {
			const QFileInfo fi(finalOutput);
			db.updateFilePath(fileId, finalOutput, fi.fileName());
		}
		rescanFile(fileId, {});
	}

	if (m_running && !m_paused) {
		dispatchJobs();
	} else if (m_paused) {
		if (DatabaseManager::instance().queuedJobCount() == 0 && !hasActiveJob()) {
			m_running = false;
			emit allFinished();
		}
	}
}

void JobQueue::deleteMergedSidecars(qint64 jobId)
{
	auto& db = DatabaseManager::instance();
	const auto jobOpt = db.jobById(jobId);
	if (!jobOpt || jobOpt->sidecarDeletionsJson.isEmpty()) return;

	const QJsonArray paths = QJsonDocument::fromJson(jobOpt->sidecarDeletionsJson.toUtf8()).array();
	QSet<QString> deletedPaths;
	for (const QJsonValue& v : paths) {
		const QString path = v.toString();
		if (path.isEmpty()) continue;
		deletedPaths.insert(path);
		if (!QFile::remove(path))
			qWarning() << "JobQueue: failed to delete sidecar" << path;
	}

	// The job's frozen stream snapshot still points these at the sidecar file we
	// just deleted. Clear the path (but keep "ext" set — computeKeptStreams()
	// always treats external streams as kept, since mkvmerge's --subtitle-tracks
	// filter never applies to them) so the "done" card stops showing the sidecar
	// marker for a file that no longer exists standalone, while still rendering
	// the track as kept rather than struck through as removed.
	if (jobOpt->originalStreamsJson.isEmpty()) return;
	QJsonArray streamsArr = QJsonDocument::fromJson(jobOpt->originalStreamsJson.toUtf8()).array();
	bool changed = false;
	for (int i = 0; i < streamsArr.size(); ++i) {
		QJsonObject o = streamsArr[i].toObject();
		if (o.value(QLatin1String("ext")).toBool()
		        && deletedPaths.contains(o.value(QLatin1String("extPath")).toString())) {
			o[QLatin1String("extPath")] = QString();
			streamsArr[i] = o;
			changed = true;
		}
	}
	if (changed) {
		db.updateJobOriginalStreams(jobId, QJsonDocument(streamsArr).toJson(QJsonDocument::Compact));
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
	const bool    wasStaged  = m_reviewWasStaged;

	m_reviewJobId    = -1;
	m_reviewFileId   = -1;
	m_reviewTmpPath.clear();
	m_reviewFinalPath.clear();
	m_reviewSrcPath.clear();
	m_reviewOrigSize = 0;
	m_reviewWasStaged = false;

	const int storageGroup = StorageGroupSettings::groupForFileId(fileId);
	occupySlot(storageGroup, jobId, fileId, 0);
	m_runningByGroup[storageGroup].finishBusy = true;

	// Rename/delete + the DB status update are filesystem/SQL work with no bearing
	// on this object's own state — run them off the UI thread, same pattern as the
	// remux-completion path in RemuxJob::onProcessFinished().
	QThreadPool::globalInstance()->start(
	    [this, jobId, fileId, tmpPath, finalPath, srcPath, origSize, exitCode, wasStaged, storageGroup]() {
		auto& db = DatabaseManager::instance();

		const bool isInPlace = (finalPath == srcPath);
		const QFileInfo origFi(srcPath);
		const QDateTime origCreated  = origFi.birthTime();
		const QDateTime origModified = origFi.lastModified();

		qint64 savedBytes   = 0;
		bool   renameFailed = false;
		const qint64 outputSize = QFileInfo(tmpPath).size();
		if (wasStaged) {
			// tmpPath is a local staging file, not next to finalPath — copy it to
			// the NAS (same convention as RemuxJob's staged-finish path: a
			// same-directory tmp beside the real destination, then an atomic
			// same-volume rename), same pattern as RemuxJob::onProcessFinished.
			const QFileInfo finalFi(finalPath);
			const QString remoteTmp = finalFi.absolutePath() + QLatin1Char('/')
			                          + finalFi.fileName() + QStringLiteral(".tmp");

			QMetaObject::invokeMethod(this, [this, jobId] {
				emit phaseChanged(jobId, tr("Copying to NAS"));
			}, Qt::QueuedConnection);

			const bool copyOk = RemuxJob::copyFileWithProgress(
			    tmpPath, remoteTmp, outputSize,
			    [this, jobId](int pct, qint64 bytes) {
				QMetaObject::invokeMethod(this, [this, jobId, pct, bytes] {
					emit progressChanged(jobId, pct);
					emit outputSizeChanged(jobId, bytes);
				}, Qt::QueuedConnection);
			    });

			if (copyOk) {
				try {
					std::filesystem::rename(remoteTmp.toStdWString(), finalPath.toStdWString());
#ifdef Q_OS_WIN
					preserveTimestamps(finalPath, origCreated, origModified);
#endif
					QFile::remove(tmpPath); // drop the local staged tmp
					if (!isInPlace)
						QFile::remove(srcPath);
					savedBytes = qMax(0LL, origSize - outputSize);
				} catch (const std::filesystem::filesystem_error& e) {
					qWarning() << "JobQueue::commitReview: staged rename failed:" << e.what();
					db.updateJobStatus(jobId, "failed", -2);
					renameFailed = true;
				}
			} else {
				QFile::remove(remoteTmp); // best-effort cleanup of a partial copy
				qWarning() << "JobQueue::commitReview: copy to NAS destination failed for" << finalPath;
				db.updateJobStatus(jobId, "failed", -2);
				renameFailed = true;
			}
		} else {
		try {
			std::filesystem::rename(tmpPath.toStdWString(), finalPath.toStdWString());
#ifdef Q_OS_WIN
			preserveTimestamps(finalPath, origCreated, origModified);
#endif
			if (!isInPlace)
				QFile::remove(srcPath);
			savedBytes = qMax(0LL, origSize - outputSize);
		} catch (const std::filesystem::filesystem_error& e) {
			qWarning() << "JobQueue::commitReview: rename failed:" << e.what();
			db.updateJobStatus(jobId, "failed", -2);
			renameFailed = true;
		}
		}

		if (renameFailed) {
			QMetaObject::invokeMethod(this, [this, storageGroup, jobId] {
				releaseSlot(storageGroup);
				emit jobFinished(jobId, false, 0);
			}, Qt::QueuedConnection);
			return;
		}

		db.updateJobStatus(jobId, "done", exitCode);
		if (savedBytes > 0) {
			db.updateJobSavedBytes(jobId, savedBytes);
			db.updateCalibrationFromJob(jobId);
		}

		QMetaObject::invokeMethod(this, [this, storageGroup, jobId, fileId, finalPath, isInPlace, savedBytes] {
			releaseSlot(storageGroup);

			if (savedBytes > 0) {
				auto& settings = AppSettings::instance();
				const qint64 prev = settings.value(QStringLiteral("stats/totalReclaimedBytes"), 0LL).toLongLong();
				settings.setValue(QStringLiteral("stats/totalReclaimedBytes"), prev + savedBytes);
			}

			emit jobFinished(jobId, true, savedBytes);

			if (fileId > 0) {
				deleteMergedSidecars(jobId);
				if (!isInPlace) {
					const QFileInfo fi(finalPath);
					DatabaseManager::instance().updateFilePath(fileId, finalPath, fi.fileName());
				}
				rescanFile(fileId, {});
			}

			if (m_running)
				dispatchJobs();
		}, Qt::QueuedConnection);
	});
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
		if (fileId > 0)
			rescanFile(fileId, {}, /*triggerReanalysis=*/true);
	} else {
		db.updateJobStatus(jobId, "failed", -3);
	}

	emit jobFinished(jobId, false, 0);

	if (m_running)
		dispatchJobs();
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
