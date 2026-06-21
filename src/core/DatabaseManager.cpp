#include "core/DatabaseManager.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QDebug>
#include <QMutexLocker>
#include <QThread>
#include <QVariant>

namespace Mc {

// ── Singleton ─────────────────────────────────────────────────────────────────

DatabaseManager& DatabaseManager::instance()
{
	static DatabaseManager s_instance;
	return s_instance;
}

DatabaseManager::DatabaseManager(QObject* parent)
	: QObject(parent)
{}

DatabaseManager::~DatabaseManager()
{
	close();
}

// ── Open / Close ──────────────────────────────────────────────────────────────

bool DatabaseManager::open(const QString& dbPath)
{
	if (m_db.isOpen())
		return true;

	if (dbPath.isEmpty()) {
		const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
		QDir().mkpath(dataDir);
		m_dbPath = dataDir + "/mediacurator.db";
	} else {
		m_dbPath = dbPath;
	}

	m_mainThreadId = QThread::currentThreadId();

	m_db = QSqlDatabase::addDatabase("QSQLITE", "mc_main");
	m_db.setDatabaseName(m_dbPath);

	if (!m_db.open()) {
		qCritical() << "DatabaseManager: failed to open database:" << m_db.lastError().text();
		return false;
	}

	// Enable WAL mode and foreign keys
	QSqlQuery pragma(connection());
	pragma.exec("PRAGMA journal_mode=WAL");
	pragma.exec("PRAGMA foreign_keys=ON");
	pragma.exec("PRAGMA synchronous=NORMAL");

	if (!initSchema()) {
		qCritical() << "DatabaseManager: schema init failed";
		return false;
	}

	emit databaseOpened();
	return true;
}

void DatabaseManager::close()
{
	if (m_db.isOpen())
		m_db.close();
}

bool DatabaseManager::isOpen() const
{
	return m_db.isOpen();
}

QSqlDatabase DatabaseManager::connection() const
{
	if (QThread::currentThreadId() == m_mainThreadId)
		return m_db;

	// Worker thread — create a named connection the first time, reuse thereafter.
	const QString name = QStringLiteral("mc_thread_%1")
						 .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));

	QMutexLocker lock(&m_connMutex);
	if (!QSqlDatabase::contains(name)) {
		QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
		db.setDatabaseName(m_dbPath);
		if (!db.open()) {
			qWarning() << "DatabaseManager: failed to open worker thread connection:" << db.lastError().text();
			return m_db;   // unsafe fallback — log loudly so it's noticed
		}
		QSqlQuery q(db);
		q.exec("PRAGMA journal_mode=WAL");
		q.exec("PRAGMA synchronous=NORMAL");
		q.exec("PRAGMA foreign_keys=ON");
	}
	return QSqlDatabase::database(name, false);
}

// ── Schema ────────────────────────────────────────────────────────────────────

bool DatabaseManager::initSchema()
{
	static const char* schema = R"(
		CREATE TABLE IF NOT EXISTS schema_version (
			version INTEGER NOT NULL
		);

		CREATE TABLE IF NOT EXISTS scan_runs (
			id             INTEGER PRIMARY KEY AUTOINCREMENT,
			start_time     INTEGER NOT NULL,
			end_time       INTEGER,
			root_path      TEXT NOT NULL,
			files_scanned  INTEGER DEFAULT 0,
			files_added    INTEGER DEFAULT 0,
			files_updated  INTEGER DEFAULT 0,
			files_removed  INTEGER DEFAULT 0
		);

		CREATE TABLE IF NOT EXISTS files (
			id                INTEGER PRIMARY KEY AUTOINCREMENT,
			path              TEXT UNIQUE NOT NULL,
			filename          TEXT NOT NULL,
			size_bytes        INTEGER NOT NULL DEFAULT 0,
			container         TEXT,
			duration_s        REAL DEFAULT 0,
			overall_bitrate   INTEGER DEFAULT 0,
			original_language TEXT,
			scan_time         INTEGER NOT NULL DEFAULT 0,
			scan_run_id       INTEGER REFERENCES scan_runs(id),
			needs_rescan      INTEGER DEFAULT 0
		);

		CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);

		CREATE TABLE IF NOT EXISTS streams (
			id                  INTEGER PRIMARY KEY AUTOINCREMENT,
			file_id             INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
			stream_index        INTEGER NOT NULL,
			codec_type          TEXT NOT NULL,
			codec_name          TEXT,
			language            TEXT,
			title               TEXT,
			track_type          TEXT DEFAULT 'main',
			type_confidence     REAL DEFAULT 0,
			channels            INTEGER DEFAULT 0,
			sample_rate         INTEGER DEFAULT 0,
			bit_rate            INTEGER DEFAULT 0,
			width               INTEGER DEFAULT 0,
			height              INTEGER DEFAULT 0,
			hdr_format          TEXT,
			is_default          INTEGER DEFAULT 0,
			is_forced           INTEGER DEFAULT 0,
			is_hearing_impaired INTEGER DEFAULT 0,
			is_visual_impaired  INTEGER DEFAULT 0,
			pixel_format        TEXT,
			frame_rate          TEXT,
			codec_level         TEXT,
			codec_profile       TEXT,
			extra_json          TEXT
		);

		CREATE INDEX IF NOT EXISTS idx_streams_file_id ON streams(file_id);

		CREATE TABLE IF NOT EXISTS jobs (
			id                      INTEGER PRIMARY KEY AUTOINCREMENT,
			file_id                 INTEGER REFERENCES files(id),
			status                  TEXT DEFAULT 'queued',
			job_type                TEXT NOT NULL,
			command_args_json       TEXT NOT NULL DEFAULT '[]',
			dry_run                 INTEGER DEFAULT 1,
			created_at              INTEGER NOT NULL DEFAULT 0,
			started_at              INTEGER DEFAULT 0,
			finished_at             INTEGER DEFAULT 0,
			result_code             INTEGER DEFAULT -1,
			output_log              TEXT,
			estimated_saving_bytes  INTEGER DEFAULT 0
		);

		CREATE TABLE IF NOT EXISTS preferences (
			key    TEXT PRIMARY KEY,
			value  TEXT
		);

		CREATE TABLE IF NOT EXISTS codec_hierarchy (
			id             INTEGER PRIMARY KEY AUTOINCREMENT,
			lossless_codec TEXT NOT NULL,
			lossy_codec    TEXT NOT NULL,
			notes          TEXT,
			UNIQUE(lossless_codec, lossy_codec)
		);

		CREATE TABLE IF NOT EXISTS poster_cache (
			file_id    INTEGER PRIMARY KEY REFERENCES files(id) ON DELETE CASCADE,
			source     TEXT NOT NULL DEFAULT '',
			status     TEXT NOT NULL DEFAULT 'pending',
			image_path TEXT NOT NULL DEFAULT '',
			imdb_id    TEXT NOT NULL DEFAULT '',
			fetched_at INTEGER NOT NULL DEFAULT 0
		);

		CREATE TABLE IF NOT EXISTS stream_overrides (
			file_id      INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
			stream_index INTEGER NOT NULL,
			PRIMARY KEY (file_id, stream_index)
		);
	)";

	// Execute each statement separated by semicolons
	const QStringList statements = QString::fromUtf8(schema).split(';', Qt::SkipEmptyParts);
	for (const QString& stmt : statements) {
		const QString trimmed = stmt.trimmed();
		if (trimmed.isEmpty()) continue;
		QSqlQuery q(connection());
		if (!q.exec(trimmed)) {
			qWarning() << "Schema statement failed:" << q.lastError().text() << "\n" << trimmed;
			return false;
		}
	}

	// Seed codec hierarchy if empty
	QSqlQuery check(connection());
	check.exec("SELECT COUNT(*) FROM codec_hierarchy");
	if (check.next() && check.value(0).toInt() == 0) {
		const QList<QPair<QString,QString>> pairs = {
			{"truehd",  "eac3"},
			{"truehd",  "ac3"},
			{"dtshd_ma","dts"},
			{"flac",    "aac"},
			{"flac",    "mp3"},
			{"pcm_s16le","aac"},
			{"pcm_s24le","aac"},
			{"pcm_s32le","aac"},
		};
		QSqlQuery ins(connection());
		ins.prepare("INSERT OR IGNORE INTO codec_hierarchy(lossless_codec,lossy_codec) VALUES(?,?)");
		for (const auto& [lossless, lossy] : pairs) {
			ins.addBindValue(lossless);
			ins.addBindValue(lossy);
			ins.exec();
		}
	}

	// Migration: add columns that didn't exist in the initial schema
	{
		QSqlQuery m(connection());
		m.exec("ALTER TABLE jobs ADD COLUMN summary TEXT DEFAULT ''");
		m.exec("ALTER TABLE jobs ADD COLUMN saved_bytes INTEGER DEFAULT 0");
		m.exec("ALTER TABLE files ADD COLUMN mtime_ms INTEGER DEFAULT 0");
		m.exec("ALTER TABLE files ADD COLUMN created_ms INTEGER DEFAULT 0");
		m.exec("ALTER TABLE jobs ADD COLUMN description_text TEXT DEFAULT ''");
		m.exec("ALTER TABLE jobs ADD COLUMN original_streams_json TEXT DEFAULT ''");
		m.exec("ALTER TABLE jobs ADD COLUMN estimated_saved_bytes INTEGER DEFAULT 0");
		// Ignore errors — they just mean the column already exists
	}

	// Migration: rename job status 'pending' → 'queued' to match UI terminology
	{
		QSqlQuery m(connection());
		m.exec("UPDATE jobs SET status='queued' WHERE status='pending'");
	}

	// Migration: enforce at-most-one active (proposed) job per file.
	// Delete older duplicates first, then create the partial unique index.
	{
		QSqlQuery m(connection());
		m.exec(R"(
			DELETE FROM jobs WHERE id NOT IN (
				SELECT MAX(id) FROM jobs WHERE status='proposed' GROUP BY file_id
			) AND status='proposed'
		)");
		m.exec("CREATE UNIQUE INDEX IF NOT EXISTS idx_jobs_file_proposed ON jobs(file_id) WHERE status='proposed'");
	}

	// Migration: deduplicate done jobs — keep only the most-recent per file.
	{
		QSqlQuery m(connection());
		m.exec(R"(
			DELETE FROM jobs WHERE id NOT IN (
				SELECT MAX(id) FROM jobs WHERE status='done' GROUP BY file_id
			) AND status='done'
		)");
	}

	// Migration: add rating columns to poster_cache
	{
		QSqlQuery m(connection());
		m.exec("ALTER TABLE poster_cache ADD COLUMN vote_average REAL DEFAULT 0.0");
		m.exec("ALTER TABLE poster_cache ADD COLUMN vote_count INTEGER DEFAULT 0");
		m.exec("ALTER TABLE poster_cache ADD COLUMN fanart_path TEXT DEFAULT ''");
		// Ignore errors — column already exists
	}

	// Migration: add title columns to files
	{
		QSqlQuery m(connection());
		m.exec("ALTER TABLE files ADD COLUMN container_title TEXT DEFAULT ''");
		m.exec("ALTER TABLE files ADD COLUMN display_title TEXT DEFAULT ''");
		m.exec("ALTER TABLE files ADD COLUMN display_year INTEGER DEFAULT 0");
	}

	// Migration: add ignored flag to files
	{
		QSqlQuery m(connection());
		m.exec("ALTER TABLE files ADD COLUMN ignored INTEGER NOT NULL DEFAULT 0");
		// Ignore errors — means the column already exists
	}

	// Migration: add per-stream original/commentary disposition flags
	{
		QSqlQuery m(connection());
		m.exec("ALTER TABLE streams ADD COLUMN is_original INTEGER DEFAULT 0");
		m.exec("ALTER TABLE streams ADD COLUMN is_commentary INTEGER DEFAULT 0");
	}

	// Migration: add flag_changes_json to jobs
	{
		QSqlQuery m(connection());
		m.exec("ALTER TABLE jobs ADD COLUMN flag_changes_json TEXT DEFAULT ''");
	}

	// Migration: add sidecar subtitle support
	{
		QSqlQuery m(connection());
		m.exec("ALTER TABLE streams ADD COLUMN is_external INTEGER DEFAULT 0");
		m.exec("ALTER TABLE streams ADD COLUMN external_path TEXT DEFAULT ''");
		m.exec("ALTER TABLE jobs ADD COLUMN sidecar_deletions_json TEXT DEFAULT ''");
	}

	// Migration: add per-track estimate breakdown for calibration
	{
		QSqlQuery m(connection());
		m.exec("ALTER TABLE jobs ADD COLUMN stream_estimates_json TEXT DEFAULT ''");
	}

	// Migration: add codec calibration table (running accuracy ratios per codec)
	{
		QSqlQuery m(connection());
		m.exec(R"(
			CREATE TABLE IF NOT EXISTS codec_calibration (
				codec_name    TEXT    NOT NULL,
				codec_type    TEXT    NOT NULL,
				used_fallback INTEGER NOT NULL DEFAULT 0,
				sample_count  INTEGER NOT NULL DEFAULT 0,
				sum_ratio     REAL    NOT NULL DEFAULT 0.0,
				sum_sq_ratio  REAL    NOT NULL DEFAULT 0.0,
				PRIMARY KEY (codec_name, used_fallback)
			)
		)");
	}

	// One-time migration: reset 'no_poster' records to 'pending' so that newly
	// bundled tools (ffmpeg, mkvextract) get a chance to extract embedded art.
	// Uses a preferences key so this only runs once per installation upgrade.
	{
		QSqlQuery pv(connection());
		pv.exec("SELECT value FROM preferences WHERE key='poster_scan_version'");
		const QString currentVersion = pv.next() ? pv.value(0).toString() : "";
		if (currentVersion != "2") {
			QSqlQuery reset(connection());
			reset.exec("UPDATE poster_cache SET status='pending',source='',image_path='' WHERE status='no_poster'");
			QSqlQuery setv(connection());
			setv.prepare("INSERT OR REPLACE INTO preferences(key,value) VALUES('poster_scan_version','2')");
			setv.exec();
		}
	}

	return true;
}

