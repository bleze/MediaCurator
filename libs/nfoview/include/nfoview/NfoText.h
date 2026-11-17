#pragma once
#include <QSet>
#include <QString>
#include <QStringList>

class QByteArray;

// Decoding and markup conversion for scene-release NFO text. Pure functions on
// bytes/strings — no file, database or network access — shared by MediaCurator
// and the standalone Hot DAMN NFO Viewer.
namespace Mc::NfoText {

// Decodes raw NFO bytes to text. Almost all scene NFOs are CP437 (box-drawing
// and block-shading art above 0x7F); UTF-8, Latin-1/Windows-1252 and one
// known corrupted mixed UTF-16BE shape are detected and decoded as such.
// Trailing whitespace is stripped per line; line breaks come back as '\n'.
// A file that some tool re-saved as UTF-8 has lost most of its art for good;
// what survived is restored, and each lost character comes back as U+FFFD.
QString decodeBytes(const QByteArray& raw);

// Number of characters in decoded text whose content was lost (see decodeBytes).
qsizetype lostCharacterCount(const QString& text);

// Brings text decoded by an older version of decodeBytes up to date without
// the original file, as far as that can be done exactly: CP437 decoding is
// 1:1, so the bytes can be rebuilt from the text (text read as CP437 although
// the file was UTF-8 is corrected), and so can a damaged file's bytes from its
// text as decoded before recovery existed. mayHoldUnrecoveredDamage says the
// text comes from such a decoder; text that has been through recovery must
// pass false. Returns the input unchanged when nothing applies.
QString redecodeCachedText(const QString& cached, bool mayHoldUnrecoveredDamage);

// Converts the subset of forum BBCode that NFO templates use — [b] [i] [u]
// [color=...] [url] — into HTML; [font=...] is dropped. Images ([img], or a
// pasted HTML <a><img></a> snippet) become a plain "[image]" link unless their
// URL is listed in inlineImages, in which case an <img src> is emitted. Never
// fetches anything itself. Everything else is HTML-escaped first, so the result
// is safe for QTextBrowser::setHtml(); wrap it in <pre> to keep art alignment.
QString bbcodeToHtml(const QString& text, const QSet<QString>* inlineImages = nullptr);

// Image URLs (http/https only, de-duplicated, in document order) that
// bbcodeToHtml() would turn into "[image]" links for this text.
QStringList imageUrls(const QString& text);

} // namespace Mc::NfoText
