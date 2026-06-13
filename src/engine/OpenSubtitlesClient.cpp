#include "engine/OpenSubtitlesClient.h"
#include "core/DatabaseManager.h"
#include "scanner/ScanWorker.h"

#include <QDateTime>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

Q_LOGGING_CATEGORY(lcOS, "mc.opensubtitles")

namespace Mc {

// ── ISO 639-2 → ISO 639-1 conversion ─────────────────────────────────────────

QString iso6392to6391(const QString& iso6392)
{
	static const QHash<QString, QString> map = {
		{"ara","ar"},{"zho","zh"},{"hrv","hr"},{"ces","cs"},{"dan","da"},
		{"nld","nl"},{"eng","en"},{"fin","fi"},{"fra","fr"},{"deu","de"},
		{"ell","el"},{"heb","he"},{"hun","hu"},{"ind","id"},{"ita","it"},
		{"jpn","ja"},{"kor","ko"},{"nor","no"},{"pol","pl"},{"por","pt"},
		{"ron","ro"},{"rus","ru"},{"srp","sr"},{"slk","sk"},{"spa","es"},
		{"swe","sv"},{"tha","th"},{"tur","tr"},{"ukr","uk"},{"vie","vi"},
		{"bul","bg"},{"cat","ca"},{"est","et"},{"hin","hi"},{"isl","is"},
		{"lav","lv"},{"lit","lt"},{"msa","ms"},{"slv","sl"},{"nob","nb"},
		{"nno","nn"},{"fas","fa"},{"ben","bn"},{"tam","ta"},{"tel","te"},
	};
	return map.value(iso6392.toLower());
}

// ── OpenSubtitlesClient ───────────────────────────────────────────────────────

OpenSubtitlesClient::OpenSubtitlesClient(const QString& apiKey,
                                         const QString& username,
                                         const QString& password,
                                         QObject* parent)
	: QObject(parent)
	, m_apiKey(apiKey)
	, m_username(username)
	, m_password(password)
{
	m_nam = new QNetworkAccessManager(this);
}

OpenSubtitlesClient::~OpenSubtitlesClient() = default;

bool OpenSubtitlesClient::isConfigured() const
{
	return !m_apiKey.isEmpty();
}

bool OpenSubtitlesClient::ensureLoggedIn()
{
	// Token valid for 24 h; reuse if we have one and it is not near expiry.
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	if (!m_token.isEmpty() && m_tokenExpiry > now + 300)
		return true;

	if (m_username.isEmpty() || m_password.isEmpty()) {
		// Anonymous mode — no JWT needed for searching.
		m_token.clear();
		return true;
	}

	QJsonObject body;
	body["username"] = m_username;
	body["password"] = m_password;

	QByteArray out;
	if (!httpPost(QStringLiteral("/login"), QJsonDocument(body).toJson(QJsonDocument::Compact), out))
		return false;

	const QJsonObject resp = QJsonDocument::fromJson(out).object();
	m_token = resp.value(QStringLiteral("token")).toString();
	if (m_token.isEmpty()) {
		m_lastError = QStringLiteral("Login failed — no token in response");
		return false;
	}
	m_tokenExpiry = now + 86400; // 24 h
	return true;
}

QList<OpenSubtitlesClient::SubtitleFile>
OpenSubtitlesClient::search(const QString& imdbId, const QStringList& iso639_1Languages)
{
	if (imdbId.isEmpty() || iso639_1Languages.isEmpty())
		return {};

	// Strip "tt" prefix — API wants the bare numeric ID.
	const QString numericId = imdbId.startsWith(QStringLiteral("tt"), Qt::CaseInsensitive)
		? imdbId.mid(2)
		: imdbId;

	QUrl url(QStringLiteral("%1/subtitles").arg(QLatin1String(kBaseUrl)));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("imdb_id"),  numericId);
	query.addQueryItem(QStringLiteral("languages"), iso639_1Languages.join(QLatin1Char(',')));
	query.addQueryItem(QStringLiteral("per_page"),  QStringLiteral("20"));
	url.setQuery(query);

	QByteArray out;
	if (!httpGet(url, out))
		return {};

	const QJsonObject root = QJsonDocument::fromJson(out).object();
	const QJsonArray  data = root.value(QStringLiteral("data")).toArray();

	QMap<QString, SubtitleFile> bestPerLang;

	for (const QJsonValue& entry : data) {
		const QJsonObject attrs = entry.toObject().value(QStringLiteral("attributes")).toObject();
		const QString lang      = attrs.value(QStringLiteral("language")).toString().toLower();
		const int downloads     = attrs.value(QStringLiteral("download_count")).toInt();

		if (!iso639_1Languages.contains(lang))
			continue;

		const QJsonArray files = attrs.value(QStringLiteral("files")).toArray();
		if (files.isEmpty())
			continue;

		const QJsonObject firstFile = files.first().toObject();
		const int fileId = firstFile.value(QStringLiteral("file_id")).toInt();
		if (fileId == 0)
			continue;

		if (!bestPerLang.contains(lang) || downloads > bestPerLang[lang].downloadCount) {
			SubtitleFile sf;
			sf.fileId        = fileId;
			sf.language      = lang;
			sf.fileName      = firstFile.value(QStringLiteral("file_name")).toString();
			sf.downloadCount = downloads;
			bestPerLang[lang] = sf;
		}
	}