// ── Scan runs ─────────────────────────────────────────────────────────────────

qint64 DatabaseManager::beginScanRun(const QString& rootPath)
{
	QSqlQuery q(connection());
	q.prepare("INSERT INTO scan_runs(start_time, root_path) VALUES(?, ?)");
	q.addBindValue(QDateTime::currentSecsSinceEpoch());
	q.addBindValue(rootPath);
	if (!q.exec()) {
		qWarning() << "beginScanRun failed:" << q.lastError().text();
		return -1;
	}
	return q.lastInsertId().toLongLong();
}

void DatabaseManager::endScanRun(qint64 scanRunId, int added, int updated, int removed)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE scan_runs SET end_time=?, files_added=?, files_updated=?, files_removed=? WHERE id=?");
	q.addBindValue(QDateTime::currentSecsSinceEpoch());
	q.addBindValue(added);
	q.addBindValue(updated);
	q.addBindValue(removed);
	q.addBindValue(scanRunId);
	q.exec();
}

// ── Files ─────────────────────────────────────────────────────────────────────

std::optional<qint64> DatabaseManager::upsertFile(const FileRecord& rec)
{
	QSqlQuery q(connection());
	q.prepare(R"(
		INSERT INTO files(path, filename, size_bytes, mtime_ms, created_ms, container, duration_s,
						  overall_bitrate, original_language, scan_time, scan_run_id, needs_rescan,
						  container_title)
		VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)
		ON CONFLICT(path) DO UPDATE SET
			filename=excluded.filename,
			size_bytes=excluded.size_bytes,
			mtime_ms=excluded.mtime_ms,
			created_ms=excluded.created_ms,
			container=excluded.container,
			duration_s=excluded.duration_s,
			overall_bitrate=excluded.overall_bitrate,
			original_language=excluded.original_language,
			scan_time=excluded.scan_time,
			scan_run_id=excluded.scan_run_id,
			needs_rescan=excluded.needs_rescan,
			container_title=excluded.container_title
	)");
	q.addBindValue(rec.path);
	q.addBindValue(rec.filename);
	q.addBindValue(rec.sizeBytes);
	q.addBindValue(rec.mtimeMs);
	q.addBindValue(rec.createdMs);
	q.addBindValue(rec.container);
	q.addBindValue(rec.durationSec);
	q.addBindValue(rec.overallBitrate);
	q.addBindValue(rec.originalLanguage);
	q.addBindValue(rec.scanTime);
	q.addBindValue(rec.scanRunId > 0 ? QVariant(rec.scanRunId) : QVariant());
	q.addBindValue(rec.needsRescan ? 1 : 0);
	q.addBindValue(rec.containerTitle);

	if (!q.exec()) {
		qWarning() << "upsertFile failed:" << q.lastError().text();
		return std::nullopt;
	}

	// INSERT returns a new rowid; UPDATE returns 0 for lastInsertId
	const qint64 insertedId = q.lastInsertId().toLongLong();
	if (insertedId > 0) {
		emit fileAdded(insertedId);
		return insertedId;
	}

	// Was an update — fetch the existing id
	QSqlQuery sel(connection());
	sel.prepare("SELECT id FROM files WHERE path=?");
	sel.addBindValue(rec.path);
	if (sel.exec() && sel.next()) {
		const qint64 existingId = sel.value(0).toLongLong();
		emit fileUpdated(existingId);
		return existingId;
	}

	return std::nullopt;
}

