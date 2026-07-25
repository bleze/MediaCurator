#include "scanner/EditionDetector.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>

namespace Mc {

QString EditionDetector::detect(const FileRecord& file, const QStringList& editionTokens)
{
	// Kodi-style "{edition-Director's Cut}" token — an explicit, unambiguous signal
	// when present, so it's checked before any keyword guessing.
	static const QRegularExpression kodiTag(
	    R"(\{edition-([^}]+)\})", QRegularExpression::CaseInsensitiveOption);
	const auto kodiMatch = kodiTag.match(file.filename);
	if (kodiMatch.hasMatch()) {
		QString label = kodiMatch.captured(1);
		label.replace('.', ' ').replace('_', ' ');
		return label.simplified();
	}

	QString stemmed = QFileInfo(file.filename).completeBaseName();
	stemmed.replace('.', ' ').replace('_', ' ');
	const QString haystack = stemmed + QLatin1Char(' ') + file.containerTitle;

	// 3D is a format dimension, not a narrative cut, so it's deliberately not part
	// of editionTokens (that list is shared with OpenSubtitles release-name
	// matching, where 3D-ness doesn't affect which subtitle to pick) — checked as
	// its own small signal instead of folding it into that list.
	//
	// Packing-format tokens are checked FIRST and independently of any literal "3D"
	// mention: "Half-SBS"/"HSBS"/"HOU"/etc. are unambiguously a 3D encoding on their
	// own — plenty of real releases are named e.g. "Movie.Half-SBS.mkv" with no
	// separate "3D" anywhere in the filename at all. Checked most-specific-first —
	// "HSBS"/"VSBS" before the generic "SBS", since the generic pattern would
	// otherwise match inside them too. Not exhaustive — same spirit as editionTokens.
	struct FormatPattern { QRegularExpression pattern; QString label; };
	static const QList<FormatPattern> packingFormats = {
		{ QRegularExpression(R"(\b(h-?sbs|half[\s._-]*sbs)\b)", QRegularExpression::CaseInsensitiveOption),
		  QStringLiteral("HSBS") },
		{ QRegularExpression(R"(\bvsbs\b)", QRegularExpression::CaseInsensitiveOption),
		  QStringLiteral("VSBS") },
		{ QRegularExpression(R"(\b(h-?ou|h-?tab|half[\s._-]*ou|half[\s._-]*tab)\b)", QRegularExpression::CaseInsensitiveOption),
		  QStringLiteral("HOU") },
		{ QRegularExpression(R"(\bsbs\b)", QRegularExpression::CaseInsensitiveOption),
		  QStringLiteral("SBS") },
		{ QRegularExpression(R"(\b(ou|tab)\b)", QRegularExpression::CaseInsensitiveOption),
		  QStringLiteral("OU") },
		{ QRegularExpression(R"(\bmvc\b)", QRegularExpression::CaseInsensitiveOption),
		  QStringLiteral("MVC") },
	};
	for (const auto& f : packingFormats) {
		if (f.pattern.match(haystack).hasMatch())
			return QStringLiteral("3D (%1)").arg(f.label);
	}

	// No specific packing token — fall back to a loose "3D" mention. Plain \b3d\b
	// covers the normal case, plus a couple of specific compounds where "3D" is
	// glued directly to an adjacent word with no separator ("BluRay3D", "3DBD").
	// Deliberately NOT a blanket "3D next to any letters" rule — that also matches
	// release-group names that happen to end in "3D" (e.g. "-TEKNO3D"), which says
	// nothing about the movie itself.
	static const QRegularExpression threeD(
	    R"(\b3d\b|bluray3d|3dbd|bd3d)", QRegularExpression::CaseInsensitiveOption);
	if (threeD.match(haystack).hasMatch()) {
		// AVC only means anything as a 3D qualifier once we already know the file
		// is 3D (this branch) — checked at top level it would false-positive
		// constantly as just the ordinary h.264 codec tag on non-3D files.
		static const QRegularExpression avc(R"(\bavc\b)", QRegularExpression::CaseInsensitiveOption);
		if (avc.match(haystack).hasMatch())
			return QStringLiteral("3D (AVC)");
		return QStringLiteral("3D");
	}

	// Each entry may list several spellings of the same cut separated by '|' (e.g.
	// "Director's Cut|Directors Cut|DC") — any spelling matching resolves to the
	// first one listed, so variant spellings are recognized as the same edition
	// instead of producing two different labels (see UserProfile::editionTokens()).
	// Groups are checked longest-variant-first, so a more specific multi-word group
	// is tried before a shorter one that might otherwise match a substring of it.
	QList<QStringList> groups;
	groups.reserve(editionTokens.size());
	for (const QString& entry : editionTokens) {
		QStringList variants = entry.split(QLatin1Char('|'), Qt::SkipEmptyParts);
		for (QString& v : variants) v = v.trimmed();
		variants.removeAll(QString());
		if (!variants.isEmpty()) groups.append(variants);
	}
	std::sort(groups.begin(), groups.end(), [](const QStringList& a, const QStringList& b) {
		const auto longestVariant = [](const QStringList& g) {
			int m = 0;
			for (const QString& v : g) m = qMax(m, v.size());
			return m;
		};
		return longestVariant(a) > longestVariant(b);
	});

	// Collect every distinct group that matches rather than stopping at the first —
	// some releases genuinely combine two cuts in one file via seamless branching
	// (e.g. "1408.2007.Theatrical.Directors.Cut...") and dropping the second match
	// would misidentify it as just the one edition.
	QStringList matched;
	for (const QStringList& group : groups) {
		const QString canonical = group.first();
		for (const QString& variant : group) {
			// Single-character variants are skipped as match patterns — anything
			// shorter than that (e.g. a bare "D") is too likely to appear as an
			// unrelated word-bounded token to trust as a strict label here.
			if (variant.size() < 2) continue;
			const QRegularExpression re(
			    QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(variant)),
			    QRegularExpression::CaseInsensitiveOption);
			if (re.match(haystack).hasMatch()) {
				matched << canonical;
				break;   // this group already matched — move on to the next group
			}
		}
	}
	if (matched.isEmpty()) return {};
	return matched.join(QStringLiteral(" & "));
}

} // namespace Mc
