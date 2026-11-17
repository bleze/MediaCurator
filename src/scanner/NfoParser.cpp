#include "scanner/NfoParser.h"
#include "scanner/ScanWorker.h"
#include "core/DriveActivityMonitor.h"
#include "core/StorageGroupSettings.h"
#include "nfoview/NfoText.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

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
// Restore a folder's or file's creation/modified timestamps after writing an .nfo
// file, so saving metadata doesn't bubble the folder up as "newest" in media libraries
// that sort by Date Modified — and doesn't make an existing .nfo look freshly edited
// either. Mirrors preserveDirTimestamps() in OpenSubtitlesClient.cpp.
static void preservePathTimestamps(const QString& path, const QDateTime& origCreated, const QDateTime& origModified)
{
	HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()),
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

namespace {

// Heuristic: a "scene NFO" worth showing a viewer button for is anything that
// isn't Kodi's <movie> XML (metadata, not release notes) and isn't just a bare
// IMDb link with nothing else in it (see NfoParser::readImdbId's any-.nfo
// fallback — those short files exist purely so Kodi's scraper has an id to
// find, there's nothing to view). Originally this also required a chunk of
// CP437 box-drawing bytes (0xB0-0xDF) to call it "art", but plenty of modern
// release groups' NFOs are pure low-ASCII figlet-style art (dashes, periods,
// apostrophes, pipes — no byte above 0x7F at all), which that check missed
// entirely. Byte content no longer matters here; only size does.
bool looksLikeSceneNfo(const QByteArray& raw)
{
	if (raw.isEmpty()) return false;
	if (raw.contains("<movie") || raw.contains("<?xml"))
		return false;

	static constexpr int kMinContentBytes = 200;
	return raw.size() >= kMinContentBytes;
}

// Byte offset/length of the first "tt" + 7-8 ASCII digits not glued to another
// letter/digit (\btt\d{7,8}\b, done manually since QRegularExpression only
// operates on an already-decoded QString — see writeMovieNfo's scene-NFO
// branch for why that decode step must never happen here). length == 0 means
// no match (AsciiMatch's default offset stays -1).
struct AsciiMatch { int offset = -1; int length = 0; };
AsciiMatch findAsciiImdbId(const QByteArray& data)
{
	auto isAlnum = [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	};
	for (int i = 0; i + 1 < data.size(); ++i) {
		if (data[i] != 't' || data[i + 1] != 't') continue;
		if (i > 0 && isAlnum(data[i - 1])) continue;
		int digits = 0;
		while (i + 2 + digits < data.size() && data[i + 2 + digits] >= '0' && data[i + 2 + digits] <= '9')
			++digits;
		if (digits < 7 || digits > 8) continue;
		const int end = i + 2 + digits;
		if (end < data.size() && isAlnum(data[end])) continue;
		return { i, end - i };
	}
	return {};
}

} // namespace