std::optional<FileRecord> DatabaseManager::fileById(qint64 id) const
{
	QSqlQuery q(connection());
	q.prepare("SELECT * FROM files WHERE id=?");
	q.addBindValue(id);
	if (!q.exec() || !q.next())
		return std::nullopt;

	FileRecord r;
	r.id               = q.value("id").toLongLong();
	r.path             = q.value("path").toString();
	r.filename         = q.value("filename").toString();
	r.sizeBytes        = q.value("size_bytes").toLongLong();
	r.mtimeMs          = q.value("mtime_ms").toLongLong();
	r.createdMs        = q.value("created_ms").toLongLong();
	r.container        = q.value("container").toString();
	r.durationSec      = q.value("duration_s").toDouble();
	r.overallBitrate   = q.value("overall_bitrate").toLongLong();
	r.originalLanguage = q.value("original_language").toString();
	r.scanTime         = q.value("scan_time").toLongLong();
	r.scanRunId        = q.value("scan_run_id").toLongLong();
	r.needsRescan      = q.value("needs_rescan").toInt() != 0;
	r.containerTitle   = q.value("container_title").toString();
	r.displayTitle     = q.value("display_title").toString();
	r.displayYear      = q.value("display_year").toInt();
	r.ignored          = q.value("ignored").toInt() != 0;
	return r;
}

std::optional<FileRecord> DatabaseManager::fileByPath(const QString& path) const
{
	QSqlQuery q(connection());
	q.prepare("SELECT * FROM files WHERE path=?");
	q.addBindValue(path);
	if (!q.exec() || !q.next())
		return std::nullopt;

	FileRecord r;
	r.id               = q.value("id").toLongLong();
	r.path             = q.value("path").toString();
	r.filename         = q.value("filename").toString();
	r.sizeBytes        = q.value("size_bytes").toLongLong();
	r.mtimeMs          = q.value("mtime_ms").toLongLong();
	r.createdMs        = q.value("created_ms").toLongLong();
	r.container        = q.value("container").toString();
	r.durationSec      = q.value("duration_s").toDouble();
	r.overallBitrate   = q.value("overall_bitrate").toLongLong();
	r.originalLanguage = q.value("original_language").toString();
	r.scanTime         = q.value("scan_time").toLongLong();
	r.needsRescan      = q.value("needs_rescan").toInt() != 0;
	r.containerTitle   = q.value("container_title").toString();
	r.displayTitle     = q.value("display_title").toString();
	r.displayYear      = q.value("display_year").toInt();
	r.ignored          = q.value("ignored").toInt() != 0;
	return r;
}

QList<FileRecord> DatabaseManager::allFiles() const
{
	QList<FileRecord> result;
	QSqlQuery q("SELECT * FROM files ORDER BY filename", m_db);
	while (q.next()) {
		FileRecord r;
		r.id               = q.value("id").toLongLong();
		r.path             = q.value("path").toString();
		r.filename         = q.value("filename").toString();
		r.sizeBytes        = q.value("size_bytes").toLongLong();
		r.mtimeMs          = q.value("mtime_ms").toLongLong();
		r.createdMs        = q.value("created_ms").toLongLong();
		r.container        = q.value("container").toString();
		r.durationSec      = q.value("duration_s").toDouble();
		r.overallBitrate   = q.value("overall_bitrate").toLongLong();
		r.originalLanguage = q.value("original_language").toString();
		r.scanTime         = q.value("scan_time").toLongLong();
		r.needsRescan      = q.value("needs_rescan").toInt() != 0;
		r.containerTitle   = q.value("container_title").toString();
		r.displayTitle     = q.value("display_title").toString();
		r.displayYear      = q.value("display_year").toInt();
		r.ignored          = q.value("ignored").toInt() != 0;
		result.append(r);
	}
	return result;
}

QList<FileRecord> DatabaseManager::allFilesPaged(int offset, int limit) const
{
	QList<FileRecord> result;
	QSqlQuery q(connection());
	q.prepare("SELECT * FROM files ORDER BY filename LIMIT ? OFFSET ?");
	q.addBindValue(limit);
	q.addBindValue(offset);
	if (!q.exec()) return result;
	while (q.next()) {
		FileRecord r;
		r.id               = q.value("id").toLongLong();
		r.path             = q.value("path").toString();
		r.filename         = q.value("filename").toString();
		r.sizeBytes        = q.value("size_bytes").toLongLong();
		r.mtimeMs          = q.value("mtime_ms").toLongLong();
		r.createdMs        = q.value("created_ms").toLongLong();
		r.container        = q.value("container").toString();
		r.durationSec      = q.value("duration_s").toDouble();
		r.overallBitrate   = q.value("overall_bitrate").toLongLong();
		r.originalLanguage = q.value("original_language").toString();
		r.scanTime         = q.value("scan_time").toLongLong();
		r.needsRescan      = q.value("needs_rescan").toInt() != 0;
		r.containerTitle   = q.value("container_title").toString();
		r.displayTitle     = q.value("display_title").toString();
		r.displayYear      = q.value("display_year").toInt();
		r.ignored          = q.value("ignored").toInt() != 0;
		result.append(r);
	}
	return result;
}

QHash<qint64, QList<StreamRecord>> DatabaseManager::streamsForFiles(const QList<qint64>& fileIds) const
{
	QHash<qint64, QList<StreamRecord>> result;
	if (fileIds.isEmpty()) return result;

	QStringList ph;
	ph.reserve(fileIds.size());
	for (int i = 0; i < fileIds.size(); ++i) ph << QStringLiteral("?");

	QSqlQuery q(connection());
	q.prepare(QStringLiteral("SELECT * FROM streams WHERE file_id IN (%1) ORDER BY file_id, stream_index")
	          .arg(ph.join(',')));
	for (qint64 id : fileIds) q.addBindValue(id);
	if (!q.exec()) return result;

	while (q.next()) {
		StreamRecord s;
		s.id                = q.value("id").toLongLong();
		s.fileId            = q.value("file_id").toLongLong();
		s.streamIndex       = q.value("stream_index").toInt();
		s.codecType         = q.value("codec_type").toString();
		s.codecName         = q.value("codec_name").toString();
		s.language          = q.value("language").toString();
		s.title             = q.value("title").toString();
		s.trackType         = q.value("track_type").toString();
		s.typeConfidence    = q.value("type_confidence").toDouble();
		s.channels          = q.value("channels").toInt();
		s.sampleRate        = q.value("sample_rate").toInt();
		s.bitRate           = q.value("bit_rate").toLongLong();
		s.width             = q.value("width").toInt();
		s.height            = q.value("height").toInt();
		s.hdrFormat         = q.value("hdr_format").toString();
		s.isDefault         = q.value("is_default").toInt() != 0;
		s.isForced          = q.value("is_forced").toInt() != 0;
		s.isOriginal        = q.value("is_original").toInt() != 0;
		s.isCommentary      = q.value("is_commentary").toInt() != 0;
		s.isHearingImpaired = q.value("is_hearing_impaired").toInt() != 0;
		s.isVisualImpaired  = q.value("is_visual_impaired").toInt() != 0;
		s.pixelFormat       = q.value("pixel_format").toString();
		s.frameRate         = q.value("frame_rate").toString();
		s.codecLevel        = q.value("codec_level").toString();
		s.codecProfile      = q.value("codec_profile").toString();
		s.extraJson         = q.value("extra_json").toString();
		s.isExternal        = q.value("is_external").toInt() != 0;
		s.externalPath      = q.value("external_path").toString();
		result[s.fileId].append(s);
	}
	return result;
}

QList<FileRecord> DatabaseManager::filesUnderPath(const QString& rootPath) const
{
	QList<FileRecord> result;
	QSqlQuery q(connection());
	const QString norm   = QDir::fromNativeSeparators(rootPath);
	const QString prefix = (norm.endsWith('/') ? norm : norm + '/') + '%';
	q.prepare("SELECT id, path FROM files WHERE path LIKE ?");
	q.addBindValue(prefix);
	if (!q.exec()) return result;
	while (q.next()) {
		FileRecord r;
		r.id   = q.value("id").toLongLong();
		r.path = q.value("path").toString();
		result.append(r);
	}
	return result;
}

int DatabaseManager::fileCount() const
{
	QSqlQuery q(connection());
	q.exec("SELECT COUNT(*) FROM files");
	return q.next() ? q.value(0).toInt() : 0;
}

