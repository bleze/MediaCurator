#include "engine/JobQueue.h"
#include "engine/ActionEngine.h"
#include "engine/RemuxJob.h"
#include "engine/SubtitleManager.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"
#include "core/StorageGroupSettings.h"
#include "core/ExternalTools.h"
#include "scanner/FfprobeScanner.h"
#include "scanner/ScanWorker.h"

#include <QCoreApplication>
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
#include <QPointer>
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
// Same idea as preserveTimestamps() above, but for the containing folder —
// FILE_FLAG_BACKUP_SEMANTICS is needed to open a directory handle at all.
// Restores the folder's timestamp to what it was before this job's subtitle
// prefetch + remux started, so an automated maintenance pass (download a
// sidecar, mux it in, delete the source) doesn't bubble the folder up as
// "newest" in media libraries that sort by Date Modified.
static void preserveDirTimestamps(const QString& dirPath, const QDateTime& origCreated, const QDateTime& origModified)
{
	HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(dirPath.utf16()),
	                       FILE_WRITE_ATTRIBUTES,
	                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                       nullptr, OPEN_EXISTING,
	                       FILE_FLAG_BACKUP_SEMANTICS, nullptr);
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

// How long tryStartJob() will wait for a missing subtitle to download before
// giving up and muxing without it — bounded deliberately short so an
// unresponsive OpenSubtitles never turns into a long stall of the queue.
static constexpr int kSubtitlePrefetchTimeoutMs = 6000;

// A queued job's DB-frozen estimated_saved_bytes was computed at proposal time —
// if the estimate formula has changed since (see calibration/estimate tuning), or
// the file was rescanned while it sat in the queue, that value goes stale while the
// job panel keeps showing a freshly recomputed number (McJobListModel's card badge,
// which always recomputes live). Recomputing the same live estimate here keeps the
// queue's actual pick order matching what "Largest savings first" shows on screen,
// instead of drifting apart whenever the formula (or the file's stream data) moves on.
// tryStartJob() also re-freezes this same recomputed value into the DB right before
// the job actually runs (see estimateJobSavingWithBreakdown() below), so the "View
// Log" dialog's "Estimated" figure reflects the data/formula at run time rather than
// whatever was true back when the job was first proposed — that's the number "Actual"
// is meaningfully being compared against.
static QSet<int> removedStreamIndices(const QList<StreamRecord>& allStreams, const QString& commandArgsJson)
{
	const QList<StreamRecord> keptStreams =
		ActionEngine::computeKeptStreams(allStreams, commandArgsJson);
	QSet<int> keptIdx;
	for (const auto& s : keptStreams) keptIdx.insert(s.streamIndex);
	QSet<int> removed;
	for (const auto& s : allStreams)
		if (!keptIdx.contains(s.streamIndex)) removed.insert(s.streamIndex);
	return removed;
}

// Live-recomputed estimate for a single job — shared by the sort below and by
// startJob() (which needs the current job's estimate for goal-progress tracking).
static qint64 estimateJobSavingBytesFromData(const FileRecord& file,
                                              const QList<StreamRecord>& allStreams,
                                              const QString& commandArgsJson)
{
	const QSet<int> removed = removedStreamIndices(allStreams, commandArgsJson);
	return estimateSavingBytes(allStreams, removed, file.sizeBytes, file.durationSec);
}

// Bytes + per-track breakdown together, for the one call site (tryStartJob) that
// persists both back into the DB — kept separate from the bytes-only helper above
// so the sort path (called on every dispatchJobs() pass) doesn't pay for building a
// JSON breakdown it never uses.
struct FreshJobEstimate {
	qint64  bytes = 0;
	QString streamEstimatesJson;
};

