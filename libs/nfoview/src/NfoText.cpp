#include "nfoview/NfoText.h"

#include <QByteArray>
#include <QHash>
#include <QRegularExpression>
#include <QStringDecoder>

#include <algorithm>
#include <cstring>

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

// CP437 0x01-0x1F: DOS draws these as symbols (smileys, card suits, a bullet,
// arrows), and NFO art uses them — the bullet at 0x07 most of all. Left as
// control characters they render as "missing glyph" boxes. 0 = keep the byte:
// tab, LF and CR mean what they say, NUL and the 0x1A end-of-file mark are not art.
constexpr char16_t kCp437Low[32] = {
	0,      0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022, // 0x00
	0x25D8, 0,      0,      0x2642, 0x2640, 0,      0x266B, 0x263C, // 0x08
	0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8, // 0x10
	0x2191, 0x2193, 0,      0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC, // 0x18
};
constexpr char16_t kCp437Del = 0x2302;   // 0x7F, the "house"

// Applied whatever encoding the text was decoded from: a control character
// has no other sensible rendering in an NFO.
QString mapControlSymbols(QString text)
{
	for (QChar& ch : text) {
		const char16_t u = ch.unicode();
		if (u < 0x20 && kCp437Low[u])
			ch = QChar(kCp437Low[u]);
		else if (u == 0x7F)
			ch = QChar(kCp437Del);
	}
	return text;
}

QString decodeCp437(const QByteArray& raw)
{
	QString out;
	out.reserve(raw.size());
	for (unsigned char c : raw)
		out += (c < 0x80) ? QChar(c) : QChar(kCp437High[c - 0x80]);
	return out;
}

// Unicode character → the CP437 byte that decodes to it (bytes 0x80 and up,
// plus the control-range symbols).
const QHash<char16_t, char>& cp437Reverse()
{
	static const QHash<char16_t, char> reverse = [] {
		QHash<char16_t, char> h;
		for (int i = 0; i < 128; ++i)
			h.insert(kCp437High[i], static_cast<char>(0x80 + i));
		for (int i = 0; i < 32; ++i) {
			if (kCp437Low[i]) h.insert(kCp437Low[i], static_cast<char>(i));
		}
		h.insert(kCp437Del, '\x7F');
		return h;
	}();
	return reverse;
}

// A common way NFOs get destroyed (older MediaCurator versions did it too):
// the CP437 file was read as UTF-8 and saved again. Qt's decoder turns each
// byte that isn't part of a valid UTF-8 sequence — nearly all the art — into
// one U+FFFD, so that byte is gone for good. Bytes that happened to form a
// valid sequence (two art bytes such as DB B2 make U+06F2) came through
// unchanged. The damaged file is valid UTF-8, and decoded as such it shows
// replacement characters with stray Arabic or Syriac letters in between.
bool looksUtf8Damaged(const QString& text)
{
	qsizetype lost = 0, other = 0;
	for (const QChar ch : text) {
		if (ch.unicode() == 0xFFFD) ++lost;
		else if (ch.unicode() >= 0x80) ++other;
	}
	return lost >= 16 && lost >= 2 * other;
}

// Decodes such a file from its bytes: each EF BF BD is one lost cell (U+FFFD),
// and every other multi-byte sequence is the original CP437 bytes, one cell
// each. Since Qt replaced invalid bytes one for one, the cells keep their
// columns. raw must be valid UTF-8 without a byte-order mark.
QString recoverUtf8Damage(const QByteArray& raw)
{
	QString out;
	out.reserve(raw.size());
	for (qsizetype i = 0; i < raw.size();) {
		const auto c = static_cast<unsigned char>(raw[i]);
		if (c < 0x80) {
			out += QChar(c);
			++i;
		} else if (raw.size() - i >= 3 && std::memcmp(raw.constData() + i, "\xEF\xBF\xBD", 3) == 0) {
			out += QChar(0xFFFD);
			i += 3;
		} else {
			const qsizetype len = qMin(raw.size() - i, qsizetype(c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2));
			out += decodeCp437(raw.mid(i, len));
			i += len;
		}
	}
	return out;
}

// Inverse of decodeCp437: false if text holds a character CP437 can't encode,
// i.e. it was not produced by decodeCp437 in the first place.
bool encodeCp437(const QString& text, QByteArray& out)
{
	const QHash<char16_t, char>& reverse = cp437Reverse();
	out.clear();
	out.reserve(text.size());
	for (const QChar ch : text) {
		const char16_t u = ch.unicode();
		if (u < 0x80) {
			out.append(static_cast<char>(u));
			continue;
		}
		const auto it = reverse.constFind(u);
		if (it == reverse.constEnd()) return false;
		out.append(*it);
	}
	return true;
}