int DatabaseManager::fileCountUnderPath(const QString& rootPath) const
{
	QSqlQuery q(connection());
	const QString norm   = QDir::fromNativeSeparators(rootPath);
	const QString prefix = (norm.endsWith('/') ? norm : norm + '/') + '%';
	q.prepare("SELECT COUNT(*) FROM files WHERE path LIKE ?");
	q.addBindValue(prefix);
	if (q.exec() && q.next())
		return q.value(0).toInt();
	return 0;
}

int DatabaseManager::removeFilesUnderPath(const QString& rootPath)
{
	QSqlQuery q(connection());
	const QString norm   = QDir::fromNativeSeparators(rootPath);
	const QString prefix = (norm.endsWith('/') ? norm : norm + '/') + '%';
	q.prepare("DELETE FROM files WHERE path LIKE ?");
	q.addBindValue(prefix);
	if (!q.exec()) return 0;
	return q.numRowsAffected();
}

bool DatabaseManager::deleteFile(qint64 fileId)
{
	QSqlQuery q(connection());
	q.prepare("DELETE FROM files WHERE id=?");
	q.addBindValue(fileId);
	const bool ok = q.exec();
	if (ok)
		emit fileDeleted(fileId);
	return ok;
}

void DatabaseManager::updateFilePath(qint64 fileId, const QString& newPath, const QString& newFilename)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE files SET path=?, filename=? WHERE id=?");
	q.addBindValue(newPath);
	q.addBindValue(newFilename);
	q.addBindValue(fileId);
	if (!q.exec())
		qWarning() << "updateFilePath failed:" << q.lastError().text();
}

// ── Streams ───────────────────────────────────────────────────────────────────

bool DatabaseManager::insertStreams(qint64 fileId, const QList<StreamRecord>& streams)
{
	if (!deleteStreamsForFile(fileId))
		return false;

	QSqlQuery q(connection());
	q.prepare(R"(
		INSERT INTO streams(file_id, stream_index, codec_type, codec_name, language, title,
			track_type, type_confidence, channels, sample_rate, bit_rate, width, height,
			hdr_format, is_default, is_forced, is_original, is_commentary,
			is_hearing_impaired, is_visual_impaired,
			pixel_format, frame_rate, codec_level, codec_profile, extra_json,
			is_external, external_path)
		VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
	)");

	connection().transaction();
	for (const auto& s : streams) {
		q.addBindValue(fileId);
		q.addBindValue(s.streamIndex);
		q.addBindValue(s.codecType);
		q.addBindValue(s.codecName);
		q.addBindValue(s.language);
		q.addBindValue(s.title);
		q.addBindValue(s.trackType);
		q.addBindValue(s.typeConfidence);
		q.addBindValue(s.channels);
		q.addBindValue(s.sampleRate);
		q.addBindValue(s.bitRate);
		q.addBindValue(s.width);
		q.addBindValue(s.height);
		q.addBindValue(s.hdrFormat);
		q.addBindValue(s.isDefault ? 1 : 0);
		q.addBindValue(s.isForced ? 1 : 0);
		q.addBindValue(s.isOriginal ? 1 : 0);
		q.addBindValue(s.isCommentary ? 1 : 0);
		q.addBindValue(s.isHearingImpaired ? 1 : 0);
		q.addBindValue(s.isVisualImpaired ? 1 : 0);
		q.addBindValue(s.pixelFormat);
		q.addBindValue(s.frameRate);
		q.addBindValue(s.codecLevel);
		q.addBindValue(s.codecProfile);
		q.addBindValue(s.extraJson);
		q.addBindValue(s.isExternal ? 1 : 0);
		q.addBindValue(s.externalPath);
		if (!q.exec()) {
			qWarning() << "insertStreams row failed:" << q.lastError().text();
			connection().rollback();
			return false;
		}
	}
	connection().commit();
	return true;
}

bool DatabaseManager::deleteStreamsForFile(qint64 fileId)
{
	QSqlQuery q(connection());
	q.prepare("DELETE FROM streams WHERE file_id=?");
	q.addBindValue(fileId);
	return q.exec();
}

QList<StreamRecord> DatabaseManager::streamsForFile(qint64 fileId) const
{
	QList<StreamRecord> result;
	QSqlQuery q(connection());
	q.prepare("SELECT * FROM streams WHERE file_id=? ORDER BY stream_index");
	q.addBindValue(fileId);
	if (!q.exec()) return result;

	while (q.next()) {
		StreamRecord s;
		s.id                = q.value("id").toLongLong();
		s.fileId            = q.value("file_id").toLongLong();
		s.streamIndex       = q.value("stream_index").toInt();
		s.codecType         = q.value("codec_type").toString();
		s.codecName         = q.value("codec_name").toString();
		s.language          = q.value("language").toString();
		s.title             = q.value("title").toString();
		s.trackType         = q.value("track_type").toString();
		s.typeConfidence    = q.value("type_confidence").toDouble();
		s.channels          = q.value("channels").toInt();
		s.sampleRate        = q.value("sample_rate").toInt();
		s.bitRate           = q.value("bit_rate").toLongLong();
		s.width             = q.value("width").toInt();
		s.height            = q.value("height").toInt();
		s.hdrFormat         = q.value("hdr_format").toString();
		s.isDefault         = q.value("is_default").toInt() != 0;
		s.isForced          = q.value("is_forced").toInt() != 0;
		s.isOriginal        = q.value("is_original").toInt() != 0;
		s.isCommentary      = q.value("is_commentary").toInt() != 0;
		s.isHearingImpaired = q.value("is_hearing_impaired").toInt() != 0;
		s.isVisualImpaired  = q.value("is_visual_impaired").toInt() != 0;
		s.pixelFormat       = q.value("pixel_format").toString();
		s.frameRate         = q.value("frame_rate").toString();
		s.codecLevel        = q.value("codec_level").toString();
		s.codecProfile      = q.value("codec_profile").toString();
		s.extraJson         = q.value("extra_json").toString();
		s.isExternal        = q.value("is_external").toInt() != 0;
		s.externalPath      = q.value("external_path").toString();
		result.append(s);
	}
	return result;
}

QHash<qint64, QList<StreamRecord>> DatabaseManager::allStreamsGrouped() const
{
	QHash<qint64, QList<StreamRecord>> result;
	QSqlQuery q(connection());
	q.exec("SELECT * FROM streams ORDER BY file_id, stream_index");
	while (q.next()) {
		StreamRecord s;
		s.id                = q.value("id").toLongLong();
		s.fileId            = q.value("file_id").toLongLong();
		s.streamIndex       = q.value("stream_index").toInt();
		s.codecType         = q.value("codec_type").toString();
		s.codecName         = q.value("codec_name").toString();
		s.language          = q.value("language").toString();
		s.title             = q.value("title").toString();
		s.trackType         = q.value("track_type").toString();
		s.typeConfidence    = q.value("type_confidence").toDouble();
		s.channels          = q.value("channels").toInt();
		s.sampleRate        = q.value("sample_rate").toInt();
		s.bitRate           = q.value("bit_rate").toLongLong();
		s.width             = q.value("width").toInt();
		s.height            = q.value("height").toInt();
		s.hdrFormat         = q.value("hdr_format").toString();
		s.isDefault         = q.value("is_default").toInt() != 0;
		s.isForced          = q.value("is_forced").toInt() != 0;
		s.isOriginal        = q.value("is_original").toInt() != 0;
		s.isCommentary      = q.value("is_commentary").toInt() != 0;
		s.isHearingImpaired = q.value("is_hearing_impaired").toInt() != 0;
		s.isVisualImpaired  = q.value("is_visual_impaired").toInt() != 0;
		s.pixelFormat       = q.value("pixel_format").toString();
		s.frameRate         = q.value("frame_rate").toString();
		s.codecLevel        = q.value("codec_level").toString();
		s.codecProfile      = q.value("codec_profile").toString();
		s.extraJson         = q.value("extra_json").toString();
		s.isExternal        = q.value("is_external").toInt() != 0;
		s.externalPath      = q.value("external_path").toString();
		result[s.fileId].append(s);
	}
	return result;
}

// ── Stream overrides ──────────────────────────────────────────────────────────

void DatabaseManager::setStreamForcedRemoval(qint64 fileId, int streamIndex, bool forced)
{
	QSqlQuery q(connection());
	if (forced) {
		q.prepare("INSERT OR IGNORE INTO stream_overrides (file_id, stream_index) VALUES (?, ?)");
		q.addBindValue(fileId);
		q.addBindValue(streamIndex);
	} else {
		q.prepare("DELETE FROM stream_overrides WHERE file_id = ? AND stream_index = ?");
		q.addBindValue(fileId);
		q.addBindValue(streamIndex);
	}
	if (!q.exec())
		qWarning() << "[ForcedRemoval] setStreamForcedRemoval failed:" << q.lastError().text()
		           << "fileId=" << fileId << "streamIndex=" << streamIndex << "forced=" << forced;
	else
		qDebug() << "[ForcedRemoval] DB write OK fileId=" << fileId << "streamIndex=" << streamIndex << "forced=" << forced;
}