	return bestPerLang.values();
}

bool OpenSubtitlesClient::downloadToFile(int fileId, const QString& savePath)
{
	// Step 1: request a temporary download link (retry up to 3× on 5xx).
	QJsonObject body;
	body["file_id"] = fileId;

	QByteArray out;
	bool ok = false;
	for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
		if (attempt > 0) {
			qCDebug(lcOS) << "Retry" << attempt << "for /download (last error:" << m_lastError << ")";
			QEventLoop wait;
			QTimer::singleShot(2000, &wait, &QEventLoop::quit);
			wait.exec();
		}
		ok = httpPost(QStringLiteral("/download"),
		              QJsonDocument(body).toJson(QJsonDocument::Compact), out);
		// Don't retry on client errors (4xx); only retry on 5xx / network errors.
		if (!ok && m_lastStatus >= 400 && m_lastStatus < 500)
			break;
	}
	if (!ok) return false;

	const QJsonObject resp = QJsonDocument::fromJson(out).object();
	const QString link     = resp.value(QStringLiteral("link")).toString();
	m_remaining = resp.value(QStringLiteral("remaining")).toInt(m_remaining);

	if (link.isEmpty()) {
		m_lastError = QStringLiteral("Download response had no link");
		return false;
	}

	// Step 2: download the actual file content from the CDN signed URL.
	// Use a bare fetch — no API-Key/Authorization, no Accept override.
	QByteArray content;
	ok = false;
	for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
		if (attempt > 0) {
			qCDebug(lcOS) << "Retry" << attempt << "fetching content (last error:" << m_lastError << ")";
			QEventLoop wait;
			QTimer::singleShot(2000, &wait, &QEventLoop::quit);
			wait.exec();
		}
		ok = fetchUrl(QUrl(link), content);
		if (!ok && m_lastStatus >= 400 && m_lastStatus < 500)
			break;
	}
	if (!ok) return false;

	// Step 3: save to disk.
	QFile f(savePath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		m_lastError = QStringLiteral("Cannot write to %1: %2").arg(savePath, f.errorString());
		return false;
	}
	f.write(content);
	return true;
}

// ── Private HTTP helpers ──────────────────────────────────────────────────────

bool OpenSubtitlesClient::httpPost(const QString& endpoint, const QByteArray& body, QByteArray& out)
{
	QUrl url(QStringLiteral("%1%2").arg(QLatin1String(kBaseUrl), endpoint));
	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
	req.setRawHeader(QByteArrayLiteral("Accept"),     QByteArrayLiteral("application/json"));
	req.setRawHeader(QByteArrayLiteral("Api-Key"),    m_apiKey.toUtf8());
	req.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("MediaCurator/1.0"));
	if (!m_token.isEmpty())
		req.setRawHeader(QByteArrayLiteral("Authorization"),
		                 QStringLiteral("Bearer %1").arg(m_token).toUtf8());

	m_reply = m_nam->post(req, body);
	{
		QEventLoop loop;
		connect(m_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		loop.exec();
	}

	m_lastStatus = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	out = m_reply->readAll();
	m_reply->deleteLater();
	m_reply = nullptr;

	if (m_lastStatus == 0) {
		m_lastError = QStringLiteral("Network error");
		return false;
	}
	if (m_lastStatus == 429) {
		m_lastError = QStringLiteral("Rate limit reached — try again later");
		return false;
	}
	if (m_lastStatus == 401) {
		m_lastError = QStringLiteral("Unauthorized — add credentials in Settings → OpenSubtitles");
		return false;
	}
	if (m_lastStatus >= 400) {
		const QJsonObject err = QJsonDocument::fromJson(out).object();
		const QString apiMsg  = err.value(QStringLiteral("message")).toString();
		m_lastError = apiMsg.isEmpty()
		    ? QStringLiteral("HTTP %1").arg(m_lastStatus)
		    : QStringLiteral("HTTP %1: %2").arg(m_lastStatus).arg(apiMsg);
		if (m_lastStatus >= 500)
			qWarning(lcOS) << "Server error" << m_lastStatus << "— body:" << out.left(400);
		return false;
	}
	return true;
}

