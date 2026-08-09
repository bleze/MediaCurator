#include "scanner/NfoParser.h"
#include "scanner/ScanWorker.h"
#include "core/DriveActivityMonitor.h"
#include "core/StorageGroupSettings.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>
#include <cstring>

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

// CP437 code points 0x80-0xFF → Unicode. Scene-release NFO "ASCII" art is
// almost universally authored in this DOS-era codepage for its box-drawing
// and block-shading characters (0xB0-0xDF) — decoding as UTF-8 instead turns
// every one of those into a replacement character (invalid UTF-8 sequence).
constexpr char16_t kCp437High[128] = {
	0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, // 0x80
	0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, // 0x88
	0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, // 0x90
	0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, // 0x98
	0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, // 0xA0
	0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, // 0xA8
	0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, // 0xB0
	0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, // 0xB8
	0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, // 0xC0
	0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, // 0xC8
	0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, // 0xD0
	0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, // 0xD8
	0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, // 0xE0
	0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, // 0xE8
	0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, // 0xF0
	0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0, // 0xF8
};

QString decodeCp437(const QByteArray& raw)
{
	QString out;
	out.reserve(raw.size());
	for (unsigned char c : raw)
		out += (c < 0x80) ? QChar(c) : QChar(kCp437High[c - 0x80]);
	return out;
}

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

// A handful of NFOs found in the wild are internally corrupted in a
// distinctive, consistent way: most of the file is genuine text encoded
// as UTF-16BE, but interspersed with runs of the literal 3-byte sequence
// EF BF BD — the UTF-8 encoding of U+FFFD, the Unicode replacement character.
// That means some earlier processing step (not us — this is already baked
// into the bytes on disk) read raw bytes with the wrong encoding, hit
// sequences it couldn't decode, substituted U+FFFD for them, and re-encoded
// the result as UTF-8 without ever recovering the original bytes. Those
// substituted spans are genuinely, permanently gone — no decode can recover
// them — but the surrounding UTF-16BE text is still intact and worth
// rendering correctly instead of feeding it through the CP437 table, which
// mangles both halves into unrelated garbage.
bool looksLikeCorruptedMixedEncoding(const QByteArray& raw)
{
	if (!raw.contains("\xEF\xBF\xBD")) return false;
	const int zeroBytes = std::count(raw.begin(), raw.end(), '\0');
	// Genuine CP437/ASCII scene NFOs never contain a raw NUL — UTF-16 text
	// does, roughly every other byte for the Latin-1 range. A low bar here
	// still comfortably separates the two.
	return zeroBytes * 20 >= raw.size();   // ~5%+
}

// Substitutes each 3-byte EF BF BD marker for the 2-byte UTF-16BE encoding of
// U+FFFD (which happens to be the same codepoint, just re-packed) so the
// already-lost spans keep showing as a visible "unrecoverable" marker while
// preserving 2-byte alignment for the genuine UTF-16BE text around them —
// substituting anything other-than-a-multiple-of-2-bytes-shorter would shift
// every pair boundary after it, corrupting text that would otherwise decode
// perfectly fine.
QString decodeCorruptedMixedEncoding(const QByteArray& raw)
{
	QByteArray fixed;
	fixed.reserve(raw.size());
	for (int i = 0; i < raw.size();) {
		if (raw.size() - i >= 3 && std::memcmp(raw.constData() + i, "\xEF\xBF\xBD", 3) == 0) {
			fixed.append('\xFF');
			fixed.append('\xFD');
			i += 3;
		} else if (raw.size() - i >= 2 && raw[i] == '\x0D' && raw[i + 1] == '\x0A') {
			// Line breaks show up as a bare 2-byte ASCII "\r\n", not the 4-byte
			// UTF-16BE encoding the surrounding text otherwise uses — left as-is,
			// each pair merges into one unrenderable codepoint (U+0D0A) instead of
			// an actual line break, and every pair boundary after it ends up
			// shifted by one byte. Re-inflating to the properly-paired encoding
			// fixes both at once.
			fixed.append('\x00'); fixed.append('\x0D');
			fixed.append('\x00'); fixed.append('\x0A');
			i += 2;
		} else {
			fixed.append(raw[i]);
			++i;
		}
	}
	if (fixed.size() % 2 != 0) fixed.chop(1);   // stray trailing byte — drop rather than misalign everything

	QStringDecoder decoder(QStringDecoder::Utf16BE);
	return decoder(fixed);
}

