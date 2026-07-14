#include "engine/OpenSubtitlesClient.h"
#include "core/DatabaseManager.h"
#include "engine/SubtitleSyncMatcher.h"
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
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

Q_LOGGING_CATEGORY(lcOS, "mc.opensubtitles")

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
// Restore the folder's creation/modified timestamps after dropping new .srt files
// into it, so downloading subtitles doesn't bubble the folder up as "newest" in
// media libraries that sort by Date Modified. Mirrors preserveTimestamps() in
// RemuxJob.cpp/JobQueue.cpp, but needs FILE_FLAG_BACKUP_SEMANTICS to open a directory handle.
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

// ── ISO 639-2 ⇄ ISO 639-1 conversion ─────────────────────────────────────────
// Single source-of-truth pair list — both directions are derived from this so
// they can't silently drift apart.

static const QList<QPair<QString, QString>>& iso639PairTable()
{
	static const QList<QPair<QString, QString>> pairs = {
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
	return pairs;
}

QString iso6392to6391(const QString& iso6392)
{
	static const QHash<QString, QString> map = [] {
		QHash<QString, QString> m;
		for (const auto& [c6392, c6391] : iso639PairTable()) m.insert(c6392, c6391);
		return m;
	}();
	return map.value(iso6392.toLower());
}

QString iso6391to6392(const QString& iso6391)
{
	static const QHash<QString, QString> map = [] {
		QHash<QString, QString> m;
		for (const auto& [c6392, c6391] : iso639PairTable()) m.insert(c6391, c6392);
		return m;
	}();
	return map.value(iso6391.toLower());
}

int parseRetryAfterSeconds(const QByteArray& headerValue)
{
	if (headerValue.isEmpty())
		return -1;

	bool ok = false;
	const int secs = headerValue.trimmed().toInt(&ok);
	if (ok)
		return secs >= 0 ? secs : -1;

	// Not a delta-seconds value — try the HTTP-date form (RFC 7231 / RFC 2822).
	const QDateTime when = QDateTime::fromString(QString::fromLatin1(headerValue), Qt::RFC2822Date);
	if (!when.isValid())
		return -1;
	const qint64 diff = QDateTime::currentDateTimeUtc().secsTo(when);
	return diff > 0 ? static_cast<int>(diff) : -1;
}

QStringList missingSubtitleLanguages(const QList<StreamRecord>& streams,
                                      const QStringList& understoodLanguages6392)
{
	QSet<QString> coveredIso6391;
	for (const auto& s : streams) {
		if (s.codecType != QLatin1String("subtitle")) continue;
		if (s.language.isEmpty() || s.language == QLatin1String("und")) continue;
		const QString c = iso6392to6391(s.language);
		if (!c.isEmpty()) coveredIso6391.insert(c);
		else if (s.language.length() == 2) coveredIso6391.insert(s.language.toLower());
	}

	QStringList missing;
	for (const QString& lang6392 : understoodLanguages6392) {
		if (lang6392 == QLatin1String("mul")) continue;
		const QString lang6391 = iso6392to6391(lang6392);
		if (!lang6391.isEmpty() && !coveredIso6391.contains(lang6391))
			missing << lang6392;
	}
	return missing;
}

double lastSubtitleCueEndSeconds(const QByteArray& srtContent)
{
	static const QRegularExpression re(
		QStringLiteral(R"(-->\s*(\d{2}):(\d{2}):(\d{2})[,.](\d{3}))"));
	const QString text = QString::fromUtf8(srtContent);
	QRegularExpressionMatchIterator it = re.globalMatch(text);

	double lastEnd = -1.0;
	while (it.hasNext()) {
		const QRegularExpressionMatch m = it.next();
		const double h   = m.captured(1).toDouble();
		const double min = m.captured(2).toDouble();
		const double s   = m.captured(3).toDouble();
		const double ms  = m.captured(4).toDouble();
		lastEnd = h * 3600.0 + min * 60.0 + s + ms / 1000.0;
	}
	return lastEnd;
}

SubtitleCoverage analyzeSubtitleCoverage(const QByteArray& srtContent, double fileDurationSec)
{
	static const QRegularExpression re(QStringLiteral(
		R"((\d{2}):(\d{2}):(\d{2})[,.](\d{3})\s*-->\s*(\d{2}):(\d{2}):(\d{2})[,.](\d{3}))"));
	const QString text = QString::fromUtf8(srtContent);
	QRegularExpressionMatchIterator it = re.globalMatch(text);

	SubtitleCoverage cov;
	while (it.hasNext()) {
		const QRegularExpressionMatch m = it.next();
		const double startSec = m.captured(1).toDouble() * 3600.0 + m.captured(2).toDouble() * 60.0
		                       + m.captured(3).toDouble() + m.captured(4).toDouble() / 1000.0;
		const double endSec   = m.captured(5).toDouble() * 3600.0 + m.captured(6).toDouble() * 60.0
		                       + m.captured(7).toDouble() + m.captured(8).toDouble() / 1000.0;
		++cov.totalCues;
		cov.lastCueEndSec = endSec;
		if (fileDurationSec > 0.0 && startSec > fileDurationSec)
			++cov.cuesPastDuration;
	}
	return cov;
}

// Underrun tolerance is loose — many subtitles don't bother captioning several
// minutes of silent trailing credits, which is harmless. 5 min turned out to be too
// tight in practice: even ordinary (non-special-edition) films routinely run 5-10+
// minutes of end credits nowadays, and both observed false rejects were ordinary
// films, not genuinely shorter cuts. A real shorter-cut mismatch (the case this is
// actually meant to catch) tends to differ by much more than 15 min, so there's
// room to be considerably more generous here without losing that protection.
static constexpr double kDurationUnderrunToleranceSec = 900.0;
// Overrun is judged by cue COUNT, not seconds — see the header comment on
// subtitleDurationPasses() for why a seconds-based overrun tolerance doesn't make
// sense here. A couple of stray trailing credit cues is normal; more than this
// many means real dialogue the file's runtime doesn't reach.
static constexpr int kMaxTrailingCuesPastDuration = 3;

bool subtitleDurationPasses(const SubtitleCoverage& coverage, double fileDurationSec)
{
	if (fileDurationSec <= 0.0 || coverage.totalCues == 0)
		return true; // nothing to judge — don't block on missing data
	if (coverage.lastCueEndSec < fileDurationSec - kDurationUnderrunToleranceSec)
		return false;
	return coverage.cuesPastDuration <= kMaxTrailingCuesPastDuration;
}

// ── Candidate ranking ─────────────────────────────────────────────────────────
// Neither a filename nor a release string is authoritative on its own — users
// sometimes keep the original release filename, sometimes rename to their own
// convention, and uploaders' release strings range from precise to blank. So this
// scores candidates against the local filename by generic token overlap, then
// layers an edition-token check on top since "same release group, wrong cut" is
// the specific failure mode this exists to catch.

static QStringList tokenize(const QString& s)
{
	static const QRegularExpression sep(QStringLiteral("[^a-z0-9]+"));
	return s.toLower().split(sep, Qt::SkipEmptyParts);
}

static double jaccardSimilarity(const QSet<QString>& a, const QSet<QString>& b)
{
	if (a.isEmpty() || b.isEmpty()) return 0.0;
	QSet<QString> inter = a;
	inter.intersect(b);
	QSet<QString> uni = a;
	uni.unite(b);
	return uni.isEmpty() ? 0.0 : double(inter.size()) / double(uni.size());
}

// Higher is a better match; not bounded to [0,1] once the moviehash/edition
// adjustments are applied. localFileName may be empty (no filename to compare) —
// callers still get a score from moviehash/download-count alone in that case.
static double candidateScore(const OpenSubtitlesClient::SubtitleFile& candidate,
                              const QString& localFileName,
                              const QStringList& editionTokens)
{
	double score = candidate.movieHashMatch ? 1000.0 : 0.0;

	const QStringList localTokenList = tokenize(localFileName);
	if (!localTokenList.isEmpty()) {
		const QSet<QString> localTokens(localTokenList.begin(), localTokenList.end());

		double best = 0.0;
		for (const QString& text : {candidate.release, candidate.fileName}) {
			if (text.isEmpty()) continue;
			const QStringList candTokenList = tokenize(text);
			const QSet<QString> candTokens(candTokenList.begin(), candTokenList.end());
			best = qMax(best, jaccardSimilarity(localTokens, candTokens));
		}
		score += best;

		// Edition-token check: does the local filename and the candidate's text each
		// mention one of the known cut/edition phrases, and do they agree?
		const QString localLower = localFileName.toLower();
		const QString candLower  = (candidate.release + QLatin1Char(' ') + candidate.fileName).toLower();
		QStringList localEditions, candEditions;
		for (const QString& tok : editionTokens) {
			const QString t = tok.toLower();
			if (t.isEmpty()) continue;
			if (localLower.contains(t)) localEditions << t;
			if (candLower.contains(t))  candEditions  << t;
		}
		if (!localEditions.isEmpty()) {
			bool anyMatch = false;
			for (const QString& e : localEditions)
				if (candEditions.contains(e)) { anyMatch = true; break; }
			// Local file names a specific edition and the candidate names a different
			// one (or none at all) — strong penalty, likely the wrong cut.
			score += anyMatch ? 0.3 : -0.5;
		} else if (!candEditions.isEmpty()) {
			// Candidate claims an edition the local filename doesn't mention — mild
			// penalty only; the local name may just be uninformative rather than wrong.
			score -= 0.15;
		}
	}

	// Popularity as a low-weight tiebreaker only — it says nothing about cut/edition.
	score += qMin(0.05, candidate.downloadCount / 200000.0);
	return score;
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

void OpenSubtitlesClient::cancel()
{
	m_cancelled.storeRelaxed(1);
	if (m_reply)
		m_reply->abort();
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
OpenSubtitlesClient::search(const QString& imdbId, const QStringList& iso639_1Languages,
                            const QString& localFileName, const QStringList& editionTokens,
                            const QString& movieHash)
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
	// Larger page than before — ranking now keeps several candidates per language
	// instead of just the single most-downloaded one, so more raw results to rank from.
	query.addQueryItem(QStringLiteral("per_page"),  QStringLiteral("50"));
	if (!movieHash.isEmpty())
		query.addQueryItem(QStringLiteral("moviehash"), movieHash);
	url.setQuery(query);

	QByteArray out;
	if (!httpGet(url, out))
		return {};

	const QJsonObject root = QJsonDocument::fromJson(out).object();
	const QJsonArray  data = root.value(QStringLiteral("data")).toArray();

	QHash<QString, QList<SubtitleFile>> candidatesPerLang;

	for (const QJsonValue& entry : data) {
		const QJsonObject attrs = entry.toObject().value(QStringLiteral("attributes")).toObject();
		const QString lang      = attrs.value(QStringLiteral("language")).toString().toLower();

		if (!iso639_1Languages.contains(lang))
			continue;

		const QJsonArray files = attrs.value(QStringLiteral("files")).toArray();
		if (files.isEmpty())
			continue;

		const QJsonObject firstFile = files.first().toObject();
		const int fileId = firstFile.value(QStringLiteral("file_id")).toInt();
		if (fileId == 0)
			continue;

		SubtitleFile sf;
		sf.fileId         = fileId;
		sf.language       = lang;
		sf.fileName       = firstFile.value(QStringLiteral("file_name")).toString();
		sf.release        = attrs.value(QStringLiteral("release")).toString();
		sf.downloadCount  = attrs.value(QStringLiteral("download_count")).toInt();
		sf.movieHashMatch = attrs.value(QStringLiteral("moviehash_match")).toBool();
		candidatesPerLang[lang].append(sf);
	}

	// Rank each language's candidates independently (best match first) and cap how
	// many we keep — downstream only ever tries a handful before giving up anyway.
	constexpr int kMaxCandidatesPerLang = 8;
	QList<SubtitleFile> results;
	for (auto it = candidatesPerLang.begin(); it != candidatesPerLang.end(); ++it) {
		QList<SubtitleFile>& list = it.value();

		QList<QPair<double, SubtitleFile>> scored;
		scored.reserve(list.size());
		for (const SubtitleFile& sf : list)
			scored.append({candidateScore(sf, localFileName, editionTokens), sf});

		std::stable_sort(scored.begin(), scored.end(),
			[](const QPair<double, SubtitleFile>& a, const QPair<double, SubtitleFile>& b) {
				return a.first > b.first;
			});

		for (int i = 0; i < scored.size() && i < kMaxCandidatesPerLang; ++i)
			results.append(scored[i].second);
	}
	return results;
}

bool OpenSubtitlesClient::downloadToBuffer(int fileId, QByteArray& outContent)
{
	// Step 1: request a temporary download link (retry up to 3× on 5xx).
	QJsonObject body;
	body["file_id"] = fileId;

	QByteArray out;
	bool ok = false;
	for (int attempt = 0; attempt < 3 && !ok && !m_cancelled.loadRelaxed(); ++attempt) {
		if (attempt > 0) {
			qCDebug(lcOS) << "Retry" << attempt << "for /download (last error:" << m_lastError << ")";
			QEventLoop wait;
			QTimer::singleShot(2000, &wait, &QEventLoop::quit);
			wait.exec();
		}
		ok = httpPost(QStringLiteral("/download"),
		              QJsonDocument(body).toJson(QJsonDocument::Compact), out);
		// Don't retry on client errors (4xx) — including 429, which won't clear up
		// within this run — or once cancelled; only retry on 5xx / network errors.
		if (!ok && ((m_lastStatus >= 400 && m_lastStatus < 500) || m_cancelled.loadRelaxed()))
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
	ok = false;
	for (int attempt = 0; attempt < 3 && !ok && !m_cancelled.loadRelaxed(); ++attempt) {
		if (attempt > 0) {
			qCDebug(lcOS) << "Retry" << attempt << "fetching content (last error:" << m_lastError << ")";
			QEventLoop wait;
			QTimer::singleShot(2000, &wait, &QEventLoop::quit);
			wait.exec();
		}
		ok = fetchUrl(QUrl(link), outContent);
		if (!ok && ((m_lastStatus >= 400 && m_lastStatus < 500) || m_cancelled.loadRelaxed()))
			break;
	}
	return ok;
}

bool OpenSubtitlesClient::downloadToFile(int fileId, const QString& savePath)
{
	QByteArray content;
	if (!downloadToBuffer(fileId, content))
		return false;

	QFile f(savePath);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		m_lastError = QStringLiteral("Cannot write to %1: %2").arg(savePath, f.errorString());
		return false;
	}
	f.write(content);
	return true;
}

// static
QString OpenSubtitlesClient::computeMovieHash(const QString& filePath)
{
	QFile f(filePath);
	if (!f.open(QIODevice::ReadOnly))
		return {};

	constexpr qint64 kChunkSize = 65536; // 64 KiB
	const qint64 fileSize = f.size();
	if (fileSize < kChunkSize * 2)
		return {}; // too small for the algorithm to be meaningful

	quint64 hash = static_cast<quint64>(fileSize);

	// Reads raw bytes as little-endian uint64 words — matches the reference algorithm
	// on the little-endian platforms this app targets (x86/x64/ARM64).
	auto sumChunk = [&](qint64 offset) -> bool {
		if (!f.seek(offset)) return false;
		const QByteArray buf = f.read(kChunkSize);
		if (buf.size() != kChunkSize) return false;
		const quint64* words = reinterpret_cast<const quint64*>(buf.constData());
		for (int i = 0; i < kChunkSize / 8; ++i)
			hash += words[i];
		return true;
	};

	if (!sumChunk(0)) return {};
	if (!sumChunk(fileSize - kChunkSize)) return {};

	return QStringLiteral("%1").arg(hash, 16, 16, QLatin1Char('0'));
}

// ── Private HTTP helpers ──────────────────────────────────────────────────────

bool OpenSubtitlesClient::httpPost(const QString& endpoint, const QByteArray& body, QByteArray& out)
{
	if (m_cancelled.loadRelaxed()) {
		m_lastError = QStringLiteral("Cancelled");
		return false;
	}

	QUrl url(QStringLiteral("%1%2").arg(QLatin1String(kBaseUrl), endpoint));
	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
	req.setRawHeader(QByteArrayLiteral("Accept"),     QByteArrayLiteral("application/json"));
	req.setRawHeader(QByteArrayLiteral("Api-Key"),    m_apiKey.toUtf8());
	req.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("MediaCurator/1.0"));
	if (!m_token.isEmpty())
		req.setRawHeader(QByteArrayLiteral("Authorization"),
		                 QStringLiteral("Bearer %1").arg(m_token).toUtf8());
	req.setTransferTimeout(30000); // don't hang forever if the server accepts but never responds

	m_reply = m_nam->post(req, body);
	{
		QEventLoop loop;
		connect(m_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		loop.exec();
	}

	m_lastStatus = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	out = m_reply->readAll();
	const QByteArray retryAfterHeader = m_reply->rawHeader(QByteArrayLiteral("Retry-After"));
	m_reply->deleteLater();
	m_reply = nullptr;

	if (m_cancelled.loadRelaxed()) {
		m_lastError = QStringLiteral("Cancelled");
		return false;
	}
	if (m_lastStatus == 0) {
		m_lastError = QStringLiteral("Network error");
		return false;
	}
	if (m_lastStatus == 429) {
		m_quotaExceeded = true;
		m_retryAfterSecs = parseRetryAfterSeconds(retryAfterHeader);
		m_lastError = QStringLiteral("Rate limit reached — try again later");
		qWarning(lcOS) << "429 on POST" << endpoint << "— Retry-After:" << retryAfterHeader
		               << "parsed:" << m_retryAfterSecs << "s — body:" << out.left(400);
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
	if (m_cancelled.loadRelaxed()) {
		m_lastError = QStringLiteral("Cancelled");
		return false;
	}

	QNetworkRequest req(url);
	req.setRawHeader(QByteArrayLiteral("Accept"),     QByteArrayLiteral("application/json"));
	req.setRawHeader(QByteArrayLiteral("Api-Key"),    m_apiKey.toUtf8());
	req.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("MediaCurator/1.0"));
	if (!m_token.isEmpty())
		req.setRawHeader(QByteArrayLiteral("Authorization"),
		                 QStringLiteral("Bearer %1").arg(m_token).toUtf8());
	req.setTransferTimeout(30000);

	m_reply = m_nam->get(req);
	{
		QEventLoop loop;
		connect(m_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		loop.exec();
	}

	m_lastStatus = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	out = m_reply->readAll();
	const QByteArray retryAfterHeader = m_reply->rawHeader(QByteArrayLiteral("Retry-After"));
	m_reply->deleteLater();
	m_reply = nullptr;

	if (m_cancelled.loadRelaxed()) {
		m_lastError = QStringLiteral("Cancelled");
		return false;
	}
	if (m_lastStatus == 0) {
		m_lastError = QStringLiteral("Network error");
		return false;
	}
	if (m_lastStatus == 429) {
		m_quotaExceeded = true;
		m_retryAfterSecs = parseRetryAfterSeconds(retryAfterHeader);
		m_lastError = QStringLiteral("Rate limit reached — try again later");
		qWarning(lcOS) << "429 on GET" << url << "— Retry-After:" << retryAfterHeader
		               << "parsed:" << m_retryAfterSecs << "s — body:" << out.left(400);
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
	if (m_cancelled.loadRelaxed()) {
		m_lastError = QStringLiteral("Cancelled");
		return false;
	}

	QNetworkRequest req(url);
	req.setRawHeader(QByteArrayLiteral("User-Agent"), QByteArrayLiteral("MediaCurator/1.0"));
	req.setTransferTimeout(30000);

	m_reply = m_nam->get(req);
	{
		QEventLoop loop;
		connect(m_reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		loop.exec();
	}

	m_lastStatus = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	out = m_reply->readAll();
	const QByteArray retryAfterHeader = m_reply->rawHeader(QByteArrayLiteral("Retry-After"));
	m_reply->deleteLater();
	m_reply = nullptr;

	if (m_cancelled.loadRelaxed()) {
		m_lastError = QStringLiteral("Cancelled");
		return false;
	}
	if (m_lastStatus == 0) {
		m_lastError = QStringLiteral("Network error fetching content");
		return false;
	}
	if (m_lastStatus == 429) {
		m_quotaExceeded = true;
		m_retryAfterSecs = parseRetryAfterSeconds(retryAfterHeader);
		m_lastError = QStringLiteral("Rate limit reached — try again later");
		qWarning(lcOS) << "429 fetching CDN content — Retry-After:" << retryAfterHeader
		               << "parsed:" << m_retryAfterSecs << "s";
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
                                               double durationSec,
                                               const QStringList& editionTokens,
                                               bool computeMovieHash,
                                               const QList<StreamRecord>& existingStreams,
                                               QObject* parent)
	: QObject(parent)
	, m_apiKey(apiKey)
	, m_username(username)
	, m_password(password)
	, m_imdbId(imdbId)
	, m_languages(iso639_1Languages)
	, m_videoPath(videoPath)
	, m_durationSec(durationSec)
	, m_editionTokens(editionTokens)
	, m_computeMovieHash(computeMovieHash)
	, m_existingStreams(existingStreams)
{}

void SubtitleDownloadWorker::cancel()
{
	m_cancelled.storeRelaxed(1);
	if (m_client)
		m_client->cancel();
}

void SubtitleDownloadWorker::run()
{
	OpenSubtitlesClient client(m_apiKey, m_username, m_password);
	m_client = &client;
	if (m_cancelled.loadRelaxed())
		client.cancel(); // cancel() arrived before run() started — apply it now

	qCDebug(lcOS) << "SubtitleDownloadWorker::run — imdb:" << m_imdbId
	              << "languages:" << m_languages << "path:" << m_videoPath;

	if (!client.ensureLoggedIn()) {
		qWarning(lcOS) << "Login failed:" << client.lastError();
		const QString err = QStringLiteral("Login failed: %1").arg(client.lastError());
		for (const QString& lang : m_languages)
			emit languageDone(lang, false, err);
		emit done(0, m_languages.size(), err, -1, client.quotaExceeded(), client.retryAfterSeconds());
		m_client = nullptr;
		return;
	}
	qCDebug(lcOS) << "Login OK";

	const QFileInfo fi(m_videoPath);
	const QString   dir      = fi.absolutePath();
	const QString   basename = fi.completeBaseName();

	// An existing subtitle already on this file (embedded or sidecar, any language) is a far
	// stronger sync signal than comparing total durations — dialogue happens at the same
	// moments regardless of language. Falls back to the duration-only check below when the
	// file has no usable existing subtitle at all.
	emit referenceLookupStarted();
	const std::optional<ReferenceSubtitle> reference =
		pickAndExtractReferenceSubtitle(m_videoPath, m_existingStreams, m_durationSec, &m_cancelled);
	qCDebug(lcOS) << "Sync reference:" << (reference ? reference->label : QStringLiteral("none available"));
	emit referenceLookupDone(reference.has_value(), reference ? reference->label : QString());

	QString movieHash;
	if (m_computeMovieHash) {
		movieHash = OpenSubtitlesClient::computeMovieHash(m_videoPath);
		qCDebug(lcOS) << "moviehash:" << (movieHash.isEmpty() ? QStringLiteral("(unavailable)") : movieHash);
	}

	const auto results = client.search(m_imdbId, m_languages, fi.fileName(), m_editionTokens, movieHash);
	qCDebug(lcOS) << "Search returned" << results.size() << "candidate(s)";
	const bool quotaHitOnSearch = client.quotaExceeded();

	// Candidates arrive pre-ranked (best match first) per language — see
	// candidateScore() in OpenSubtitlesClient::search().
	QHash<QString, QList<OpenSubtitlesClient::SubtitleFile>> resultMap;
	for (const auto& r : results)
		resultMap[r.language].append(r);

	// Capture the folder's timestamps before dropping new .srt files into it.
	const QFileInfo dirFi(dir);
	const QDateTime dirOrigCreated  = dirFi.birthTime();
	const QDateTime dirOrigModified = dirFi.lastModified();

	int downloaded = 0, failed = 0;

	// Special/alternate editions are released and uploaded far less often than the
	// theatrical/standard cut, so the one matching this exact runtime is statistically
	// more likely to be buried lower in the popularity-influenced ranking — worth
	// trying harder (more of the up-to-8 kept candidates) rather than giving up at 3.
	const bool localIsEditionRelease = std::any_of(m_editionTokens.begin(), m_editionTokens.end(),
		[&](const QString& tok) {
			return !tok.isEmpty() && fi.fileName().contains(tok, Qt::CaseInsensitive);
		});
	constexpr int kBaseMaxAttempts    = 3; // caps quota burn chasing a bad match
	constexpr int kEditionMaxAttempts = 8; // matches search()'s kMaxCandidatesPerLang cap
	const int maxAttemptsPerLanguage = localIsEditionRelease ? kEditionMaxAttempts : kBaseMaxAttempts;

	// Sequential, single-client downloads: OpenSubtitles' API flags concurrent
	// logins/requests from the same key as abuse and rate-limits the account,
	// so every language reuses the one JWT obtained above rather than logging
	// in again on its own thread.
	for (const QString& lang : m_languages) {
		if (m_cancelled.loadRelaxed()) {
			qCDebug(lcOS) << "Cancelled — stopping before" << lang;
			emit languageDone(lang, false, QStringLiteral("Cancelled"));
			++failed;
			continue;
		}

		if (!resultMap.contains(lang)) {
			// A 429 during the single /subtitles search call above empties resultMap
			// for every language — report it as the rate limit it is, not "not found".
			if (quotaHitOnSearch) {
				qCDebug(lcOS) << "Quota exceeded — skipping" << lang;
				emit languageDone(lang, false, QStringLiteral("Rate limit reached — try again later"));
			} else {
				qCDebug(lcOS) << "No result for" << lang;
				emit languageDone(lang, false, QStringLiteral("Not found on OpenSubtitles.com"));
			}
			++failed;
			continue;
		}

		emit languageStarted(lang);

		// File name uses the 3-letter code — matches the manual "Set Language" menu
		// and SubtitleLanguageDetector's renames — even though `lang` itself (used
		// for the API query and the signals above) stays ISO 639-1 throughout.
		const QString lang6392  = iso6391to6392(lang);
		const QString fileLang  = !lang6392.isEmpty() ? lang6392 : lang;
		const QString savePath  = QStringLiteral("%1/%2.%3.srt").arg(dir, basename, fileLang);

		const QList<OpenSubtitlesClient::SubtitleFile>& candidates = resultMap.value(lang);
		const int attemptCap = qMin(candidates.size(), maxAttemptsPerLanguage);
		bool    success    = false;
		bool    stopAll    = false;
		QString failReason;

		for (int i = 0; i < attemptCap; ++i) {
			if (m_cancelled.loadRelaxed()) {
				failReason = QStringLiteral("Cancelled");
				break;
			}

			const auto& candidate = candidates.at(i);
			qCDebug(lcOS) << "Trying" << lang << "candidate" << (i + 1) << "of" << attemptCap
			              << "file_id" << candidate.fileId;

			QByteArray content;
			if (!client.downloadToBuffer(candidate.fileId, content)) {
				failReason = client.lastError();
				qWarning(lcOS) << " " << lang << "candidate" << candidate.fileId
				               << "failed:" << failReason;
				if (client.quotaExceeded()) { stopAll = true; break; }
				continue; // try next candidate
			}

			// An existing subtitle track on this file (any language) beats comparing total
			// durations — it can catch fixed offsets and drift, not just wrong-length cuts.
			// Only falls back to the duration-only check when the file has no usable
			// existing subtitle at all (analyzeSubtitleCoverage/subtitleDurationPasses,
			// unchanged from before this reference check existed).
			bool    passed = false;
			QString matchReason;
			SyncMatchResult sync;
			SubtitleCoverage coverage;
			if (reference) {
				const QList<CueTiming> candidateCues = parseSrtCueTimings(content);
				sync   = compareCueTimings(candidateCues, reference->cues);
				passed = syncMatchPasses(sync);
				if (passed) {
					matchReason = QStringLiteral("synced against %1 (%2% aligned, Δ%3s offset)")
						.arg(reference->label)
						.arg(qRound(sync.alignedFraction * 100.0))
						.arg(qRound(sync.medianOffsetSec));
				} else {
					qCDebug(lcOS) << " " << lang << "candidate" << candidate.fileId
					              << "failed sync check vs" << reference->label
					              << "— aligned" << sync.alignedFraction << "drift" << sync.driftSec;
					const QString detail = qAbs(sync.driftSec) > kMaxDriftSec
						? QStringLiteral("drifts increasingly out of sync — possible framerate mismatch")
						: QStringLiteral("only %1% of cues align").arg(qRound(sync.alignedFraction * 100.0));
					failReason = QStringLiteral("Doesn't match %1 (%2)").arg(reference->label, detail);
				}
			} else {
				coverage = analyzeSubtitleCoverage(content, m_durationSec);
				passed   = subtitleDurationPasses(coverage, m_durationSec);
				if (passed) {
					matchReason = candidate.movieHashMatch
					    ? QStringLiteral("exact file match")
					    : (coverage.lastCueEndSec >= 0.0
					           ? QStringLiteral("runtime check passed, Δ%1s")
					                 .arg(qRound(qAbs(coverage.lastCueEndSec - m_durationSec)))
					           : QStringLiteral("no runtime data to verify"));
				} else {
					qCDebug(lcOS) << " " << lang << "candidate" << candidate.fileId
					              << "failed runtime check — subtitle ends at" << coverage.lastCueEndSec
					              << "s (" << coverage.cuesPastDuration << "cues past end), file is"
					              << m_durationSec << "s";
					// Surfaced in the UI tooltip — concrete enough to tell "different cut" from
					// "this check is being overly strict" without needing a debug console attached.
					const QString detail = coverage.lastCueEndSec < m_durationSec
						? QStringLiteral("subtitle ends %1 min before file end")
						      .arg(QString::number((m_durationSec - coverage.lastCueEndSec) / 60.0, 'f', 1))
						: QStringLiteral("%1 cue(s) past file end").arg(coverage.cuesPastDuration);
					failReason = QStringLiteral("Runtime mismatch (%1)").arg(detail);
				}
			}

			if (!passed)
				continue; // try next candidate

			QFile f(savePath);
			if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
				failReason = QStringLiteral("Cannot write to %1: %2").arg(savePath, f.errorString());
				break; // a local write failure won't fix itself on the next candidate
			}
			f.write(content);

			// Which candidate we actually picked, not just why — so a user checking the
			// UI can tell "this release" apart from "some release, trust me".
			const QString candidateLabel = !candidate.release.isEmpty()  ? candidate.release
			                              : !candidate.fileName.isEmpty() ? candidate.fileName
			                              : QStringLiteral("file_id %1").arg(candidate.fileId);
			qCDebug(lcOS) << " " << lang << "OK —" << matchReason << "(" << candidateLabel << ")";
			emit languageDone(lang, true, QStringLiteral("Matched: %1\n%2\nSaved to: %3")
			                                   .arg(candidateLabel, matchReason, savePath));
			success = true;
			break;
		}

		if (success) {
			++downloaded;
			continue;
		}

		++failed;
		emit languageDone(lang, false, failReason.isEmpty()
		                                   ? QStringLiteral("Not found on OpenSubtitles.com")
		                                   : failReason);

		// Once the account is rate-limited, every remaining language will fail
		// the same way — stop hammering the API instead of retrying per-language.
		if (stopAll) {
			const int idx = m_languages.indexOf(lang);
			for (int j = idx + 1; j < m_languages.size(); ++j) {
				emit languageDone(m_languages.at(j), false,
				                  QStringLiteral("Rate limit reached — try again later"));
				++failed;
			}
			break;
		}
	}

#ifdef Q_OS_WIN
	if (downloaded > 0)
		preserveDirTimestamps(dir, dirOrigCreated, dirOrigModified);
#endif

	QString statusMsg;
	if (downloaded > 0)
		statusMsg = QStringLiteral("Downloaded %1 subtitle(s)").arg(downloaded);
	if (failed > 0)
		statusMsg += (statusMsg.isEmpty() ? QString{} : QStringLiteral(", "))
		             + QStringLiteral("%1 failed").arg(failed);
	if (client.remaining() >= 0)
		statusMsg += QStringLiteral(" (%1 downloads remaining today)").arg(client.remaining());

	qCDebug(lcOS) << "Done:" << statusMsg;
	emit done(downloaded, failed, statusMsg, client.remaining(), client.quotaExceeded(),
	          client.retryAfterSeconds());
	m_client = nullptr;
}

} // namespace Mc
