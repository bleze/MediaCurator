#pragma once
#include "core/DatabaseManager.h"
#include <QString>
#include <QStringList>

namespace Mc {

// Heuristics to detect a movie's edition/cut (Theatrical, Director's Cut, 3D, ...)
// from its filename and container title tag, for grouping multiple releases of the
// same movie in the library's "Group by Movie" view.
class EditionDetector {
public:
	// editionTokens: the same list OpenSubtitles release-name matching already uses
	// (UserProfile::editionTokens()) — one shared, user-editable list instead of a
	// second one maintained here. Returns a canonical edition label (e.g. "Director's
	// Cut", "3D"), or an empty string if nothing was detected.
	static QString detect(const FileRecord& file, const QStringList& editionTokens);
};

} // namespace Mc
