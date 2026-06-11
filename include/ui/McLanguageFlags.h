#pragma once
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

// English language name ("Danish"), or the raw code if unknown.
[[nodiscard]] QString displayName(const QString& lang);

// Flag pixmap rendered at the given logical height (4:3 aspect, cached).
// Returns a null pixmap when the language has no mapped flag.
[[nodiscard]] QPixmap flag(const QString& lang, int height, qreal dpr);

} // namespace McLanguageFlags
} // namespace Mc