QHash<qint64, QSet<int>> DatabaseManager::allStreamForcedRemovals() const
{
	QHash<qint64, QSet<int>> result;
	QSqlQuery q(connection());
	if (!q.exec("SELECT file_id, stream_index FROM stream_overrides"))
		qWarning() << "[ForcedRemoval] allStreamForcedRemovals SELECT failed:" << q.lastError().text();
	while (q.next())
		result[q.value(0).toLongLong()].insert(q.value(1).toInt());
	return result;
}

void DatabaseManager::clearStreamForcedRemovals(qint64 fileId)
{
	QSqlQuery q(connection());
	q.prepare("DELETE FROM stream_overrides WHERE file_id = ?");
	q.addBindValue(fileId);
	q.exec();
}

// ── Jobs ──────────────────────────────────────────────────────────────────────

bool DatabaseManager::hasActiveJobForFile(qint64 fileId) const
{
	QSqlQuery q(connection());
	q.prepare("SELECT 1 FROM jobs WHERE file_id=? AND status IN ('proposed','queued','running') LIMIT 1");
	q.addBindValue(fileId);
	return q.exec() && q.next();
}

std::optional<JobRecord> DatabaseManager::activeJobForFile(qint64 fileId) const
{
	QSqlQuery q(connection());
	q.prepare("SELECT * FROM jobs WHERE file_id=? AND status IN ('proposed','queued') ORDER BY created_at DESC LIMIT 1");
	q.addBindValue(fileId);
	if (!q.exec() || !q.next()) return std::nullopt;
	JobRecord j;
	j.id                  = q.value("id").toLongLong();
	j.fileId              = q.value("file_id").toLongLong();
	j.status              = q.value("status").toString();
	j.jobType             = q.value("job_type").toString();
	j.commandArgsJson     = q.value("command_args_json").toString();
	j.summary             = q.value("summary").toString();
	j.dryRun              = q.value("dry_run").toInt() != 0;
	j.createdAt           = q.value("created_at").toLongLong();
	j.startedAt           = q.value("started_at").toLongLong();
	j.finishedAt          = q.value("finished_at").toLongLong();
	j.resultCode          = q.value("result_code").toInt();
	j.outputLog              = q.value("output_log").toString();
	j.savedBytes             = q.value("saved_bytes").toLongLong();
	j.estimatedSavedBytes    = q.value("estimated_saved_bytes").toLongLong();
	j.descriptionText        = q.value("description_text").toString();
	j.originalStreamsJson    = q.value("original_streams_json").toString();
	j.flagChangesJson        = q.value("flag_changes_json").toString();
	j.sidecarDeletionsJson   = q.value("sidecar_deletions_json").toString();
	j.streamEstimatesJson    = q.value("stream_estimates_json").toString();
	return j;
}

qint64 DatabaseManager::insertJob(const JobRecord& job)
{
	QSqlQuery q(connection());
	q.prepare(R"(
		INSERT INTO jobs(file_id, status, job_type, command_args_json,
						 summary, dry_run, created_at, description_text, original_streams_json,
						 saved_bytes, estimated_saved_bytes, flag_changes_json, sidecar_deletions_json,
						 stream_estimates_json)
		VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)
	)");
	q.addBindValue(job.fileId > 0 ? QVariant(job.fileId) : QVariant());
	q.addBindValue(job.status.isEmpty() ? "proposed" : job.status);
	q.addBindValue(job.jobType.isEmpty() ? "remux" : job.jobType);
	q.addBindValue(job.commandArgsJson);
	q.addBindValue(job.summary);
	q.addBindValue(job.dryRun ? 1 : 0);
	q.addBindValue(QDateTime::currentSecsSinceEpoch());
	q.addBindValue(job.descriptionText);
	q.addBindValue(job.originalStreamsJson);
	q.addBindValue(job.savedBytes);           // estimate at creation; overwritten on completion
	q.addBindValue(job.estimatedSavedBytes);  // frozen at creation, never overwritten
	q.addBindValue(job.flagChangesJson);
	q.addBindValue(job.sidecarDeletionsJson);
	q.addBindValue(job.streamEstimatesJson);
	if (!q.exec()) {
		qWarning() << "insertJob failed:" << q.lastError().text();
		return -1;
	}
	return q.lastInsertId().toLongLong();
}

bool DatabaseManager::updateJobStatus(qint64 jobId, const QString& status, int resultCode, const QString& log)
{
	QSqlQuery q(connection());
	if (status == "running") {
		q.prepare("UPDATE jobs SET status=?, started_at=? WHERE id=?");
		q.addBindValue(status);
		q.addBindValue(QDateTime::currentSecsSinceEpoch());
		q.addBindValue(jobId);
	} else {
		q.prepare("UPDATE jobs SET status=?, finished_at=?, result_code=?, output_log=? WHERE id=?");
		q.addBindValue(status);
		q.addBindValue(QDateTime::currentSecsSinceEpoch());
		q.addBindValue(resultCode);
		q.addBindValue(log);
		q.addBindValue(jobId);
	}
	const bool ok = q.exec();
	if (ok) {
		emit jobStatusChanged(jobId, status);
		if (status == QLatin1String("done")) {
			// Keep only the most-recent done job per file — drop all older ones now.
			QSqlQuery prune(connection());
			prune.prepare(
				"DELETE FROM jobs WHERE status='done'"
				" AND file_id=(SELECT file_id FROM jobs WHERE id=?)"
				" AND id!=?");
			prune.addBindValue(jobId);
			prune.addBindValue(jobId);
			prune.exec();
		}
	}
	return ok;
}

bool DatabaseManager::updateJobSavedBytes(qint64 jobId, qint64 savedBytes)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE jobs SET saved_bytes=? WHERE id=?");
	q.addBindValue(savedBytes);
	q.addBindValue(jobId);
	return q.exec();
}

void DatabaseManager::updateCalibrationFromJob(qint64 jobId)
{
	const auto jobOpt = jobById(jobId);
	if (!jobOpt) return;
	const JobRecord& job = *jobOpt;
	if (job.savedBytes <= 0 || job.estimatedSavedBytes <= 0) return;
	if (job.streamEstimatesJson.isEmpty()) return;

	const QJsonArray tracks = QJsonDocument::fromJson(job.streamEstimatesJson.toUtf8()).array();
	if (tracks.isEmpty()) return;

	// One accuracy ratio per job, applied to every track removed in that job.
	// For multi-track jobs this is an approximation; single-track jobs give an
	// exact signal. Over many jobs the average converges per codec family.
	const double ratio = static_cast<double>(job.savedBytes)
	                   / static_cast<double>(job.estimatedSavedBytes);

	QSqlQuery q(connection());
	q.prepare(R"(
		INSERT INTO codec_calibration(codec_name, codec_type, used_fallback,
		                              sample_count, sum_ratio, sum_sq_ratio)
		VALUES(?, ?, ?, 1, ?, ?)
		ON CONFLICT(codec_name, used_fallback) DO UPDATE SET
			sample_count = sample_count + 1,
			sum_ratio    = sum_ratio    + excluded.sum_ratio,
			sum_sq_ratio = sum_sq_ratio + excluded.sum_sq_ratio
	)");

	for (const QJsonValue& v : tracks) {
		const QJsonObject obj   = v.toObject();
		const QString codecName = obj[QStringLiteral("codecName")].toString();
		const QString codecType = obj[QStringLiteral("codecType")].toString();
		const int usedFallback  = obj[QStringLiteral("declaredBitrate")].toVariant().toLongLong() == 0 ? 1 : 0;
		q.addBindValue(codecName);
		q.addBindValue(codecType);
		q.addBindValue(usedFallback);
		q.addBindValue(ratio);
		q.addBindValue(ratio * ratio);
		if (!q.exec())
			qWarning() << "updateCalibrationFromJob failed:" << q.lastError().text();
	}
}