// Some NFOs have absurdly long runs of trailing spaces on individual lines —
// padding to a fixed console width from whatever tool generated them, and
// invisible either way. Left in, one such line inflates readSceneNfoArt's
// widest-line width far beyond anything actually visible, which throws off
// the viewer dialog's content-based sizing. Leading whitespace is untouched —
// that's real indentation, load-bearing for art alignment.
QString stripTrailingWhitespacePerLine(const QString& text)
{
	static const QRegularExpression lineBreakRe(R"(\r\n|\r|\n)");
	static const QRegularExpression trailingWsRe(R"([ \t]+$)");
	const QStringList lines = text.split(lineBreakRe);
	QStringList trimmed;
	trimmed.reserve(lines.size());
	for (QString line : lines) {
		line.remove(trailingWsRe);
		trimmed << line;
	}
	return trimmed.join(QLatin1Char('\n'));
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

	const QString text = looksLikeCorruptedMixedEncoding(raw)
	    ? decodeCorruptedMixedEncoding(raw)
	    : decodeCp437(raw);
	return { true, stripTrailingWhitespacePerLine(text) };
}

QString NfoParser::bbcodeToHtml(const QString& text)
{
	QString html = text.toHtmlEscaped();

	// Nested [url=...][img]...[/img][/url] first, so the plain [img]/[url]
	// passes below don't turn it into an <a> nested inside another <a>
	// (invalid HTML) — collapse the whole thing into one link up front.
	static const QRegularExpression nestedUrlImgRe(
		R"(\[url=(https?://[^\]]+)\]\s*\[img\]https?://[^\[]+\[/img\]\s*\[/url\])",
		QRegularExpression::CaseInsensitiveOption);
	html.replace(nestedUrlImgRe, QStringLiteral(R"(<a href="\1">[image]</a>)"));

	// Never fetched/embedded — this stays a local file viewer, not something
	// that reaches out to whatever image host a downloaded NFO points at.
	static const QRegularExpression imgRe(
		R"(\[img\](https?://[^\[]+)\[/img\])", QRegularExpression::CaseInsensitiveOption);
	html.replace(imgRe, QStringLiteral(R"(<a href="\1">[image]</a>)"));

	static const QRegularExpression urlEqRe(
		R"(\[url=(https?://[^\]]+)\](.*?)\[/url\])", QRegularExpression::CaseInsensitiveOption);
	html.replace(urlEqRe, QStringLiteral(R"(<a href="\1">\2</a>)"));

	static const QRegularExpression urlBareRe(
		R"(\[url\](https?://[^\[]+)\[/url\])", QRegularExpression::CaseInsensitiveOption);
	html.replace(urlBareRe, QStringLiteral(R"(<a href="\1">\1</a>)"));

	// No useful info for us — we force our own monospace font regardless, for
	// art alignment — so just drop the tags and keep whatever they wrapped.
	static const QRegularExpression fontOpenRe(R"(\[font=[^\]]*\])", QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression fontCloseRe(R"(\[/font\])", QRegularExpression::CaseInsensitiveOption);
	html.remove(fontOpenRe);
	html.remove(fontCloseRe);

	static const QRegularExpression boldOpenRe(R"(\[b\])", QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression boldCloseRe(R"(\[/b\])", QRegularExpression::CaseInsensitiveOption);
	html.replace(boldOpenRe, QStringLiteral("<b>"));
	html.replace(boldCloseRe, QStringLiteral("</b>"));

	static const QRegularExpression italicOpenRe(R"(\[i\])", QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression italicCloseRe(R"(\[/i\])", QRegularExpression::CaseInsensitiveOption);
	html.replace(italicOpenRe, QStringLiteral("<i>"));
	html.replace(italicCloseRe, QStringLiteral("</i>"));

	static const QRegularExpression underlineOpenRe(R"(\[u\])", QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression underlineCloseRe(R"(\[/u\])", QRegularExpression::CaseInsensitiveOption);
	html.replace(underlineOpenRe, QStringLiteral("<u>"));
	html.replace(underlineCloseRe, QStringLiteral("</u>"));

	// Only a validated hex/named color reaches the style attribute — anything
	// else (typos, unsupported forum-specific color syntax) is left as
	// literal escaped text rather than risking a broken style attribute.
	static const QRegularExpression colorOpenRe(
		R"(\[color=(#[0-9A-Fa-f]{3}|#[0-9A-Fa-f]{6}|[A-Za-z]{2,20})\])",
		QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression colorCloseRe(R"(\[/color\])", QRegularExpression::CaseInsensitiveOption);
	html.replace(colorOpenRe, QStringLiteral(R"(<span style="color:\1">)"));
	html.replace(colorCloseRe, QStringLiteral("</span>"));

	return html;
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
