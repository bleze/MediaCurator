#pragma once

#include <QHash>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMetaType>
#include <optional>

namespace Mc {

// Mirrors the 'files' table row
struct FileRecord {
	qint64      id = -1;
	QString     path;
	QString     filename;
	qint64      sizeBytes = 0;
	qint64      mtimeMs = 0;
	qint64      createdMs = 0;  // filesystem birth time (creation date) in ms since epoch
	QString     container;
	double      durationSec = 0.0;
	qint64      overallBitrate = 0;
	QString     originalLanguage;
	qint64      scanTime = 0;
	qint64      scanRunId = -1;
	bool        needsRescan = false;
	QString     containerTitle;   // title from ffprobe format tags; may be absent or junk
	QString     embeddedImdbId;  // IMDb ID from container tags (e.g. IMDB=tt1234567); not persisted
	QString     displayTitle;     // TMDB/user-assigned override; preferred over all others
	int         displayYear  = 0; // release year from TMDB (0 = unknown)
	bool        ignored = false;  // user-hidden; excluded from library view by default
	qint64      subtitleAttemptedMs = 0; // last time an OpenSubtitles lookup was attempted (0 = never)
};

// Mirrors the 'streams' table row
struct StreamRecord {
	qint64      id = -1;
	qint64      fileId = -1;
	int         streamIndex = 0;
	QString     codecType;      // video|audio|subtitle|data
	QString     codecName;
	QString     language;       // ISO 639-2
	QString     title;
	QString     trackType;      // classified: main|commentary|sdh|forced|signs|...
	double      typeConfidence = 0.0;
	int         channels = 0;
	int         sampleRate = 0;
	qint64      bitRate = 0;
	int         width = 0;
	int         height = 0;
	QString     hdrFormat;
	int         maxCll = 0;      // Max Content Light Level (nits)
	int         maxFall = 0;     // Max Frame-Average Light Level (nits)
	QString     masteringDisplay; // e.g. "R(0.68,0.32) G(0.265,0.69) B(0.15,0.06) WP(0.3127,0.329) L(1000,0.0005)"
	bool        isDefault = false;
	bool        isForced = false;
	bool        isOriginal = false;
	bool        isCommentary = false;
	bool        isHearingImpaired = false;
	bool        isVisualImpaired = false;
	QString     pixelFormat;
	QString     frameRate;
	QString     codecLevel;
	QString     codecProfile;
	QString     extraJson;
	bool        isExternal = false;   // sidecar file, not embedded in container
	QString     externalPath;         // absolute path to sidecar file if isExternal
};

using FileStreamMap  = QHash<qint64, QList<StreamRecord>>;
using StreamRecordList = QList<StreamRecord>;
using FileRecordList   = QList<FileRecord>;
using FileIdList       = QList<qint64>;

// Mirrors the 'jobs' table row
struct JobRecord {
	qint64      id = -1;
	qint64      fileId = -1;
	QString     status;         // proposed|queued|running|done|failed|cancelled
	QString     jobType;        // remux|tag_edit
	QString     commandArgsJson;
	QString     summary;        // human-readable "Remove 2 audio, 3 subtitle"
	bool        dryRun = false;
	qint64      createdAt = 0;
	qint64      startedAt = 0;
	qint64      finishedAt = 0;
	int         resultCode = -1;
	QString     outputLog;
	qint64      savedBytes = 0;          // actual bytes freed after completion
	qint64      estimatedSavedBytes = 0; // estimate recorded at job-creation time
	QString     descriptionText;      // human-readable list of removed tracks
	QString     originalStreamsJson;  // stream snapshot taken before remux — for done-job display
	QString     flagChangesJson;       // JSON array of {streamIndex, flag, value} flag overrides
	QString     sidecarDeletionsJson;  // JSON array of sidecar file paths to delete alongside this job
	QString     streamEstimatesJson;   // per-track estimates recorded at job-creation time (calibration)
};

// One row per (codec_name, used_fallback) in the calibration table.
// avgRatio = mean(saved_bytes / estimated_saved_bytes) across all completed jobs
// that removed a track of this codec. Values < 1.0 mean we over-estimated;
// values > 1.0 mean we under-estimated. Only usedFallback entries have a
// meaningful suggestedFallback — for declared-bitrate tracks the ratio tells
// you how accurate ffprobe's declared values are for this codec family.
struct CalibrationEntry {
	QString codecName;
	QString codecType;
	bool    usedFallback = false;
	int     sampleCount  = 0;
	double  avgRatio     = 1.0;
	double  stdDevRatio  = 0.0;
};

// Poster / enrichment cache
struct PosterRecord {
	qint64  fileId      = -1;
	QString source;         // "embedded" | "tmdb" | ""
	QString status;         // "pending" | "done" | "no_poster" | "failed"
	QString imagePath;
	QString fanartPath;     // absolute path to w780 backdrop; empty if not yet fetched
	QString imdbId;
	qint64  fetchedAt   = 0;
	double  voteAverage = 0.0;
	int     voteCount   = 0;
};

// For display in McJobPanel (jobs JOIN files JOIN poster_cache)
struct JobDisplayRecord {
	qint64  jobId      = -1;
	qint64  fileId     = -1;
	QString filename;
	QString filePath;
	QString summary;
	QString status;
	QString jobType;           // "remux" or "tag_edit"
	qint64  savedBytes = 0;
	qint64  sizeBytes  = 0;
	double  durationSec = 0.0;
	qint64  createdAt   = 0;
	qint64  finishedAt  = 0;   // epoch seconds; 0 for jobs not yet completed
	QString imdbId;            // from poster_cache; empty if no IMDb link yet
	double  voteAverage = 0.0;
	QString originalLanguage;  // ISO 639-2; set by RuleEngine or TMDB dialog
	QString commandArgsJson;
	QString originalStreamsJson;
	QString flagChangesJson;
	QString containerTitle;   // title from ffprobe format tags; may be absent or junk
	QString displayTitle;     // TMDB/user-assigned override; preferred over all others
	int     displayYear = 0;  // release year from TMDB (0 = unknown)
};

enum class JobSortMode {
	SmallestFirst,       // process smallest files first — best when low on disk space
	LargestSavingsFirst, // process largest files first — best to recover maximum space quickly
	MostRecentFirst,     // completed jobs sorted by finished_at DESC — for the Done tab
};

/**
 * DatabaseManager — singleton access to the SQLite database.
 *
 * All SQL lives here. No other class should issue raw SQL queries.
 * The database file is stored in QStandardPaths::AppDataLocation.
 */
class DatabaseManager : public QObject
{
	Q_OBJECT
public:
	static DatabaseManager& instance();