QList<Mc::CalibrationEntry> DatabaseManager::calibrationReport() const
{
	QList<Mc::CalibrationEntry> result;
	QSqlQuery q(connection());
	q.exec(R"(
		SELECT codec_name, codec_type, used_fallback, sample_count,
		       sum_ratio / sample_count AS avg_ratio,
		       CASE WHEN sample_count > 1
		            THEN SQRT(MAX(0.0,
		                 sum_sq_ratio / sample_count
		                 - (sum_ratio / sample_count) * (sum_ratio / sample_count)))
		            ELSE 0.0 END AS std_dev_ratio
		FROM codec_calibration
		ORDER BY codec_type, codec_name, used_fallback
	)");
	while (q.next()) {
		Mc::CalibrationEntry e;
		e.codecName    = q.value("codec_name").toString();
		e.codecType    = q.value("codec_type").toString();
		e.usedFallback = q.value("used_fallback").toInt() != 0;
		e.sampleCount  = q.value("sample_count").toInt();
		e.avgRatio     = q.value("avg_ratio").toDouble();
		e.stdDevRatio  = q.value("std_dev_ratio").toDouble();
		result.append(e);
	}
	return result;
}

void DatabaseManager::clearCalibration()
{
	QSqlQuery q(connection());
	q.exec("DELETE FROM codec_calibration");
}

bool DatabaseManager::updateJobType(qint64 jobId, const QString& jobType)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE jobs SET job_type=? WHERE id=? AND status IN ('proposed','queued')");
	q.addBindValue(jobType);
	q.addBindValue(jobId);
	return q.exec();
}

bool DatabaseManager::updateJobCommandArgs(qint64 jobId, const QString& commandArgsJson)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE jobs SET command_args_json=? WHERE id=? AND status IN ('proposed','queued')");
	q.addBindValue(commandArgsJson);
	q.addBindValue(jobId);
	return q.exec();
}

bool DatabaseManager::updateJobFlagChanges(qint64 jobId, const QString& flagChangesJson)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE jobs SET flag_changes_json=? WHERE id=? AND status IN ('proposed','queued')");
	q.addBindValue(flagChangesJson);
	q.addBindValue(jobId);
	return q.exec();
}

bool DatabaseManager::updateJobSidecarDeletions(qint64 jobId, const QString& sidecarDeletionsJson)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE jobs SET sidecar_deletions_json=? WHERE id=? AND status IN ('proposed','queued')");
	q.addBindValue(sidecarDeletionsJson);
	q.addBindValue(jobId);
	return q.exec();
}

bool DatabaseManager::updateJobSummary(qint64 jobId, const QString& summary)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE jobs SET summary=? WHERE id=? AND status IN ('proposed','queued')");
	q.addBindValue(summary);
	q.addBindValue(jobId);
	return q.exec();
}

bool DatabaseManager::deleteJob(qint64 jobId)
{
	QSqlQuery q(connection());
	q.prepare("DELETE FROM jobs WHERE id=?");
	q.addBindValue(jobId);
	return q.exec();
}

bool DatabaseManager::deleteJobsBatch(const QList<qint64>& jobIds)
{
	if (jobIds.isEmpty()) return true;
	auto db = connection();
	db.transaction();
	QSqlQuery q(db);
	q.prepare("DELETE FROM jobs WHERE id=?");
	for (qint64 id : jobIds) {
		q.addBindValue(id);
		if (!q.exec()) { db.rollback(); return false; }
	}
	return db.commit();
}

bool DatabaseManager::clearJobsByStatus(const QString& status)
{
	QSqlQuery q(connection());
	q.prepare("DELETE FROM jobs WHERE status=?");
	q.addBindValue(status);
	return q.exec();
}

bool DatabaseManager::promoteJobsToQueued(const QList<qint64>& jobIds)
{
	if (jobIds.isEmpty()) return true;
	connection().transaction();
	QSqlQuery q(connection());
	q.prepare("UPDATE jobs SET status='queued' WHERE id=? AND status='proposed'");
	for (qint64 id : jobIds) {
		q.addBindValue(id);
		if (!q.exec()) { connection().rollback(); return false; }
		emit jobStatusChanged(id, "queued");
	}
	connection().commit();
	return true;
}

bool DatabaseManager::requeueFailedJobs(const QList<qint64>& jobIds)
{
	if (jobIds.isEmpty()) return true;
	auto db = connection();
	db.transaction();
	QSqlQuery q(db);
	q.prepare("UPDATE jobs SET status='queued', started_at=NULL, finished_at=NULL, "
	          "result_code=NULL, output_log=NULL WHERE id=? AND status IN ('failed','running')");
	for (qint64 id : jobIds) {
		q.addBindValue(id);
		if (!q.exec()) { db.rollback(); return false; }
		emit jobStatusChanged(id, "queued");
	}
	return db.commit();
}

bool DatabaseManager::recoverRunningJobs()
{
	QSqlQuery q(connection());
	// Both 'running' and 'needs_review' mean the app was closed mid-job.
	// The .tmp file (if any) is orphaned on disk — mark as failed so user can retry.
	q.prepare("UPDATE jobs SET status='failed', finished_at=UNIXEPOCH(), result_code=-1 "
	          "WHERE status IN ('running', 'needs_review')");
	return q.exec();
}

bool DatabaseManager::updateFileOriginalLanguage(qint64 fileId, const QString& lang)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE files SET original_language=? WHERE id=?");
	q.addBindValue(lang);
	q.addBindValue(fileId);
	return q.exec();
}

bool DatabaseManager::updateDisplayTitle(qint64 fileId, const QString& title, int year)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE files SET display_title=?, display_year=? WHERE id=?");
	q.addBindValue(title);
	q.addBindValue(year);
	q.addBindValue(fileId);
	return q.exec();
}

bool DatabaseManager::setFileIgnored(qint64 fileId, bool ignored)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE files SET ignored=? WHERE id=?");
	q.addBindValue(ignored ? 1 : 0);
	q.addBindValue(fileId);
	return q.exec();
}

void DatabaseManager::deleteJobsForFile(qint64 fileId)
{
	QSqlQuery q(connection());
	q.prepare("DELETE FROM jobs WHERE file_id=? AND status != 'running'");
	q.addBindValue(fileId);
	q.exec();
}

void DatabaseManager::deletePendingJobsForFile(qint64 fileId)
{
	QSqlQuery q(connection());
	q.prepare("DELETE FROM jobs WHERE file_id=? AND status IN ('proposed','queued')");
	q.addBindValue(fileId);
	q.exec();
}

QList<JobRecord> DatabaseManager::queuedJobs(JobSortMode sortMode) const
{
	QList<JobRecord> result;
	QSqlQuery q(connection());
	const QString orderBy = (sortMode == JobSortMode::LargestSavingsFirst)
		? QStringLiteral("j.saved_bytes DESC")
		: QStringLiteral("COALESCE(f.size_bytes, 0) ASC");
	const QString sql = QStringLiteral(
		"SELECT j.id, j.file_id, j.status, j.job_type, j.command_args_json,"
		"       j.summary, j.dry_run, j.created_at, j.started_at, j.finished_at,"
		"       j.result_code, j.output_log, j.saved_bytes, j.estimated_saved_bytes,"
		"       j.description_text, j.flag_changes_json, j.sidecar_deletions_json"
		" FROM jobs j LEFT JOIN files f ON j.file_id = f.id"
		" WHERE j.status = 'queued'"
		" ORDER BY %1").arg(orderBy);
	q.exec(sql);
	while (q.next()) {
		JobRecord j;
		j.id               = q.value("id").toLongLong();
		j.fileId           = q.value("file_id").toLongLong();
		j.status           = q.value("status").toString();
		j.jobType          = q.value("job_type").toString();
		j.commandArgsJson  = q.value("command_args_json").toString();
		j.summary          = q.value("summary").toString();
		j.dryRun           = q.value("dry_run").toInt() != 0;
		j.createdAt        = q.value("created_at").toLongLong();
		j.startedAt        = q.value("started_at").toLongLong();
		j.finishedAt       = q.value("finished_at").toLongLong();
		j.resultCode             = q.value("result_code").toInt();
		j.outputLog              = q.value("output_log").toString();
		j.savedBytes             = q.value("saved_bytes").toLongLong();
		j.estimatedSavedBytes    = q.value("estimated_saved_bytes").toLongLong();
		j.descriptionText        = q.value("description_text").toString();
		j.flagChangesJson        = q.value("flag_changes_json").toString();
		j.sidecarDeletionsJson   = q.value("sidecar_deletions_json").toString();
		result.append(j);
	}
	return result;
}

