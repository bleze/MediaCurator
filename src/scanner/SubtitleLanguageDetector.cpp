#include "scanner/SubtitleLanguageDetector.h"

#include "compact_lang_det.h"

namespace Mc {

std::optional<SubtitleLanguageDetector::Result> SubtitleLanguageDetector::detect(const QString& sampleText)
{
	const QByteArray utf8 = sampleText.toUtf8();
	// CLD2's n-gram scoring needs a real sample to be trustworthy; a handful of
	// short lines (a few forced-subtitle captions, say) isn't enough to trust.
	if (utf8.size() < 64)
		return std::nullopt;

	CLD2::Language language3[3];
	int  percent3[3];
	int  textBytes = 0;
	bool isReliable = false;

	const CLD2::Language top = CLD2::DetectLanguageSummary(
		utf8.constData(), utf8.size(), /*is_plain_text=*/true,
		language3, percent3, &textBytes, &isReliable);

	if (!isReliable || top == CLD2::UNKNOWN_LANGUAGE || percent3[0] < kMinConfidencePercent)
		return std::nullopt;

	const QString code = QString::fromLatin1(CLD2::LanguageCode(top));
	if (code.isEmpty() || code == QLatin1String("un"))
		return std::nullopt;

	return Result{code, percent3[0]};
}

} // namespace Mc
