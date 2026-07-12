#pragma once
#include <QString>
#include <optional>

namespace Mc {

/**
 * Detects the language of subtitle dialogue text via CLD2 (n-gram text
 * categorization) — used when a sidecar subtitle's filename carries no
 * language token to parse (see ScanWorker::scanSidecarSubtitles).
 */
class SubtitleLanguageDetector {
public:
	struct Result {
		QString code;               // CLD2 language code, e.g. "en", "da", "zh-Hant"
		int     confidencePercent = 0; // share of scanned text attributed to `code`
	};

	// Detection is deliberately conservative: renaming a sidecar from a wrong
	// guess is worse than leaving it unlabeled, so a result is only returned
	// when CLD2 itself reports the read as reliable AND the top language holds
	// at least this share of the scanned text.
	static constexpr int kMinConfidencePercent = 85;

	// sampleText must be plain dialogue prose — strip subtitle markup/timing
	// before calling (see ScanWorker's sidecar text extraction). Returns
	// std::nullopt if the sample is too short or below kMinConfidencePercent.
	static std::optional<Result> detect(const QString& sampleText);
};

} // namespace Mc