namespace Mc {

QString NfoParser::nfoPathFor(const QString& videoPath)
{
	const QFileInfo fi(videoPath);
	return fi.dir().filePath(fi.completeBaseName() + ".nfo");
}

QString NfoParser::readImdbId(const QString& videoPath)
{
	static const QRegularExpression re(R"(\btt\d{7,8}\b)");

	auto scanFile = [&](const QString& path) -> QString {
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
		const QString content = QString::fromUtf8(f.readAll());
		const auto m = re.match(content);
		return m.hasMatch() ? m.captured(0) : QString{};
	};

	const QString id = scanFile(nfoPathFor(videoPath));
	if (!id.isEmpty()) return id;

	const QDir dir(QFileInfo(videoPath).absolutePath());

	// Kodi's folder-level convention — unambiguous regardless of library layout.
	if (dir.exists(QStringLiteral("movie.nfo"))) {
		const QString found = scanFile(dir.filePath(QStringLiteral("movie.nfo")));
		if (!found.isEmpty()) return found;
	}

	// The any-.nfo fallback assumes the movie-per-folder layout, where every .nfo
	// in the folder describes this movie. In a flat library folder it would hand
	// this file a *sibling movie's* identity (wrong poster/title/NFO from then on)
	// — so only allow it when this video is the folder's sole video file.
	int videoCount = 0;
	for (const QFileInfo& fi : dir.entryInfoList(QDir::Files)) {
		if (ScanWorker::videoExtensions().contains(fi.suffix().toLower()) && ++videoCount > 1)
			return {};
	}

	for (const QString& name : dir.entryList({"*.nfo"}, QDir::Files)) {
		const QString found = scanFile(dir.filePath(name));
		if (!found.isEmpty()) return found;
	}
	return {};
}

NfoParser::SceneNfoScan NfoParser::scanSceneNfo(const QString& videoPath)
{
	const QString path = nfoPathFor(videoPath);
	if (!QFileInfo::exists(path)) return { false, {} };   // confirmed: no .nfo at all

	QFile f(path);
	if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
		return { std::nullopt, {} };   // exists but locked/inaccessible right now

	const QByteArray raw = f.readAll();
	if (!looksLikeSceneNfo(raw)) return { false, {} };

	return { true, decodeSceneNfoBytes(raw) };
}

QString NfoParser::decodeSceneNfoBytes(const QByteArray& raw)
{
	return NfoText::decodeBytes(raw);
}

QString NfoParser::redecodeCachedSceneNfoText(const QString& cached, bool mayHoldUnrecoveredDamage)
{
	return NfoText::redecodeCachedText(cached, mayHoldUnrecoveredDamage);
}

QStringList NfoParser::sceneNfoImageUrls(const QString& text)
{
	return NfoText::imageUrls(text);
}

QString NfoParser::bbcodeToHtml(const QString& text, const QSet<QString>* inlineImages)
{
	return NfoText::bbcodeToHtml(text, inlineImages);
}

QSet<QString>& NfoParser::ownWrites()
{
	static QSet<QString> s;
	return s;
}

bool NfoParser::checkAndClearOwnWrite(const QString& nfoPath)
{
	return ownWrites().remove(nfoPath);
}

bool NfoParser::writeMovieNfo(const QString& videoPath, const QString& imdbId,
                              const NfoMovieMeta& meta)
{
	DriveActivityMonitor::touchPath(videoPath);
	const QString nfoPath = nfoPathFor(videoPath);
	// Register before the write so the watcher callback (if any) can skip it.
	ownWrites().insert(nfoPath);
	QFile file(nfoPath);

	// Capture the folder's timestamps before writing the .nfo file into it.
	const QFileInfo dirFi(QFileInfo(nfoPath).absolutePath());
	const QDateTime dirOrigCreated  = dirFi.birthTime();
	const QDateTime dirOrigModified = dirFi.lastModified();
	const QString   dirPath         = dirFi.absoluteFilePath();

	// Capture the .nfo file's own timestamps too (if it already exists) — updating
	// metadata shouldn't make an existing .nfo look freshly created/edited, same
	// reasoning as preserving the folder's timestamps above.
	const QFileInfo nfoFi(nfoPath);
	const bool      nfoExisted    = nfoFi.exists();
	const QDateTime nfoOrigCreated  = nfoFi.birthTime();
	const QDateTime nfoOrigModified = nfoFi.lastModified();

	const QString imdbUniqueIdTag =
		QStringLiteral("<uniqueid type=\"imdb\" default=\"true\">%1</uniqueid>").arg(imdbId);
	const QString idTag = QStringLiteral("<id>%1</id>").arg(imdbId);
	const QString tmdbUniqueIdTag = meta.tmdbId > 0
		? QStringLiteral("<uniqueid type=\"tmdb\">%1</uniqueid>").arg(meta.tmdbId)
		: QString();
	const QString titleTag = !meta.title.isEmpty()
		? QStringLiteral("<title>%1</title>").arg(meta.title.toHtmlEscaped())
		: QString();
	const QString originalTitleTag = !meta.originalTitle.isEmpty()
		? QStringLiteral("<originaltitle>%1</originaltitle>").arg(meta.originalTitle.toHtmlEscaped())
		: QString();
	const QString yearTag = meta.year > 0
		? QStringLiteral("<year>%1</year>").arg(meta.year)
		: QString();
	const QString premieredTag = !meta.premiered.isEmpty()
		? QStringLiteral("<premiered>%1</premiered>").arg(meta.premiered)
		: QString();
	const QString ratingsTag = meta.voteAverage > 0.0
		? QStringLiteral(
		      "<ratings>\n"
		      "    <rating name=\"themoviedb\" max=\"10\" default=\"true\">\n"
		      "      <value>%1</value>\n"
		      "      <votes>%2</votes>\n"
		      "    </rating>\n"
		      "  </ratings>")
		      .arg(meta.voteAverage, 0, 'f', 1)
		      .arg(meta.voteCount)
		: QString();

	if (file.exists()) {
		// An existing NFO we can't read (locked by Kodi/Radarr/AV, permissions)
		// must never fall through to the create-branch below — its WriteOnly |
		// Truncate open can still succeed and would destroy the file's content.
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
			return false;
		const QByteArray rawContent = file.readAll();
		file.close();

		// Byte-level check — "<movie" is pure ASCII, so this is exactly as
		// reliable without decoding first, which the scene-NFO branch below
		// must never do (see there).
		if (rawContent.toLower().contains("<movie")) {
			// Kodi's own NFO spec is UTF-8 — and it's what writeMovieNfo itself
			// writes further down — so decoding is safe and correct here.
			QString content = QString::fromUtf8(rawContent);

			// Drop the old, non-standard <imdbid> tag written by earlier versions.
			static const QRegularExpression legacyImdbTagRe(
				R"(\s*<imdbid>.*?</imdbid>)", QRegularExpression::CaseInsensitiveOption);
			content.remove(legacyImdbTagRe);

			// Type-specific — a Kodi-scraped NFO may carry both an imdb and a tmdb
			// <uniqueid>; a type-agnostic pattern would collapse both into one.
			static const QRegularExpression imdbUniqueIdTagRe(
				R"(<uniqueid\s+type="imdb"[^>]*>.*?</uniqueid>)", QRegularExpression::CaseInsensitiveOption);
			static const QRegularExpression tmdbUniqueIdTagRe(
				R"(<uniqueid\s+type="tmdb"[^>]*>.*?</uniqueid>)", QRegularExpression::CaseInsensitiveOption);
			static const QRegularExpression idTagRe(
				R"(<id>.*?</id>)", QRegularExpression::CaseInsensitiveOption);
			static const QRegularExpression titleTagRe(
				R"(<title>.*?</title>)", QRegularExpression::CaseInsensitiveOption);
			static const QRegularExpression originalTitleTagRe(
				R"(<originaltitle>.*?</originaltitle>)", QRegularExpression::CaseInsensitiveOption);
			static const QRegularExpression yearTagRe(
				R"(<year>.*?</year>)", QRegularExpression::CaseInsensitiveOption);
			static const QRegularExpression premieredTagRe(
				R"(<premiered>.*?</premiered>)", QRegularExpression::CaseInsensitiveOption);
			static const QRegularExpression ratingsTagRe(
				R"(<ratings>.*?</ratings>)",
				QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

			auto upsertTag = [&](const QRegularExpression& tagRe, const QString& newTag) {
				// Empty newTag means "caller didn't supply this piece" — leave
				// whatever is already in the file (or absence) untouched.
				if (newTag.isEmpty()) return;
				if (content.contains(tagRe)) {
					content.replace(tagRe, newTag);
				} else {
					const int closeIdx = content.lastIndexOf("</movie>", -1, Qt::CaseInsensitive);
					if (closeIdx >= 0)
						content.insert(closeIdx, "  " + newTag + "\n");
					else
						content += newTag + "\n";
				}
			};

			upsertTag(imdbUniqueIdTagRe, imdbUniqueIdTag);
			upsertTag(idTagRe, idTag);
			upsertTag(tmdbUniqueIdTagRe, tmdbUniqueIdTag);
			upsertTag(titleTagRe, titleTag);
			upsertTag(originalTitleTagRe, originalTitleTag);
			upsertTag(yearTagRe, yearTag);
			upsertTag(premieredTagRe, premieredTag);
			upsertTag(ratingsTagRe, ratingsTag);

			if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
				file.write(content.toUtf8());
#ifdef Q_OS_WIN
				preservePathTimestamps(dirPath, dirOrigCreated, dirOrigModified);
				if (nfoExisted) preservePathTimestamps(nfoPath, nfoOrigCreated, nfoOrigModified);
#endif
				return true;
			}
			return false;
		}

		// Non-XML (scene-release style) NFO: free-form text that already carries an
		// IMDb id/URL somewhere, sometimes wrong from a copy/paste mistake. Never
		// truncate/replace a file like this — only correct the id text in place so
		// everything else (release notes, ASCII art, ...) survives untouched.
		//
		// Patched at the raw byte level — NEVER decoded through
		// QString::fromUtf8, unlike the Kodi branch above. A great many scene
		// NFOs use CP437 (for their box-drawing art) or some other non-UTF-8
		// encoding; fromUtf8 silently replaces every byte it can't decode with
		// U+FFFD, and writing that back out via toUtf8() bakes the loss in
		// permanently — this used to be exactly what this function did here.
		// findAsciiImdbId (below, byte-level, no decode) replaces the
		// QRegularExpression-on-QString search that used to run on `content`
		// for the same reason: decoding the file just to search it for an id
		// was the very thing corrupting it. The giveaway when this had already
		// happened to a file: a perfectly correct IMDb link this function
		// itself appended, sitting inside an otherwise-unreadable file — its
		// id-search no longer matched anything once the surrounding content
		// had already been mangled by this same read-as-UTF8 step on an
		// earlier run.
		QByteArray patched = rawContent;
		const auto idMatch = findAsciiImdbId(patched);
		if (idMatch.length > 0) {
			patched.replace(idMatch.offset, idMatch.length, imdbId.toUtf8());
		} else {
			// No recognizable id at all — append one so Kodi's own scraper can still
			// pick up the match, without touching the existing content above it.
			if (!patched.isEmpty() && !patched.endsWith('\n'))
				patched += '\n';
			patched += QStringLiteral("https://www.imdb.com/title/%1/\n").arg(imdbId).toUtf8();
		}

		if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
			file.write(patched);
#ifdef Q_OS_WIN
			preservePathTimestamps(dirPath, dirOrigCreated, dirOrigModified);
			if (nfoExisted) preservePathTimestamps(nfoPath, nfoOrigCreated, nfoOrigModified);
#endif
			return true;
		}
		return false;
	}

