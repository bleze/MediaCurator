#include "scanner/OriginalLanguageDetector.h"

namespace Mc {

QString OriginalLanguageDetector::detect(const FileRecord& /*file*/, const QList<StreamRecord>& streams)
{
	// Prefer a track the container itself marks as original (Matroska FlagOriginal,
	// exposed by ffprobe as disposition.original / StreamRecord.isOriginal) — this is
	// how a user-corrected "Set as Original" edit survives a later re-analysis instead
	// of being silently overwritten by the track-order guess below.
	for (const StreamRecord& s : streams) {
		if (s.codecType == "audio" && s.isOriginal && !s.language.isEmpty() && s.language != "und")
			return s.language;
	}
	// Fallback heuristic: the language of the first non-undetermined, non-empty audio
	// track. Works for the vast majority of single-language source files that have no
	// FlagOriginal set at all.
	for (const StreamRecord& s : streams) {
		if (s.codecType == "audio" && !s.language.isEmpty() && s.language != "und")
			return s.language;
	}
	return {};
}

} // namespace Mc