QList<JobRecord> DatabaseManager::allJobs() const
{
	QList<JobRecord> result;
	QSqlQuery q(connection());
	q.exec("SELECT * FROM jobs ORDER BY created_at DESC");
	while (q.next()) {
		JobRecord j;
		j.id               = q.value("id").toLongLong();
		j.fileId           = q.value("file_id").toLongLong();
		j.status           = q.value("status").toString();
		j.jobType          = q.value("job_type").toString();
		j.commandArgsJson  = q.value("command_args_json").toString();
		j.summary          = q.value("summary").toString();
		j.dryRun           = q.value("dry_run").toInt() != 0;
		j.createdAt        = q.value("created_at").toLongLong();
		j.startedAt        = q.value("started_at").toLongLong();
		j.finishedAt       = q.value("finished_at").toLongLong();
		j.resultCode       = q.value("result_code").toInt();
		j.outputLog        = q.value("output_log").toString();
		j.savedBytes            = q.value("saved_bytes").toLongLong();
		j.descriptionText       = q.value("description_text").toString();
		j.originalStreamsJson   = q.value("original_streams_json").toString();
		j.flagChangesJson       = q.value("flag_changes_json").toString();
		j.sidecarDeletionsJson  = q.value("sidecar_deletions_json").toString();
		j.streamEstimatesJson   = q.value("stream_estimates_json").toString();
		result.append(j);
	}
	return result;
}

std::optional<JobRecord> DatabaseManager::jobById(qint64 jobId) const
{
	QSqlQuery q(connection());
	q.prepare("SELECT * FROM jobs WHERE id = ?");
	q.addBindValue(jobId);
	if (!q.exec() || !q.next()) return std::nullopt;
	JobRecord j;
	j.id                  = q.value("id").toLongLong();
	j.fileId              = q.value("file_id").toLongLong();
	j.status              = q.value("status").toString();
	j.jobType             = q.value("job_type").toString();
	j.commandArgsJson     = q.value("command_args_json").toString();
	j.summary             = q.value("summary").toString();
	j.dryRun              = q.value("dry_run").toInt() != 0;
	j.createdAt           = q.value("created_at").toLongLong();
	j.startedAt           = q.value("started_at").toLongLong();
	j.finishedAt          = q.value("finished_at").toLongLong();
	j.resultCode             = q.value("result_code").toInt();
	j.outputLog              = q.value("output_log").toString();
	j.savedBytes             = q.value("saved_bytes").toLongLong();
	j.estimatedSavedBytes    = q.value("estimated_saved_bytes").toLongLong();
	j.descriptionText        = q.value("description_text").toString();
	j.originalStreamsJson    = q.value("original_streams_json").toString();
	j.flagChangesJson        = q.value("flag_changes_json").toString();
	j.sidecarDeletionsJson   = q.value("sidecar_deletions_json").toString();
	j.streamEstimatesJson    = q.value("stream_estimates_json").toString();
	return j;
}

static void parseJobDisplayRecord(QSqlQuery& q, QList<Mc::JobDisplayRecord>& result)
{
	while (q.next()) {
		Mc::JobDisplayRecord r;
		r.jobId       = q.value(0).toLongLong();
		r.fileId      = q.value(1).toLongLong();
		r.summary     = q.value(2).toString();
		r.status      = q.value(3).toString();
		r.savedBytes  = q.value(4).toLongLong();
		r.createdAt   = q.value(5).toLongLong();
		r.filename    = q.value(6).toString();
		r.filePath    = q.value(7).toString();
		r.sizeBytes   = q.value(8).toLongLong();
		r.imdbId            = q.value(9).toString();
		r.jobType            = q.value("job_type").toString();
		r.durationSec        = q.value("duration_s").toDouble();
		r.voteAverage        = q.value("vote_average").toDouble();
		r.originalLanguage   = q.value("original_language").toString();
		r.commandArgsJson    = q.value("command_args_json").toString();
		r.originalStreamsJson = q.value("original_streams_json").toString();
		r.flagChangesJson    = q.value("flag_changes_json").toString();
		r.finishedAt         = q.value("finished_at").toLongLong();
		result.append(r);
	}
}

QList<JobDisplayRecord> DatabaseManager::allJobsForPanel(JobSortMode sortMode) const
{
	QList<JobDisplayRecord> result;
	QSqlQuery q(connection());
	const QString orderBy =
		(sortMode == JobSortMode::LargestSavingsFirst) ? QStringLiteral("j.saved_bytes DESC, j.created_at ASC") :
		(sortMode == JobSortMode::MostRecentFirst)     ? QStringLiteral("j.finished_at DESC, j.id DESC") :
		                                                 QStringLiteral("COALESCE(f.size_bytes,0) ASC, j.created_at ASC");
	const QString sql = QStringLiteral(
		"SELECT j.id, j.file_id, j.summary, j.status, j.saved_bytes, j.created_at,"
		"       f.filename, f.path, COALESCE(f.size_bytes, 0) AS size_bytes,"
		"       COALESCE(pc.imdb_id, '') AS imdb_id,"
		"       j.job_type,"
		"       COALESCE(f.duration_s, 0.0) AS duration_s,"
		"       COALESCE(pc.vote_average, 0.0) AS vote_average,"
		"       COALESCE(f.original_language, '') AS original_language,"
		"       COALESCE(j.command_args_json, '[]') AS command_args_json,"
		"       COALESCE(j.original_streams_json, '') AS original_streams_json,"
		"       COALESCE(j.flag_changes_json, '') AS flag_changes_json,"
		"       COALESCE(j.finished_at, 0) AS finished_at"
		" FROM jobs j LEFT JOIN files f ON j.file_id = f.id"
		" LEFT JOIN poster_cache pc ON j.file_id = pc.file_id"
		" ORDER BY %1").arg(orderBy);
	q.exec(sql);
	if (!q.isActive())
		qWarning() << "allJobsForPanel query failed:" << q.lastError().text();
	parseJobDisplayRecord(q, result);
	return result;
}

QList<JobDisplayRecord> DatabaseManager::allJobsForPanelPaged(int limit, const QString& statusFilter, JobSortMode sortMode) const
{
	QList<JobDisplayRecord> result;
	QSqlQuery q(connection());
	const QString orderBy =
		(sortMode == JobSortMode::LargestSavingsFirst) ? QStringLiteral("j.saved_bytes DESC, j.created_at ASC") :
		(sortMode == JobSortMode::MostRecentFirst)     ? QStringLiteral("j.finished_at DESC, j.id DESC") :
		                                                 QStringLiteral("COALESCE(f.size_bytes,0) ASC, j.created_at ASC");
	QString sql = QStringLiteral(
		"SELECT j.id, j.file_id, j.summary, j.status, j.saved_bytes, j.created_at,"
		"       f.filename, f.path, COALESCE(f.size_bytes, 0) AS size_bytes,"
		"       COALESCE(pc.imdb_id, '') AS imdb_id,"
		"       j.job_type,"
		"       COALESCE(f.duration_s, 0.0) AS duration_s,"
		"       COALESCE(pc.vote_average, 0.0) AS vote_average,"
		"       COALESCE(f.original_language, '') AS original_language,"
		"       COALESCE(j.command_args_json, '[]') AS command_args_json,"
		"       COALESCE(j.original_streams_json, '') AS original_streams_json,"
		"       COALESCE(j.flag_changes_json, '') AS flag_changes_json,"
		"       COALESCE(j.finished_at, 0) AS finished_at"
		" FROM jobs j LEFT JOIN files f ON j.file_id = f.id"
		" LEFT JOIN poster_cache pc ON j.file_id = pc.file_id");
	if (!statusFilter.isEmpty())
		sql += QStringLiteral(" WHERE j.status = ?");
	sql += QStringLiteral(" ORDER BY %1 LIMIT ?").arg(orderBy);
	q.prepare(sql);
	if (!statusFilter.isEmpty()) q.addBindValue(statusFilter);
	q.addBindValue(limit);
	q.exec();
	parseJobDisplayRecord(q, result);
	return result;
}

int DatabaseManager::totalJobCount() const
{
	QSqlQuery q(connection());
	q.exec("SELECT COUNT(*) FROM jobs WHERE status NOT IN ('done','cancelled')");
	return q.next() ? q.value(0).toInt() : 0;
}

int DatabaseManager::queuedJobCount() const
{
	QSqlQuery q(connection());
	q.exec("SELECT COUNT(*) FROM jobs WHERE status='queued'");
	return q.next() ? q.value(0).toInt() : 0;
}

QHash<QString, int> DatabaseManager::jobStatusCounts() const
{
	QHash<QString, int> result;
	QSqlQuery q(connection());
	q.exec("SELECT status, COUNT(*) FROM jobs GROUP BY status");
	while (q.next())
		result.insert(q.value(0).toString(), q.value(1).toInt());
	return result;
}

QSet<qint64> DatabaseManager::proposedJobFileIds() const
{
	QSet<qint64> result;
	QSqlQuery q(connection());
	q.exec("SELECT file_id FROM jobs WHERE status='proposed'");
	while (q.next())
		result.insert(q.value(0).toLongLong());
	return result;
}