bool OpenSubtitlesClient::httpGet(const QUrl& url, QByteArray& out)
{
	QNetworkRequest req(url);
	req.setRawHeader(QByteArrayLiteral("Accept"),     QByteArrayLiteral("application/json"));
	req.setRawHeader(QByteArrayLiteral("Api-Key"),    m_apiKey.toUtf8());
	req.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("MediaCurator/1.0"));
	if (!m_token.isEmpty())
		req.setRawHeader(QByteArrayLiteral("Authorization"),
		                 QStringLiteral("Bearer %1").arg(m_token).toUtf8());

	m_reply = m_nam->get(req);
	{
		QEventLoop loop;
		connect(m_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		loop.exec();
	}

	m_lastStatus = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	out = m_reply->readAll();
	m_reply->deleteLater();
	m_reply = nullptr;

	if (m_lastStatus == 0) {
		m_lastError = QStringLiteral("Network error");
		return false;
	}
	if (m_lastStatus >= 400) {
		m_lastError = QStringLiteral("HTTP %1").arg(m_lastStatus);
		if (m_lastStatus >= 500)
			qWarning(lcOS) << "Server error" << m_lastStatus << "— body:" << out.left(400);
		return false;
	}
	return true;
}

bool OpenSubtitlesClient::fetchUrl(const QUrl& url, QByteArray& out)
{
	QNetworkRequest req(url);
	req.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("MediaCurator/1.0"));

	m_reply = m_nam->get(req);
	{
		QEventLoop loop;
		connect(m_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		loop.exec();
	}

	m_lastStatus = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	out = m_reply->readAll();
	m_reply->deleteLater();
	m_reply = nullptr;

	if (m_lastStatus == 0) {
		m_lastError = QStringLiteral("Network error fetching content");
		return false;
	}
	if (m_lastStatus >= 400) {
		m_lastError = QStringLiteral("HTTP %1 fetching content").arg(m_lastStatus);
		if (m_lastStatus >= 500)
			qWarning(lcOS) << "CDN error" << m_lastStatus << "— body:" << out.left(200);
		return false;
	}
	return true;
}

// ── SubtitleDownloadWorker ────────────────────────────────────────────────────

SubtitleDownloadWorker::SubtitleDownloadWorker(const QString& apiKey,
                                               const QString& username,
                                               const QString& password,
                                               const QString& imdbId,
                                               const QStringList& iso639_1Languages,
                                               const QString& videoPath,
                                               QObject* parent)
	: QObject(parent)
	, m_apiKey(apiKey)
	, m_username(username)
	, m_password(password)
	, m_imdbId(imdbId)
	, m_languages(iso639_1Languages)
	, m_videoPath(videoPath)
{}

void SubtitleDownloadWorker::run()
{
	OpenSubtitlesClient client(m_apiKey, m_username, m_password);

	qCDebug(lcOS) << "SubtitleDownloadWorker::run — imdb:" << m_imdbId
	              << "languages:" << m_languages << "path:" << m_videoPath;

	if (!client.ensureLoggedIn()) {
		qWarning(lcOS) << "Login failed:" << client.lastError();
		const QString err = QStringLiteral("Login failed: %1").arg(client.lastError());
		for (const QString& lang : m_languages)
			emit languageDone(lang, false, err);
		emit done(0, m_languages.size(), err);
		return;
	}
	qCDebug(lcOS) << "Login OK";

	const auto results = client.search(m_imdbId, m_languages);
	qCDebug(lcOS) << "Search returned" << results.size() << "result(s)";

	// Map language → best result
	QHash<QString, OpenSubtitlesClient::SubtitleFile> resultMap;
	for (const auto& r : results)
		resultMap[r.language] = r;

	const QFileInfo fi(m_videoPath);
	const QString   dir      = fi.absolutePath();
	const QString   basename = fi.completeBaseName();

	int downloaded = 0, failed = 0;

	for (const QString& lang : m_languages) {
		if (!resultMap.contains(lang)) {
			qCDebug(lcOS) << "No result for" << lang;
			emit languageDone(lang, false,
			                  QStringLiteral("Not found on OpenSubtitles.com"));
			++failed;
			continue;
		}

		emit languageStarted(lang);

		const auto&   r        = resultMap[lang];
		const QString savePath = QStringLiteral("%1/%2.%3.srt").arg(dir, basename, lang);
		qCDebug(lcOS) << "Downloading file_id" << r.fileId << "→" << savePath;

		if (client.downloadToFile(r.fileId, savePath)) {
			qCDebug(lcOS) << "  OK";
			emit languageDone(lang, true, savePath);
			++downloaded;
		} else {
			qWarning(lcOS) << "  failed:" << client.lastError();
			emit languageDone(lang, false, client.lastError());
			++failed;
		}
	}

	QString statusMsg;
	if (downloaded > 0)
		statusMsg = QStringLiteral("Downloaded %1 subtitle(s)").arg(downloaded);
	if (failed > 0)
		statusMsg += (statusMsg.isEmpty() ? QString{} : QStringLiteral(", "))
		             + QStringLiteral("%1 failed").arg(failed);
	if (client.remaining() >= 0)
		statusMsg += QStringLiteral(" (%1 downloads remaining today)").arg(client.remaining());

	qCDebug(lcOS) << "Done:" << statusMsg;
	emit done(downloaded, failed, statusMsg);
}

} // namespace Mc
