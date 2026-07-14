#pragma once

#include <QAtomicInt>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

namespace Mc {

struct StreamRecord;

/**
 * OpenSubtitlesClient — blocking HTTP client for the OpenSubtitles.com REST API v1.
 *
 * Must run on a QThread (not a plain thread-pool worker) because it owns a
 * QNetworkAccessManager and uses QEventLoop for synchronous-style calls.
 *
 * Typical usage (from a worker thread):
 *   OpenSubtitlesClient client(apiKey, username, password);
 *   if (!client.ensureLoggedIn()) { ... }
 *   const auto results = client.search("tt1234567", {"en","da"});
 *   for (const auto& r : results)
 *       client.downloadToFile(r.fileId, savePath);
 */
class OpenSubtitlesClient : public QObject
{
	Q_OBJECT
public:
	struct SubtitleFile {
		int     fileId         = 0;
		QString language;        // ISO 639-1, e.g. "en"
		QString fileName;        // suggested filename from API
		QString release;         // freeform uploader-supplied release name, e.g.
		                          // "Movie.Title.2020.EXTENDED.1080p.BluRay.x264-GROUP"
		int     downloadCount  = 0;
		bool    movieHashMatch = false; // true when the request's moviehash matched exactly
	};

	explicit OpenSubtitlesClient(const QString& apiKey,
	                             const QString& username,
	                             const QString& password,
	                             QObject* parent = nullptr);
	~OpenSubtitlesClient() override;

	bool    isConfigured()   const;
	QString lastError()      const { return m_lastError; }
	int     remaining()      const { return m_remaining; }
	// True once any request has come back HTTP 429 — the account's daily quota is
	// exhausted, so further requests will fail the same way; callers should stop
	// retrying rather than treating it as an ordinary per-item failure.
	bool    quotaExceeded()  const { return m_quotaExceeded; }
	// Seconds to wait before retrying, taken from the most recent 429's Retry-After
	// header; -1 if no 429 was seen or the header was absent/unparseable.
	int     retryAfterSeconds() const { return m_retryAfterSecs; }
	bool    wasCancelled()   const { return m_cancelled.loadRelaxed() != 0; }

	// Blocking — must be called from the thread this object lives on.
	bool ensureLoggedIn();

	// Returns up to a handful of ranked candidates per language (best match first),
	// not just the single most-downloaded file — callers can try the next candidate
	// when the first fails a post-download check (e.g. runtime mismatch).
	// localFileName/editionTokens drive release-name scoring (see candidateScore() in
	// the .cpp); movieHash is sent opportunistically (opt-in, see
	// UserProfile::computeSubtitleMovieHash) and, on an exact moviehash_match, that
	// candidate is boosted to the very top regardless of filename score.
	QList<SubtitleFile> search(const QString& imdbId, const QStringList& iso639_1Languages,
	                           const QString& localFileName = QString(),
	                           const QStringList& editionTokens = {},
	                           const QString& movieHash = QString());

	// Steps 1+2 of a download (resolve the signed CDN link, fetch its content) without
	// writing to disk — lets a caller inspect content (e.g. check subtitle runtime)
	// before deciding whether to keep it.
	bool downloadToBuffer(int fileId, QByteArray& outContent);
	bool downloadToFile(int fileId, const QString& savePath);

	// OpenSubtitles' moviehash: filesize plus the first and last 64 KiB of the file,
	// summed as little-endian uint64 words. Matches the original file's exact bytes —
	// meaningless once a file has been remuxed/re-edited, which is why this is opt-in
	// (UserProfile::computeSubtitleMovieHash) rather than always-on: it means reading
	// the whole 128 KiB from every file up front, which adds up over a batch.
	// Returns an empty string if the file is smaller than 128 KiB or unreadable.
	static QString computeMovieHash(const QString& filePath);

public slots:
	// Abort whatever request is currently in flight (safe to invoke cross-thread
	// via a queued connection — it is delivered and processed inside the nested
	// QEventLoop that httpPost/httpGet/fetchUrl run while blocked).
	void cancel();

private:
	bool httpPost(const QString& endpoint, const QByteArray& body, QByteArray& out);
	bool httpGet(const QUrl& url, QByteArray& out);     // API GET — adds Api-Key + Accept: application/json
	bool fetchUrl(const QUrl& url, QByteArray& out);    // bare GET — no API headers (CDN signed URLs)

	QNetworkAccessManager* m_nam    = nullptr;
	QNetworkReply*         m_reply  = nullptr;
	QString m_apiKey;
	QString m_username;
	QString m_password;
	QString m_token;
	qint64  m_tokenExpiry = 0;
	QString m_lastError;
	int     m_remaining      = -1;
	int     m_lastStatus     = 0;
	bool    m_quotaExceeded  = false;
	int     m_retryAfterSecs = -1;
	QAtomicInt m_cancelled{0};