// ── Preferences ───────────────────────────────────────────────────────────────

bool DatabaseManager::setPref(const QString& key, const QString& value)
{
	QSqlQuery q(connection());
	q.prepare("INSERT OR REPLACE INTO preferences(key,value) VALUES(?,?)");
	q.addBindValue(key);
	q.addBindValue(value);
	return q.exec();
}

QString DatabaseManager::getPref(const QString& key, const QString& defaultValue) const
{
	QSqlQuery q(connection());
	q.prepare("SELECT value FROM preferences WHERE key=?");
	q.addBindValue(key);
	if (q.exec() && q.next())
		return q.value(0).toString();
	return defaultValue;
}

// ── Poster cache ──────────────────────────────────────────────────────────────

void DatabaseManager::upsertPosterRecord(const PosterRecord& rec)
{
	QSqlQuery q(connection());
	q.prepare(R"(
		INSERT INTO poster_cache(file_id, source, status, image_path, fanart_path, imdb_id, fetched_at, vote_average, vote_count)
		VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT(file_id) DO UPDATE SET
			source=excluded.source,
			status=excluded.status,
			image_path=excluded.image_path,
			fanart_path=CASE WHEN excluded.fanart_path != '' THEN excluded.fanart_path ELSE fanart_path END,
			imdb_id=excluded.imdb_id,
			fetched_at=excluded.fetched_at,
			vote_average=CASE WHEN excluded.vote_average > 0 THEN excluded.vote_average ELSE vote_average END,
			vote_count=CASE WHEN excluded.vote_count > 0 THEN excluded.vote_count ELSE vote_count END
	)");
	// Bind empty string (not null) — Qt maps null QString → SQL NULL which violates NOT NULL
	auto nn = [](const QString& s) { return s.isNull() ? QString("") : s; };
	q.addBindValue(rec.fileId);
	q.addBindValue(nn(rec.source));
	q.addBindValue(nn(rec.status));
	q.addBindValue(nn(rec.imagePath));
	q.addBindValue(nn(rec.fanartPath));
	q.addBindValue(nn(rec.imdbId));
	q.addBindValue(rec.fetchedAt);
	q.addBindValue(rec.voteAverage);
	q.addBindValue(rec.voteCount);
	if (!q.exec())
		qWarning() << "upsertPosterRecord failed:" << q.lastError().text();
}

std::optional<PosterRecord> DatabaseManager::posterForFile(qint64 fileId) const
{
	QSqlQuery q(connection());
	q.prepare("SELECT source,status,image_path,fanart_path,imdb_id,fetched_at,vote_average,vote_count FROM poster_cache WHERE file_id=?");
	q.addBindValue(fileId);
	if (!q.exec() || !q.next()) return {};
	PosterRecord r;
	r.fileId      = fileId;
	r.source      = q.value(0).toString();
	r.status      = q.value(1).toString();
	r.imagePath   = q.value(2).toString();
	r.fanartPath  = q.value(3).toString();
	r.imdbId      = q.value(4).toString();
	r.fetchedAt   = q.value(5).toLongLong();
	r.voteAverage = q.value(6).toDouble();
	r.voteCount   = q.value(7).toInt();
	return r;
}

QList<qint64> DatabaseManager::fileIdsNeedingPosters() const
{
	QSqlQuery q(connection());
	q.exec(R"(
		SELECT f.id FROM files f
		LEFT JOIN poster_cache pc ON pc.file_id = f.id
		WHERE pc.file_id IS NULL
		   OR pc.status = 'pending'
		   OR (pc.status = 'done' AND pc.imdb_id != '' AND (
		           pc.vote_average = 0
		        OR f.display_title = ''
		        OR f.display_title IS NULL
		        OR pc.fanart_path = ''
		        OR pc.fanart_path IS NULL
		   ))
		   OR (pc.status = 'no_poster' AND (
		           f.display_title = ''
		        OR f.display_title IS NULL
		        OR (pc.imdb_id != '' AND pc.vote_average = 0)
		   ))
		ORDER BY f.filename COLLATE NOCASE
	)");
	QList<qint64> ids;
	while (q.next()) ids << q.value(0).toLongLong();
	return ids;
}

void DatabaseManager::resetPosterForFile(qint64 fileId)
{
	QSqlQuery q(connection());
	// ON CONFLICT preserves imdb_id — only the poster image fields are reset.
	q.prepare(R"(
		INSERT INTO poster_cache(file_id, source, status, image_path, imdb_id, fetched_at)
		VALUES(?, '', 'pending', '', '', 0)
		ON CONFLICT(file_id) DO UPDATE SET
			source = '', status = 'pending', image_path = '', fetched_at = 0
	)");
	q.addBindValue(fileId);
	q.exec();
}

void DatabaseManager::updateImdbId(qint64 fileId, const QString& imdbId)
{
	QSqlQuery q(connection());
	// Upsert a minimal row if none exists, or update only imdb_id on conflict.
	q.prepare(R"(
		INSERT INTO poster_cache(file_id, source, status, image_path, imdb_id, fetched_at)
		VALUES(?, '', 'pending', '', ?, 0)
		ON CONFLICT(file_id) DO UPDATE SET imdb_id = excluded.imdb_id
	)");
	q.addBindValue(fileId);
	q.addBindValue(imdbId);
	if (!q.exec())
		qWarning() << "updateImdbId failed:" << q.lastError().text();
}

void DatabaseManager::clearPosterPath(const QString& imagePath)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE poster_cache SET status='pending', image_path='' WHERE image_path=?");
	q.addBindValue(imagePath);
	q.exec();
}

void DatabaseManager::clearFanartPath(const QString& fanartPath)
{
	QSqlQuery q(connection());
	q.prepare("UPDATE poster_cache SET fanart_path='' WHERE fanart_path=?");
	q.addBindValue(fanartPath);
	q.exec();
}

void DatabaseManager::resetNoPosterRecords()
{
	QSqlQuery q(connection());
	q.exec("UPDATE poster_cache SET status='pending',source='',image_path='' WHERE status='no_poster'");
}

QHash<qint64, QString> DatabaseManager::allDonePosterPaths() const
{
	QSqlQuery q(connection());
	q.exec("SELECT file_id, image_path FROM poster_cache WHERE status='done' AND image_path != ''");
	QHash<qint64, QString> result;
	while (q.next())
		result.insert(q.value(0).toLongLong(), q.value(1).toString());
	return result;
}

QHash<qint64, QString> DatabaseManager::allDoneFanartPaths() const
{
	QSqlQuery q(connection());
	q.exec("SELECT file_id, fanart_path FROM poster_cache WHERE fanart_path != ''");
	QHash<qint64, QString> result;
	while (q.next())
		result.insert(q.value(0).toLongLong(), q.value(1).toString());
	return result;
}

QHash<qint64, QString> DatabaseManager::allKnownImdbIds() const
{
	QSqlQuery q(connection());
	q.exec("SELECT file_id, imdb_id FROM poster_cache WHERE imdb_id != ''");
	QHash<qint64, QString> result;
	while (q.next())
		result.insert(q.value(0).toLongLong(), q.value(1).toString());
	return result;
}

QHash<qint64, double> DatabaseManager::allRatings() const
{
	QSqlQuery q(connection());
	q.exec("SELECT file_id, vote_average FROM poster_cache WHERE vote_average > 0");
	QHash<qint64, double> result;
	while (q.next())
		result.insert(q.value(0).toLongLong(), q.value(1).toDouble());
	return result;
}

void DatabaseManager::cleanupStalledJobs()
{
	QSqlQuery q(connection());
	// Find all jobs that were 'running' when the app last crashed
	q.exec("SELECT id, command_args_json FROM jobs WHERE status='running'");
	QList<QPair<qint64, QString>> stalled;
	while (q.next())
		stalled.append({q.value(0).toLongLong(), q.value(1).toString()});

	if (stalled.isEmpty()) return;

	for (const auto& [jobId, argsJson] : stalled) {
		// Parse the command args to find the temp output path (arg after "-o")
		const QJsonArray arr = QJsonDocument::fromJson(argsJson.toUtf8()).array();
		for (int i = 0; i + 1 < arr.size(); ++i) {
			if (arr.at(i).toString() == "-o") {
				const QString tmpPath = arr.at(i + 1).toString();
				if (!tmpPath.isEmpty() && QFile::exists(tmpPath)) {
					QFile::remove(tmpPath);
					qDebug() << "Removed stalled temp file:" << tmpPath;
				}
				break;
			}
		}
	}

	// Reset all stalled jobs to failed
	QSqlQuery upd(connection());
	upd.exec("UPDATE jobs SET status='failed', result_code=-1 WHERE status='running'");
}

} // namespace Mc
