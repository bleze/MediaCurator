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
		int     fileId        = 0;
		QString language;       // ISO 639-1, e.g. "en"
		QString fileName;       // suggested filename from API
		int     downloadCount = 0;
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
	bool    wasCancelled()   const { return m_cancelled.loadRelaxed() != 0; }

	// Blocking — must be called from the thread this object lives on.
	bool                ensureLoggedIn();
	QList<SubtitleFile> search(const QString& imdbId, const QStringList& iso639_1Languages);
	bool                downloadToFile(int fileId, const QString& savePath);

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
	QAtomicInt m_cancelled{0};

	static constexpr const char* kBaseUrl = "https://api.opensubtitles.com/api/v1";
};

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
	                                QObject* parent = nullptr);

public slots:
	void run();
	// Abort the in-flight request (if any) and stop before starting the next
	// language. Safe to call cross-thread — forwarded to OpenSubtitlesClient::cancel(),
	// which is itself safe to invoke while run() is blocked in its nested event loop.
	void cancel();

signals:
	void languageStarted(QString language);                           // ISO 639-1
	void languageDone(QString language, bool success, QString message); // ISO 639-1
	// remaining: OpenSubtitles' reported downloads-left-today, or -1 if unknown.
	// quotaExceeded: an HTTP 429 was seen — the account is rate-limited, not just
	// this run's languages failing individually.
	void done(int downloaded, int failed, QString statusMessage, int remaining, bool quotaExceeded);

private:
	QString     m_apiKey, m_username, m_password;
	QString     m_imdbId;
	QStringList m_languages;
	QString     m_videoPath;
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

} // namespace Mc