	// Opens (or creates) the database. Call once at startup.
	bool open(const QString& dbPath = {});
	void close();
	bool isOpen() const;

	QString databasePath() const { return m_dbPath; }

	// ── Scan runs ────────────────────────────────────────────────────────────
	[[nodiscard]] qint64 beginScanRun(const QString& rootPath);
	void endScanRun(qint64 scanRunId, int added, int updated, int removed);

	// ── Files ────────────────────────────────────────────────────────────────
	[[nodiscard]] std::optional<qint64> upsertFile(const FileRecord& rec);
	[[nodiscard]] std::optional<FileRecord> fileById(qint64 id) const;
	[[nodiscard]] std::optional<FileRecord> fileByPath(const QString& path) const;
	// Batched form of fileById() — one query (chunked) instead of one round trip per
	// id. Use this instead of calling fileById() in a loop over more than a handful
	// of ids (see streamsForFiles() for the same pattern with streams).
	QHash<qint64, FileRecord> filesByIds(const QList<qint64>& ids) const;
	QList<FileRecord> allFiles() const;
	// Files with no row at all in the jobs table (any status) — never analyzed
	// before, as opposed to a stale/failed/completed job existing already. Used
	// by "Quick Analyze" to only run the rule engine over files it hasn't
	// touched yet, unlike "Analyze Library" which re-evaluates everything.
	QList<FileRecord> filesWithoutAnyJob() const;
	// sortOrder mirrors Mc::McFileListModel::SortOrder (0=Name, 1=Newest, 2=Oldest,
	// 3=Largest, 4=RatingHigh, 5=RatingLow) — duplicated as a plain int here so this
	// core class doesn't depend on a UI header. Callers must keep pages in the same
	// order the model itself sorts by (McFileListModel::sortOrder()), or the "first
	// page" loaded synchronously at startup won't match what's actually about to
	// land in the viewport, and every later background page reshuffles rows above
	// the fold as it arrives.
	QList<FileRecord> allFilesPaged(int offset, int limit, int sortOrder = 0) const;
	QList<FileRecord> filesUnderPath(const QString& rootPath) const;
	int fileCount() const;
	int fileCountUnderPath(const QString& rootPath) const;
	int removeFilesUnderPath(const QString& rootPath);
	bool deleteFile(qint64 fileId);
	void updateFilePath(qint64 fileId, const QString& newPath, const QString& newFilename);