	// Create a fresh minimal Kodi-format NFO. Every meta field is included only
	// when the caller has it (TMDB-backed callers do); otherwise this stays
	// id-only and Kodi (or any other scraper) fills the rest in itself, in
	// whatever language the user's own media center is configured for.
	QString xml = QStringLiteral(
		"<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\n"
		"<movie>\n");
	if (!titleTag.isEmpty())         xml += "  " + titleTag + "\n";
	if (!originalTitleTag.isEmpty()) xml += "  " + originalTitleTag + "\n";
	if (!ratingsTag.isEmpty())       xml += "  " + ratingsTag + "\n";
	if (!yearTag.isEmpty())          xml += "  " + yearTag + "\n";
	if (!premieredTag.isEmpty())     xml += "  " + premieredTag + "\n";
	xml += "  " + imdbUniqueIdTag + "\n";
	xml += "  " + idTag + "\n";
	if (!tmdbUniqueIdTag.isEmpty())  xml += "  " + tmdbUniqueIdTag + "\n";
	xml += QStringLiteral("</movie>\n");

	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		return false;
	file.write(xml.toUtf8());
#ifdef Q_OS_WIN
	preservePathTimestamps(dirPath, dirOrigCreated, dirOrigModified);
#endif
	return true;
}

bool NfoParser::writeSceneNfoFile(const QString& videoPath, const QByteArray& rawContent)
{
	DriveActivityMonitor::touchPath(videoPath);
	const QString nfoPath = nfoPathFor(videoPath);
	ownWrites().insert(nfoPath);

	const QFileInfo dirFi(QFileInfo(nfoPath).absolutePath());
	const QDateTime dirOrigCreated  = dirFi.birthTime();
	const QDateTime dirOrigModified = dirFi.lastModified();
	const QString   dirPath         = dirFi.absoluteFilePath();

	const QFileInfo nfoFi(nfoPath);
	const bool      nfoExisted      = nfoFi.exists();
	const QDateTime nfoOrigCreated  = nfoFi.birthTime();
	const QDateTime nfoOrigModified = nfoFi.lastModified();

	QFile file(nfoPath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		return false;
	file.write(rawContent);
	file.close();
#ifdef Q_OS_WIN
	preservePathTimestamps(dirPath, dirOrigCreated, dirOrigModified);
	if (nfoExisted) preservePathTimestamps(nfoPath, nfoOrigCreated, nfoOrigModified);
#endif
	return true;
}

QString NfoParser::titleFromFilename(const QString& filename)
{
	QString name = QFileInfo(filename).completeBaseName();

	name.replace('.', ' ');
	name.replace('_', ' ');

	// Keep everything up to and including the first release-year
	static const QRegularExpression yearRe(R"(\b(19|20)\d{2}\b)");
	const auto match = yearRe.match(name);
	if (match.hasMatch())
		name = name.left(match.capturedStart() + match.capturedLength());

	return name.simplified();
}

} // namespace Mc
