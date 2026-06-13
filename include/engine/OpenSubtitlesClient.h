#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

namespace Mc {

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

	bool    isConfigured() const;
	QString lastError()    const { return m_lastError; }
	int     remaining()    const { return m_remaining; }

	// Blocking — must be called from the thread this object lives on.
	bool                ensureLoggedIn();
	QList<SubtitleFile> search(const QString& imdbId, const QStringList& iso639_1Languages);
	bool                downloadToFile(int fileId, const QString& savePath);

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
	int     m_remaining   = -1;
	int     m_lastStatus  = 0;

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

signals:
	void languageStarted(QString language);                           // ISO 639-1
	void languageDone(QString language, bool success, QString message); // ISO 639-1
	void done(int downloaded, int failed, QString statusMessage);

private:
	QString     m_apiKey, m_username, m_password;
	QString     m_imdbId;
	QStringList m_languages;
	QString     m_videoPath;
};

// ── ISO 639-2 → ISO 639-1 conversion (shared utility) ────────────────────────

QString iso6392to6391(const QString& iso6392);

} // namespace Mc