	// ── Quick Scan directory baselines ──────────────────────────────────────
	// A known folder's own mtime moving past its recorded baseline means an entry
	// changed there (file added/removed/renamed) since the baseline was last set —
	// see ScanWorker's quick-scan walk. Baselines must be re-touched immediately
	// after MediaCurator itself writes a sidecar file (.nfo, downloaded .srt) into
	// a folder — otherwise that self-inflicted mtime bump would look like an
	// external change on the next scan and defeat Quick Scan's whole point.
	QHash<QString, qint64> allDirBaselineMtimes() const;
	void touchDirBaseline(const QString& dirPath);

	// ── Subtitle retry cooldown ──────────────────────────────────────────────
	void updateSubtitleAttempted(qint64 fileId, qint64 epochMs);

	// ── Streams ──────────────────────────────────────────────────────────────
	bool insertStreams(qint64 fileId, const QList<StreamRecord>& streams);
	bool deleteStreamsForFile(qint64 fileId);
	// Updates just the language + path of one external sidecar stream (e.g. after
	// renaming it to include a language token). Unlike a full rescan, this touches
	// only this one row — it never invalidates any pending job for the file.
	bool updateStreamExternalInfo(qint64 fileId, int streamIndex,
	                               const QString& language, const QString& externalPath);
	// Persists a single default/forced/original disposition flag directly to the
	// streams table — used by immediate (non-job) track flag edits.
	bool updateStreamFlag(qint64 fileId, int streamIndex, const QString& flag, bool value);
	// Persists the language of an embedded (non-sidecar) stream. For external
	// sidecars use updateStreamExternalInfo instead (it also renames the file).
	bool updateStreamLanguageInternal(qint64 fileId, int streamIndex, const QString& language);
	QList<StreamRecord> streamsForFile(qint64 fileId) const;
	QList<StreamRecord> allStreams() const;
	QHash<qint64, QList<StreamRecord>> allStreamsGrouped() const;
	QHash<qint64, QList<StreamRecord>> streamsForFiles(const QList<qint64>& fileIds) const;

	// ── Stream overrides (user-forced removals) ───────────────────────────────
	void setStreamForcedRemoval(qint64 fileId, int streamIndex, bool forced);
	QHash<qint64, QSet<int>> allStreamForcedRemovals() const;
	void clearStreamForcedRemovals(qint64 fileId);

	// ── Jobs ─────────────────────────────────────────────────────────────────
	[[nodiscard]] bool   hasActiveJobForFile(qint64 fileId) const;
	[[nodiscard]] std::optional<JobRecord> activeJobForFile(qint64 fileId) const;
	[[nodiscard]] qint64 insertJob(const JobRecord& job);
	bool updateJobStatus(qint64 jobId, const QString& status, int resultCode = -1, const QString& log = {});
	bool updateJobSavedBytes(qint64 jobId, qint64 savedBytes);
	bool updateJobEstimate(qint64 jobId, qint64 estimatedSavedBytes, const QString& streamEstimatesJson);
	void updateCalibrationFromJob(qint64 jobId);
	[[nodiscard]] QList<CalibrationEntry> calibrationReport() const;
	// Empty codecNames clears everything; otherwise clears only the named
	// codecs' used_fallback=1 rows — the ones actually included when the
	// calibration dialog's "Copy Suggested Code" ran, so a format still
	// accumulating samples (too few to have been actionable yet) isn't reset
	// just because some other format's constant was just updated in code.
	void clearCalibration(const QStringList& codecNames = {});
	bool updateJobType(qint64 jobId, const QString& jobType);
	bool updateJobCommandArgs(qint64 jobId, const QString& commandArgsJson);
	bool updateJobFlagChanges(qint64 jobId, const QString& flagChangesJson);
	bool updateJobSidecarDeletions(qint64 jobId, const QString& sidecarDeletionsJson);
	bool updateJobOriginalStreams(qint64 jobId, const QString& originalStreamsJson);
	bool updateJobSummary(qint64 jobId, const QString& summary);
	bool deleteJob(qint64 jobId);
	bool deleteJobsBatch(const QList<qint64>& jobIds);
	bool clearJobsByStatus(const QString& status);
	bool promoteJobsToQueued(const QList<qint64>& jobIds);
	bool requeueRunningJobs(const QList<qint64>& jobIds);
	bool requeueFailedJobs(const QList<qint64>& jobIds);
	bool recoverRunningJobs();
	bool updateFileOriginalLanguage(qint64 fileId, const QString& lang);
	bool updateDisplayTitle(qint64 fileId, const QString& title, int year = 0);
	bool setFileIgnored(qint64 fileId, bool ignored);
	void deleteJobsForFile(qint64 fileId);
	void deletePendingJobsForFile(qint64 fileId);
	[[nodiscard]] std::optional<JobRecord> jobById(qint64 jobId) const;
	QList<JobRecord> queuedJobs(JobSortMode sortMode = JobSortMode::SmallestFirst) const;
	QList<JobRecord> allJobs() const;
	QList<JobDisplayRecord> allJobsForPanel(JobSortMode sortMode = JobSortMode::SmallestFirst) const;
	QList<JobDisplayRecord> allJobsForPanelPaged(int limit, const QString& statusFilter = {}, JobSortMode sortMode = JobSortMode::SmallestFirst) const;
	[[nodiscard]] std::optional<JobDisplayRecord> jobDisplayRecordById(qint64 jobId) const;
	QList<JobDisplayRecord> liveJobsForPanel() const;
	int                     totalJobCount() const;
	int                     queuedJobCount() const;
	QHash<QString, int>     jobStatusCounts() const;
	QSet<qint64>            proposedJobFileIds() const;
	QList<qint64>           jobIdsByStatus(const QString& status) const;

