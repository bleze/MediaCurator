#pragma once
#include <QList>
#include <QPair>
#include <QPixmap>
#include <QString>

namespace Mc {

/**
 * McLanguageFlags — maps ISO 639 language codes (639-1 and 639-2 B/T, as found
 * in MKV/ffprobe metadata) to a representative country flag and an English
 * display name. Flag SVGs are vendored from lipis/flag-icons (MIT) under
 * :/flags/<cc>.svg.
 */
namespace McLanguageFlags {

// ISO 3166-1 alpha-2 country code for a language code, or empty if unmapped.
[[nodiscard]] QString countryForLanguage(const QString& lang);

// Returns the ISO 639-1 (2-letter) code for any ISO 639-1/2 variant, or empty if unmapped.
[[nodiscard]] QString toIso1(const QString& langCode);

// Converts an ISO 639-1 (2-letter) code — e.g. from TMDB's original_language —
// to the ISO 639-2/T (3-letter) code used in file/stream language fields.
// Anything that isn't a recognized 2-letter code is returned unchanged.
[[nodiscard]] QString iso6392FromIso1(const QString& iso1);

// English language name ("Danish"), or the raw code if unknown.
[[nodiscard]] QString displayName(const QString& lang);

// (ISO 639-2 code, English display name) for every mapped language,
// sorted by display name — for populating language-picker menus/dialogs.
[[nodiscard]] QList<QPair<QString, QString>> commonLanguages();

// Flag pixmap rendered at the given logical height (4:3 aspect, cached).
// Returns a null pixmap when the language has no mapped flag.
[[nodiscard]] QPixmap flag(const QString& lang, int height, qreal dpr);

} // namespace McLanguageFlags
} // namespace Mc
