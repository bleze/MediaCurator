#pragma once

#include <QMutex>
#include <QHash>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
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
	QString     displayTitle;     // TMDB/user-assigned override; preferred over all others
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
	bool        isDefault = false;
	bool        isForced = false;
	bool        isHearingImpaired = false;
	bool        isVisualImpaired = false;
	QString     pixelFormat;
	QString     frameRate;
	QString     codecLevel;
	QString     codecProfile;
	QString     extraJson;
};

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
	qint64      savedBytes = 0;       // actual bytes freed after completion
	QString     descriptionText;      // human-readable list of removed tracks
	QString     originalStreamsJson;  // stream snapshot taken before remux — for done-job display
};

// Poster / enrichment cache
struct PosterRecord {
	qint64  fileId      = -1;
	QString source;         // "embedded" | "tmdb" | ""
	QString status;         // "pending" | "done" | "no_poster" | "failed"
	QString imagePath;
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
	qint64  savedBytes = 0;
	qint64  sizeBytes  = 0;
	double  durationSec = 0.0;
	qint64  createdAt  = 0;
	QString imdbId;            // from poster_cache; empty if no IMDb link yet
	double  voteAverage = 0.0;
	QString originalLanguage;  // ISO 639-2; set by RuleEngine or TMDB dialog
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
	QList<FileRecord> allFiles() const;
	QList<FileRecord> allFilesPaged(int offset, int limit) const;
	QList<FileRecord> filesUnderPath(const QString& rootPath) const;
	int fileCountUnderPath(const QString& rootPath) const;
	int removeFilesUnderPath(const QString& rootPath);
	bool deleteFile(qint64 fileId);
	void updateFilePath(qint64 fileId, const QString& newPath, const QString& newFilename);

	// ── Streams ──────────────────────────────────────────────────────────────
	bool insertStreams(qint64 fileId, const QList<StreamRecord>& streams);
	bool deleteStreamsForFile(qint64 fileId);
	QList<StreamRecord> streamsForFile(qint64 fileId) const;
	QList<StreamRecord> allStreams() const;
	QHash<qint64, QList<StreamRecord>> allStreamsGrouped() const;
	QHash<qint64, QList<StreamRecord>> streamsForFiles(const QList<qint64>& fileIds) const;

	// ── Jobs ─────────────────────────────────────────────────────────────────
	[[nodiscard]] bool   hasActiveJobForFile(qint64 fileId) const;
	[[nodiscard]] qint64 insertJob(const JobRecord& job);
	bool updateJobStatus(qint64 jobId, const QString& status, int resultCode = -1, const QString& log = {});
	bool updateJobSavedBytes(qint64 jobId, qint64 savedBytes);
	bool updateJobCommandArgs(qint64 jobId, const QString& commandArgsJson);
	bool deleteJob(qint64 jobId);
	bool clearJobsByStatus(const QString& status);
	bool promoteJobsToQueued(const QList<qint64>& jobIds);
	bool updateFileOriginalLanguage(qint64 fileId, const QString& lang);
	bool updateDisplayTitle(qint64 fileId, const QString& title);
	QList<JobRecord> queuedJobs() const;
	QList<JobRecord> allJobs() const;
	QList<JobDisplayRecord> allJobsForPanel() const;
	QList<JobDisplayRecord> allJobsForPanelPaged(int limit, const QString& statusFilter = {}) const;
	int                     totalJobCount() const;
	int                     queuedJobCount() const;

	// ── Poster cache ─────────────────────────────────────────────────────────
	void                        upsertPosterRecord(const PosterRecord& rec);
	std::optional<PosterRecord> posterForFile(qint64 fileId) const;
	QList<qint64>               fileIdsNeedingPosters() const;
	QHash<qint64, QString>      allDonePosterPaths() const;
	QHash<qint64, QString>      allKnownImdbIds() const;
	QHash<qint64, double>       allRatings() const;
	void                        resetPosterForFile(qint64 fileId);
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
	mutable QMutex   m_connMutex;
};

} // namespace Mc

Q_DECLARE_METATYPE(Mc::FileRecord)
Q_DECLARE_METATYPE(QList<Mc::StreamRecord>)