static FreshJobEstimate estimateJobSavingWithBreakdown(const JobRecord& job)
{
	auto& db = DatabaseManager::instance();
	const auto fileOpt = db.fileById(job.fileId);
	if (!fileOpt) return {};

	const QList<StreamRecord> allStreams = db.streamsForFile(job.fileId);
	const QSet<int> removed = removedStreamIndices(allStreams, job.commandArgsJson);

	FreshJobEstimate result;
	result.bytes = estimateSavingBytes(allStreams, removed, fileOpt->sizeBytes, fileOpt->durationSec);
	result.streamEstimatesJson =
		buildStreamEstimatesJson(allStreams, removed, fileOpt->sizeBytes, fileOpt->durationSec);
	return result;
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
	slot.activation         = m_nextSlotActivation++;
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

	const FreshJobEstimate freshEstimate = estimateJobSavingWithBreakdown(job);
	const qint64 estimatedSavings        = freshEstimate.bytes;

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
		connect(proc, &QProcess::errorOccurred,
		        this, [this, proc, jobId, fileId, storageGroup, sidecarDeletionsJson,
		               flagChangesJson, allStreams](QProcess::ProcessError err) {
			// FailedToStart never emits finished() — fail the job here so the
			// slot is released instead of staying busy until app restart.
			if (err != QProcess::FailedToStart)
				return;
			const QString log = QStringLiteral("Failed to start mkvpropedit: %1")
			                        .arg(proc->errorString());
			proc->deleteLater();
			finishTagEditJob(storageGroup, jobId, fileId, false, log, sidecarDeletionsJson,
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

	// Pre-flight staleness check — ffprobe the file live and compare it against
	// what the DB (and therefore this job's command line) currently believes the
	// track layout is. Catches a source file that drifted out-of-band since the
	// last scan before burning a full mkvmerge remux only to discover the same
	// mismatch afterward via RemuxJob::onProcessFinished's post-mux verification.
	// Sidecar subtitles aren't part of the ffprobe comparison — ffprobe never sees
	// them — so they're only checked for continued existence on disk.
	if (fileId > 0 && fileOpt) {
		const QString preflightFfprobePath = ExternalTools::instance().ffprobePath();
		FfprobeScanner preflightScanner(preflightFfprobePath);
		const auto preflightResult = preflightScanner.scanFile(fileOpt->path);
		if (preflightResult.success) {
			const QList<StreamRecord> dbStreams = db.streamsForFile(fileId);

			QList<StreamRecord> dbContainerStreams;
			bool sidecarMissing = false;
			for (const auto& s : dbStreams) {
				if (s.isExternal) {
					if (!s.externalPath.isEmpty() && !QFile::exists(s.externalPath))
						sidecarMissing = true;
				} else {
					dbContainerStreams.append(s);
				}
			}

			const ActionEngine::StreamDiff preflightDiff =
				ActionEngine::diffStreams(dbContainerStreams, preflightResult.streams);

			if (!preflightDiff.isEmpty() || sidecarMissing) {
				qWarning() << "JobQueue: job" << jobId << "for file" << fileId
				           << "is stale vs the current on-disk track layout"
				           << (sidecarMissing ? "(missing sidecar file)" : "(track drift)")
				           << "— dropping and rescanning instead of remuxing";
				db.deleteJob(jobId);
				emit jobFinished(jobId, false, 0);
				rescanFile(fileId, {}, /*triggerReanalysis=*/true);
				return false;
			}
		}
		// If the pre-flight scan itself fails (locked file, transient I/O hiccup on
		// a network share, etc.), don't block the job on it — proceed and let the
		// existing post-mux verification be the backstop.
	}

	// Reserve this storage-group slot and flip the job to "running" now, before
	// the subtitle prefetch below — mirrors the tag_edit path's use of
	// finishBusy above. Two reasons: (1) marks the group busy immediately, so a
	// dispatchJobs() call re-entering while we're blocked waiting on the
	// download (e.g. a different storage group's job finishing mid-wait) can't
	// pick this same job or group again; (2) the UI's "queued → running"
	// transition and auto-scroll fire right away instead of being delayed by
	// however long the subtitle wait takes.
	// Note: deliberately not holding a RunningJobSlot& across the subtitle wait
	// below — a different storage group's job getting reentrantly dispatched
	// during that wait can insert a new key into m_runningByGroup and rehash,
	// invalidating any reference taken beforehand. Every access here is its own
	// fresh operator[] lookup instead.
	// Re-freeze the estimate (and its per-track breakdown) using the data available
	// right now, immediately before the mux actually runs — replaces whatever was
	// frozen at proposal time, which can be stale by the time a queued job's turn
	// comes up. This is the number "Actual" is compared against in the job log.
	db.updateJobEstimate(jobId, estimatedSavings, freshEstimate.streamEstimatesJson);

	occupySlot(storageGroup, jobId, fileId, estimatedSavings);
	m_runningByGroup[storageGroup].finishBusy = true;
#ifdef Q_OS_WIN
	// Capture the folder's timestamp now, before the subtitle prefetch touches
	// it — restored once this job's mux actually finishes mutating the folder,
	// whichever way that turns out (success, cancel, or failure). Keyed by
	// jobId (not kept on the RunningJobSlot) so it survives cancelJob()'s
	// releaseSlot() call, which happens before the killed process's async
	// cleanup — and thus the restore — actually runs.
	if (fileOpt) {
		const QFileInfo dirFi(QFileInfo(fileOpt->path).absolutePath());
		m_pendingDirTimestamps[jobId] = DirTimestamp{ dirFi.birthTime(), dirFi.lastModified() };
	}
#endif
	db.updateJobStatus(jobId, "running");
	emit jobStarted(jobId);

	// Sidecar merging is about to absorb whatever external subtitles exist right
	// now (streams fetched just below) — give a missing one a short, bounded
	// chance to land first so it gets embedded instead of only ever existing as
	// an unmerged sidecar. No-ops immediately if there's nothing fetchable or
	// downloading can't run right now; if OpenSubtitles is slow, this simply
	// gives up after the timeout and the file muxes without it, same as before —
	// it stays available for a manual "Download Subtitles" pass afterward.
	const quint64 slotActivation = m_runningByGroup[storageGroup].activation;
	if (m_mergeSidecarSubtitles && fileId > 0) {
		SubtitleManager::instance().tryDownloadNow(fileId, kSubtitlePrefetchTimeoutMs);
		// tryDownloadNow blocks in a nested event loop, so UI events ran while this
		// frame was suspended: the user may have cancelled this job (cancelJob()
		// released the slot — there was no RemuxJob to kill yet) or the whole queue,
		// and a dispatch may since have handed the group's slot to another job — or
		// even to a fresh activation of this same one. In all of those cases this
		// frame no longer owns the slot and must not launch a mux against it; the
		// job itself was already put back to "queued" by the cancel path.
		const auto slotIt = m_runningByGroup.constFind(storageGroup);
		if (slotIt == m_runningByGroup.cend() || slotIt->activation != slotActivation) {
#ifdef Q_OS_WIN
			// This activation never touched the folder — drop its captured
			// timestamp unless a fresh activation of the same job re-captured one.
			if (slotIt == m_runningByGroup.cend() || slotIt->jobId != jobId)
				m_pendingDirTimestamps.remove(jobId);
#endif
			return true;
		}
	}

	QStringList args;
	const QJsonArray arr = QJsonDocument::fromJson(commandArgsJson.toUtf8()).array();
	for (const auto& v : arr) args << v.toString();

	// Inject flag changes and sidecar subtitle files into the mkvmerge command.
	// Capture the source path before sidecar args are appended — each sidecar block
	// ends with its file path, which would otherwise corrupt args.last().
	QString sourcePath;
	// What RemuxJob should find left over in the output once mkvmerge is done —
	// computed from the same "before" snapshot and commandArgsJson used to build
	// the command itself, so it verifies against the same expectation the queue
	// is acting on. Attribute-based (see ActionEngine::diffStreams), not index-based.
	QList<StreamRecord> expectedStreams;
	if (!args.isEmpty()) {
		const QList<StreamRecord> streams = db.streamsForFile(fileId);

		// Refresh the "before" snapshot from the current live streams right before
		// this remux actually runs — a proposed job can sit queued long enough for
		// the on-disk track/sidecar layout to change, so a snapshot frozen at
		// proposal time would go stale. This is what the "done" card renders from.
		db.updateJobOriginalStreams(jobId, ActionEngine::serializeStreamSnapshot(streams));

		expectedStreams = ActionEngine::computeKeptStreams(streams, commandArgsJson);

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
			// Some subtitle sources (OpenSubtitles included) sometimes serve
			// MicroDVD content mislabeled with a .srt extension — mkvmerge sniffs
			// the real content and rejects the whole job outright. Fix any such
			// sidecar in place before building the merge args, using this file's
			// own frame rate to convert frame-based timing to real timestamps.
			double videoFps = 0.0;
			for (const StreamRecord& s : streams) {
				if (s.codecType == QLatin1String("video")) {
					videoFps = ActionEngine::parseFrameRate(s.frameRate);
					break;
				}
			}
			if (videoFps > 0.0) {
				for (const StreamRecord& s : streams) {
					if (s.isExternal && !s.externalPath.isEmpty() && s.codecType == QLatin1String("subtitle"))
						ActionEngine::fixMislabeledSidecarSubtitle(s.externalPath, videoFps);
				}
			}

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
	const QString ffprobePath  = ExternalTools::instance().ffprobePath();
	// The slot was already reserved (and the job already marked "running")
	// above, before the subtitle prefetch — fetch it fresh now (safe: nothing
	// between here and its use below touches m_runningByGroup) and attach the
	// actual process.
	RunningJobSlot& slot = m_runningByGroup[storageGroup];
	slot.remux = new RemuxJob(jobId, mkvmergePath, args, sourcePath, descriptionText,
	                          m_writeJobLog, finalOutputPathOverride,
	                          expectedStreams, ffprobePath, this);

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
		// This is an internal rescan following a job (remux, tag edit, self-heal),
		// not a user-initiated library scan — don't let it bump "Last scanned",
		// or a processed movie bubbles to the top of that sort for no reason the
		// user actually did.
		updated.scanTime = fileCopy.scanTime;
		(void)db2.upsertFile(updated);
		// Re-discover sidecar subtitles alongside the (possibly renamed) file
		const auto sidecars = ScanWorker::scanSidecarSubtitles(
		    fileCopy.path, ScanWorker::nextSidecarStreamIndex(result.streams), m_detectSubtitleLanguage);
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

void JobQueue::restoreDirTimestampForJob(qint64 jobId, qint64 fileId)
{
	const auto it = m_pendingDirTimestamps.constFind(jobId);
	if (it == m_pendingDirTimestamps.cend()) return;
	const DirTimestamp snap = it.value();
	m_pendingDirTimestamps.erase(it);
#ifdef Q_OS_WIN
	if (fileId <= 0) return;
	const auto fileOpt = DatabaseManager::instance().fileById(fileId);
	if (!fileOpt) return;
	const QString dirPath = QFileInfo(fileOpt->path).absolutePath();
	preserveDirTimestamps(dirPath, snap.origCreated, snap.origModified);
#else
	Q_UNUSED(fileId);
#endif
}

void JobQueue::onRemuxJobFinished(int storageGroup, qint64 jobId, qint64 fileId,
                                  int exitCode, const QString& log, qint64 savedBytes)
{
	if (consumeAbortedJob(jobId, storageGroup)) {
		// The killed process's .tmp cleanup (RemuxJob::onProcessFinished's
		// exitCode!=0 branch) has now run — restore the folder timestamp it
		// bumped, same as any other non-success exit below.
		restoreDirTimestampForJob(jobId, fileId);
		return;
	}

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
	// hasTrackMismatch() covers both triggers RemuxJob checks before finalizing: the
	// mkvmerge "track not found" warning, and the streams-level diff against what
	// the job was expected to keep (see RemuxJob::onProcessFinished).
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

		QString warningText;
		if (!warnLines.isEmpty()) {
			warningText = warnLines.join('\n');
		} else {
			// No mkvmerge warning text — this was caught by the streams-level diff
			// instead (see RemuxJob::onProcessFinished). Describe it from that.
			const ActionEngine::StreamDiff diff = remux->streamDiff();
			if (!diff.missing.isEmpty() || !diff.unexpected.isEmpty()) {
				warningText = tr("The output doesn't match what this job expected to keep "
				                 "(%1 missing, %2 unexpected track(s)).")
				    .arg(diff.missing.size()).arg(diff.unexpected.size());
			} else {
				warningText = tr("A requested track was not found in the source file.");
			}
		}

		// Source streams: use the snapshot stored at job-creation time so the source
		// card is always correct even when the file was modified by an earlier job run.
		const QList<StreamRecord> srcStreams = (jobOpt && !jobOpt->originalStreamsJson.isEmpty())
			? ActionEngine::deserializeStreamSnapshot(jobOpt->originalStreamsJson)
			: QList<StreamRecord>{};

		// RemuxJob already scanned the .tmp output as part of its own verification
		// gate — reuse that instead of probing it again. Only fall back to a fresh
		// scan if that didn't happen (e.g. no expectedStreams to verify against).
		const QList<StreamRecord> cachedTmpStreams = remux->tmpStreams();
		if (!cachedTmpStreams.isEmpty()) {
			emit reviewRequested(jobId, filename, warningText, cmdArgs, srcStreams, cachedTmpStreams);
			return;
		}

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

	if (!ok) {
		// mkvmerge can't tell a genuinely missing source from a sidecar file that a
		// previous job already muxed into the container and deleted, but whose
		// removal never made it back into this job's (now stale) command line —
		// e.g. the job was queued before that earlier job's rescan ran. Recognize
		// that specific mkvmerge error and self-heal instead of leaving a
		// permanently failed job: drop the stale job and rescan/reanalyze the file
		// so a fresh job gets built from the current on-disk track layout.
		static const QRegularExpression staleFileRe(
		    R"(The file '(.+)' could not be opened for reading: open file error)",
		    QRegularExpression::CaseInsensitiveOption);
		const auto    staleMatch  = staleFileRe.match(log);
		const QString missingPath = staleMatch.hasMatch() ? staleMatch.captured(1) : QString{};
		const bool    staleSidecar = !missingPath.isEmpty() && missingPath != remux->inputFilePath();

		if (staleSidecar && fileId > 0) {
			qWarning() << "JobQueue: job" << jobId << "referenced" << missingPath
			           << "which no longer exists (likely already muxed in by an earlier job) "
			              "— dropping the job and rescanning" << fileId;
			db.deleteJob(jobId);
			remux->deleteLater();
			releaseSlot(storageGroup);
			emit jobFinished(jobId, false, 0);
			restoreDirTimestampForJob(jobId, fileId);
			rescanFile(fileId, {}, /*triggerReanalysis=*/true);

			if (m_running && !m_paused) {
				dispatchJobs();
			} else if (m_paused) {
				if (DatabaseManager::instance().queuedJobCount() == 0 && !hasActiveJob()) {
					m_running = false;
					emit allFinished();
				}
			}
			return;
		}
	}

	db.updateJobStatus(jobId, ok ? "done" : "failed", exitCode, log);
	if (ok && savedBytes > 0) {
		db.updateJobSavedBytes(jobId, savedBytes);
		db.updateCalibrationFromJob(jobId);
		AppSettings::instance().addReclaimedBytes(savedBytes);
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

		// Every file operation for this job (subtitle prefetch, mux, rename,
		// sidecar cleanup) is done now — restore the folder's timestamp to what
		// it was before any of it started, so this automated pass doesn't bubble
		// the folder up as "newest" in media libraries sorted by Date Modified.
		restoreDirTimestampForJob(jobId, fileId);

		if (!finalOutput.isEmpty() && finalOutput != originalInput) {
			const QFileInfo fi(finalOutput);
			db.updateFilePath(fileId, finalOutput, fi.fileName());
		}
		rescanFile(fileId, {});
	} else if (!ok) {
		// mkvmerge failed for an ordinary reason (not cancelled, not the
		// stale-sidecar self-heal above) — RemuxJob::onProcessFinished still
		// deleted the .tmp it had started writing, bumping the folder the same
		// way a success would; restore it here too.
		restoreDirTimestampForJob(jobId, fileId);
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
	// remux-completion path in RemuxJob::onProcessFinished(). No raw `this`: the
	// task can outlive this object on app shutdown mid NAS copy, so results are
	// marshaled through qApp with a QPointer guard checked on the main thread.
	QThreadPool::globalInstance()->start(
	    [self = QPointer<JobQueue>(this), jobId, fileId, tmpPath, finalPath, srcPath,
	     origSize, exitCode, wasStaged, storageGroup]() {
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

			QMetaObject::invokeMethod(qApp, [self, jobId] {
				if (self) emit self->phaseChanged(jobId, tr("Copying to NAS"));
			}, Qt::QueuedConnection);

			const bool copyOk = RemuxJob::copyFileWithProgress(
			    tmpPath, remoteTmp, outputSize,
			    [self, jobId](int pct, qint64 bytes) {
				QMetaObject::invokeMethod(qApp, [self, jobId, pct, bytes] {
					if (!self) return;
					emit self->progressChanged(jobId, pct);
					emit self->outputSizeChanged(jobId, bytes);
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
			QMetaObject::invokeMethod(qApp, [self, storageGroup, jobId] {
				if (!self) return;
				self->releaseSlot(storageGroup);
				emit self->jobFinished(jobId, false, 0);
			}, Qt::QueuedConnection);
			return;
		}

		db.updateJobStatus(jobId, "done", exitCode);
		if (savedBytes > 0) {
			db.updateJobSavedBytes(jobId, savedBytes);
			db.updateCalibrationFromJob(jobId);
		}

		QMetaObject::invokeMethod(qApp, [self, storageGroup, jobId, fileId, finalPath, isInPlace, savedBytes] {
			if (!self) return;
			self->releaseSlot(storageGroup);

			if (savedBytes > 0) {
				AppSettings::instance().addReclaimedBytes(savedBytes);
			}

			emit self->jobFinished(jobId, true, savedBytes);

			if (fileId > 0) {
				self->deleteMergedSidecars(jobId);
				self->restoreDirTimestampForJob(jobId, fileId);
				if (!isInPlace) {
					const QFileInfo fi(finalPath);
					DatabaseManager::instance().updateFilePath(fileId, finalPath, fi.fileName());
				}
				self->rescanFile(fileId, {});
			}

			if (self->m_running)
				self->dispatchJobs();
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
	restoreDirTimestampForJob(jobId, fileId);

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