// Plain-text NFOs from (mostly European) groups are often Windows-1252/Latin-1
// rather than CP437: "Französisch" with ö as 0xF6, which the CP437 table turns
// into "Franz÷sisch". Told apart by shape, not by byte value (0xC0-0xDF means
// box-drawing in one codepage and accented capitals in the other): accented
// letters sit singly or in pairs inside a word, art comes in runs. So: every
// high byte is 0xC0+ (the Latin-1 letter range; CP437's own accented letters
// and its shading/vertical box characters are all below that), and every run
// of them is at most 2 long and touches an ASCII letter.
bool looksLikeLatin1Text(const QByteArray& raw)
{
	auto isAsciiLetter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
	bool any = false;
	for (qsizetype i = 0; i < raw.size();) {
		const unsigned char c = static_cast<unsigned char>(raw[i]);
		if (c < 0x80) { ++i; continue; }
		const qsizetype start = i;
		while (i < raw.size() && static_cast<unsigned char>(raw[i]) >= 0x80) {
			if (static_cast<unsigned char>(raw[i]) < 0xC0) return false;
			++i;
		}
		if (i - start > 2) return false;
		const bool letterBefore = start > 0 && isAsciiLetter(raw[start - 1]);
		const bool letterAfter  = i < raw.size() && isAsciiLetter(raw[i]);
		if (!letterBefore && !letterAfter) return false;
		any = true;
	}
	return any;
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

} // namespace