	static constexpr const char* kBaseUrl = "https://api.opensubtitles.com/api/v1";
};

// Parses an HTTP Retry-After header value — either a plain integer number of
// seconds, or an RFC 2822/HTTP-date string — into seconds from now.
// Returns -1 if the value is empty or unparseable.
int parseRetryAfterSeconds(const QByteArray& headerValue);

// ── Worker QObject — move to a QThread, call run() via QThread::started ──────

class SubtitleDownloadWorker : public QObject
{
	Q_OBJECT
public:
	explicit SubtitleDownloadWorker(const QString& apiKey,
	                                const QString& username,
	                                const QString& password,
	                                const QString& imdbId,
	                                const QStringList& iso639_1Languages,
	                                const QString& videoPath,
	                                double durationSec,
	                                const QStringList& editionTokens,
	                                bool computeMovieHash,
	                                const QList<StreamRecord>& existingStreams,
	                                QObject* parent = nullptr);

public slots:
	void run();
	// Abort the in-flight request (if any) and stop before starting the next
	// language. Safe to call cross-thread — forwarded to OpenSubtitlesClient::cancel(),
	// which is itself safe to invoke while run() is blocked in its nested event loop.
	void cancel();

signals:
	// Emitted once, before the (usually few-second, occasionally longer) reference-subtitle
	// lookup runs — the only phase of a download with no other visible progress signal.
	void referenceLookupStarted();
	// found/label mirror pickAndExtractReferenceSubtitle()'s result — label is empty when
	// found is false (no existing subtitle qualified, falling back to the duration check).
	void referenceLookupDone(bool found, QString label);
	void languageStarted(QString language);                           // ISO 639-1
	void languageDone(QString language, bool success, QString message); // ISO 639-1
	// remaining: OpenSubtitles' reported downloads-left-today, or -1 if unknown.
	// quotaExceeded: an HTTP 429 was seen — the account is rate-limited, not just
	// this run's languages failing individually.
	// retryAfterSecs: seconds from the 429's Retry-After header, or -1 if unknown.
	void done(int downloaded, int failed, QString statusMessage, int remaining,
	          bool quotaExceeded, int retryAfterSecs);

private:
	QString     m_apiKey, m_username, m_password;
	QString     m_imdbId;
	QStringList m_languages;
	QString     m_videoPath;
	double      m_durationSec = 0.0;
	QStringList m_editionTokens;
	bool        m_computeMovieHash = false;
	QList<StreamRecord> m_existingStreams;
	QAtomicInt         m_cancelled{0};
	OpenSubtitlesClient* m_client = nullptr; // valid only while run() is executing
};

// ── ISO 639-2 → ISO 639-1 conversion (shared utility) ────────────────────────

QString iso6392to6391(const QString& iso6392);

// Reverse of the above — used to name downloaded sidecar files with the same
// 3-letter convention as the rest of the app (manual "Set Language" menu,
// SubtitleLanguageDetector), even though the OpenSubtitles API itself is
// queried with 2-letter codes.
QString iso6391to6392(const QString& iso6391);

// Understood languages (ISO 639-2) with no subtitle coverage among the given streams.
// A stream covers a language if it's a subtitle track whose language converts to the
// same ISO 639-1 code. "mul" (multiple/unknown) is never treated as missing.
QStringList missingSubtitleLanguages(const QList<StreamRecord>& streams,
                                      const QStringList& understoodLanguages6392);

// A subtitle's cue timings, measured against a file's known duration.
struct SubtitleCoverage {
	int    totalCues        = 0;    // total cues found; 0 = empty/unparseable subtitle
	int    cuesPastDuration = 0;    // cues whose START time falls after fileDurationSec
	double lastCueEndSec    = -1.0; // last cue's end time in seconds, -1 if none found
};

// Parses every SRT-style "-->" cue timestamp in srtContent and measures it against
// fileDurationSec (pass <= 0 to skip duration-dependent scoring).
SubtitleCoverage analyzeSubtitleCoverage(const QByteArray& srtContent, double fileDurationSec);

// Checks whether a subtitle's cue timings are consistent with fileDurationSec.
//
// Underrun (subtitle ends well before the file does) uses a loose time-based
// tolerance — many subtitles simply don't caption several minutes of silent
// trailing credits, which is harmless.
//
// Overrun is judged differently, deliberately not by "how many seconds past the
// end": a cue can never actually display once the file has stopped playing, so a
// lone translator "synced by..." credit is equally invisible whether it sits 5
// seconds or 5 minutes past the end — the seconds don't mean anything. What matters
// is *how many* cues fall past the end: a handful is normal trailing noise, but
// dozens means real dialogue this file's runtime doesn't reach, i.e. this subtitle
// is timed to a longer cut.
//
// Only catches whole-file length mismatches, not a same-length re-edit with content
// swapped mid-film — a coarse filter, not a sync guarantee.
// Returns true (passes) when duration is unknown or the subtitle has no cues at all.
bool subtitleDurationPasses(const SubtitleCoverage& coverage, double fileDurationSec);

} // namespace Mc