	// ── Poster cache ─────────────────────────────────────────────────────────
	void                        upsertPosterRecord(const PosterRecord& rec);
	std::optional<PosterRecord> posterForFile(qint64 fileId) const;
	QList<qint64>               fileIdsNeedingPosters() const;
	QHash<qint64, QString>      allDonePosterPaths() const;
	QHash<qint64, QString>      allDoneFanartPaths() const;
	QHash<qint64, QString>      allKnownImdbIds() const;
	QHash<qint64, double>       allRatings() const;

	// Batch load for library startup (poster + fanart + IMDb + ratings).
	// Fills the hashes from a single pass over poster_cache to reduce query overhead.
	void loadPosterMeta(QHash<qint64, QString>& posterPaths,
	                    QHash<qint64, QString>& imdbIds,
	                    QHash<qint64, double>& ratings,
	                    QHash<qint64, QString>& fanartPaths) const;

	void                        resetPosterForFile(qint64 fileId);
	void                        clearPosterPath(const QString& imagePath);
	void                        clearFanartPath(const QString& fanartPath);
	void                        updateImdbId(qint64 fileId, const QString& imdbId);
	void                        resetNoPosterRecords();

	// ── Startup cleanup ──────────────────────────────────────────────────────
	// Reset jobs stuck as 'running' (crashed) to 'failed' and delete their temp files.
	void cleanupStalledJobs();

	// ── Preferences ──────────────────────────────────────────────────────────
	bool setPref(const QString& key, const QString& value);
	QString getPref(const QString& key, const QString& defaultValue = {}) const;

signals:
	void databaseOpened();
	void fileAdded(qint64 fileId);
	void fileUpdated(qint64 fileId);
	void fileDeleted(qint64 fileId);
	void jobStatusChanged(qint64 jobId, const QString& status);

private:
	explicit DatabaseManager(QObject* parent = nullptr);
	~DatabaseManager() override;
	DatabaseManager(const DatabaseManager&) = delete;
	DatabaseManager& operator=(const DatabaseManager&) = delete;

	bool initSchema();
	bool runMigrations();

	// Returns the QSqlDatabase for the calling thread.
	// The main thread uses m_db; worker threads get their own named connection
	// created on first access so Qt's "one connection per thread" rule is satisfied.
	QSqlDatabase connection() const;

	QSqlDatabase     m_db;
	QString          m_dbPath;
	Qt::HANDLE       m_mainThreadId = {};
};

} // namespace Mc

Q_DECLARE_METATYPE(Mc::FileRecord)
Q_DECLARE_METATYPE(Mc::StreamRecordList)
Q_DECLARE_METATYPE(Mc::FileRecordList)
Q_DECLARE_METATYPE(Mc::FileStreamMap)
Q_DECLARE_METATYPE(Mc::FileIdList)