namespace Mc::NfoText {

static QString convertBbcode(const QString& text, const QSet<QString>* inlineImages, QStringList* foundUrls);

static QString decodeByEncoding(const QByteArray& raw);

QString decodeBytes(const QByteArray& raw)
{
	return mapControlSymbols(decodeByEncoding(raw));
}

static QString decodeByEncoding(const QByteArray& raw)
{
	if (looksLikeCorruptedMixedEncoding(raw))
		return stripTrailingWhitespacePerLine(decodeCorruptedMixedEncoding(raw));

	// Some NFOs are UTF-8 rather than CP437 (modern groups, or a file re-saved
	// by a tool along the way). Feeding those through the CP437 table turns each
	// multi-byte character into 2-3 unrelated glyphs and pushes the rest of the
	// line out of alignment. Strict whole-file validation keeps real CP437 art
	// out of this branch: its runs of 0xB0-0xDF bytes are never valid UTF-8.
	const bool hasHighByte = std::any_of(raw.begin(), raw.end(),
	                                     [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
	if (hasHighByte && raw.isValidUtf8()) {
		const qsizetype bom = raw.startsWith("\xEF\xBB\xBF") ? 3 : 0;
		const QString text = QString::fromUtf8(raw.constData() + bom, raw.size() - bom);
		return stripTrailingWhitespacePerLine(looksUtf8Damaged(text) ? recoverUtf8Damage(raw.mid(bom)) : text);
	}
	if (looksLikeLatin1Text(raw))
		return stripTrailingWhitespacePerLine(QString::fromLatin1(raw));
	return stripTrailingWhitespacePerLine(decodeCp437(raw));
}

qsizetype lostCharacterCount(const QString& text)
{
	return text.count(QChar(0xFFFD));
}

QString redecodeCachedText(const QString& cached, bool mayHoldUnrecoveredDamage)
{
	// An older decoder cached a damaged file as its plain UTF-8 text, which
	// re-encodes to the file's exact bytes (trailing blanks aside). Recovered
	// text must not go through this again — its CP437 characters would be
	// taken for surviving byte sequences — which is why the caller has to say.
	if (mayHoldUnrecoveredDamage && looksUtf8Damaged(cached))
		return decodeBytes(cached.toUtf8());

	// Text cached before control characters were mapped to their symbols still
	// holds them raw; that part needs no bytes, so it applies in every case.
	const QString mapped = mapControlSymbols(cached);
	QByteArray raw;
	if (!encodeCp437(mapped, raw)) return mapped;
	// Only the UTF-8 correction is applied here, because only that one is
	// unambiguous: mis-decoded UTF-8 leaves multi-byte sequences no correctly
	// decoded text re-encodes to. The Latin-1 detection is NOT safe on rebuilt
	// bytes — already-correct text containing e.g. "ß" re-encodes to CP437
	// 0xE1, which that heuristic would then read as Latin-1 "á". Latin-1 NFOs
	// are corrected from their real bytes the next time they are scanned.
	const bool hasHighByte = std::any_of(raw.begin(), raw.end(),
	                                     [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
	if (!hasHighByte || !raw.isValidUtf8()) return mapped;
	return decodeBytes(raw);
}

// One image-markup shape (BBCode or pasted HTML): swaps every match for either
// an inline <img> (when its URL is in inlineImages) or a plain "[image]" link.
// The image URL is reported/looked up unescaped; the html itself stays escaped.
static QString replaceImageMarkup(const QString& html, const QRegularExpression& re, int linkGroup, int srcGroup,
                                  const QSet<QString>* inlineImages, QStringList* foundUrls)
{
	QString out;
	qsizetype last = 0;
	auto it = re.globalMatch(html);
	while (it.hasNext()) {
		const QRegularExpressionMatch m = it.next();
		const QString link = m.captured(linkGroup).trimmed();
		const QString src  = m.captured(srcGroup).trimmed();
		QString key = src;
		key.replace(QLatin1String("&amp;"), QLatin1String("&"));
		if (foundUrls && !foundUrls->contains(key))
			foundUrls->append(key);
		out += QStringView(html).mid(last, m.capturedStart() - last);
		if (inlineImages && inlineImages->contains(key))
			out += QStringLiteral(R"(<a href="%1"><img src="%2"></a>)").arg(link, src);
		else
			out += QStringLiteral(R"(<a href="%1">[image]</a>)").arg(link);
		last = m.capturedEnd();
	}
	out += QStringView(html).mid(last);
	return out;
}

QStringList imageUrls(const QString& text)
{
	QStringList urls;
	(void)convertBbcode(text, nullptr, &urls);
	return urls;
}

QString bbcodeToHtml(const QString& text, const QSet<QString>* inlineImages)
{
	return convertBbcode(text, inlineImages, nullptr);
}

static QString convertBbcode(const QString& text, const QSet<QString>* inlineImages, QStringList* foundUrls)
{
	QString html = text.toHtmlEscaped();

	// Nested [url=...][img]...[/img][/url] first, so the plain [img]/[url]
	// passes below don't turn it into an <a> nested inside another <a>
	// (invalid HTML) — collapse the whole thing into one link up front.
	static const QRegularExpression nestedUrlImgRe(
		R"(\[url=(https?://[^\]]+)\]\s*\[img\](https?://[^\[]+)\[/img\]\s*\[/url\])",
		QRegularExpression::CaseInsensitiveOption);
	html = replaceImageMarkup(html, nestedUrlImgRe, 1, 2, inlineImages, foundUrls);

	// Nothing is fetched here — an image only becomes an inline <img> once the
	// caller (McNfoView) has downloaded it and passes its URL back in
	// via inlineImages; until then, and on any failure, it stays a link.
	static const QRegularExpression imgRe(
		R"(\[img\](https?://[^\[]+)\[/img\])", QRegularExpression::CaseInsensitiveOption);
	html = replaceImageMarkup(html, imgRe, 1, 1, inlineImages, foundUrls);

	// Some groups paste the image host's raw HTML embed snippet instead of
	// BBCode (<a href="..."><img src="..."/></a>). Matched in its escaped form
	// (toHtmlEscaped above turned < > " into entities), so only these exact
	// shapes become markup.
	static const QRegularExpression htmlLinkedImgRe(
		R"(&lt;a\s(?:(?!&gt;).)*?href=(?:&quot;|')(https?://(?:(?!&quot;|'|&gt;)\S)+)(?:&quot;|')(?:(?!&gt;).)*&gt;\s*&lt;img\s(?:(?!&gt;).)*?src=(?:&quot;|')(https?://(?:(?!&quot;|'|&gt;)\S)+)(?:&quot;|')(?:(?!&gt;).)*&gt;\s*&lt;/a&gt;)",
		QRegularExpression::CaseInsensitiveOption);
	html = replaceImageMarkup(html, htmlLinkedImgRe, 1, 2, inlineImages, foundUrls);

	static const QRegularExpression htmlImgRe(
		R"(&lt;img\s(?:(?!&gt;).)*?src=(?:&quot;|')(https?://(?:(?!&quot;|'|&gt;)\S)+)(?:&quot;|')(?:(?!&gt;).)*&gt;)",
		QRegularExpression::CaseInsensitiveOption);
	html = replaceImageMarkup(html, htmlImgRe, 1, 1, inlineImages, foundUrls);

	static const QRegularExpression htmlAnchorRe(
		R"(&lt;a\s(?:(?!&gt;).)*?href=(?:&quot;|')(https?://(?:(?!&quot;|'|&gt;)\S)+)(?:&quot;|')(?:(?!&gt;).)*&gt;((?:(?!&lt;a\s|<a\s).)*?)&lt;/a&gt;)",
		QRegularExpression::CaseInsensitiveOption);
	html.replace(htmlAnchorRe, QStringLiteral(R"(<a href="\1">\2</a>)"));

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

} // namespace Mc::NfoText
