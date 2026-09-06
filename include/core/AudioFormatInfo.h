#pragma once
#include <QString>
#include <QStringList>

namespace Mc {

/** Classifies an audio stream's codec/profile/title into display/filter labels. */
class AudioFormatInfo {
public:
	// Fixed, ordered set of options for the library/job-queue "Audio" filter
	// dropdown (McFilterPanel/McJobPanel — same McMultiCheckDropdown widget as
	// the Edition and HDR/DV filters, hardcoded rather than DB-queried since
	// these values are a known, closed set).
	static QStringList filterLabels();

	// Every filterLabels() entry this one audio stream matches — usually more
	// than one at once (e.g. a DTS:X stream also matches "DTS-HD MA/HRA" and
	// plain "DTS"), same layered-label approach as DolbyVisionInfo.
	static QStringList matchingFilterLabels(const QString& codecName, const QString& codecProfile,
	                                         const QString& title);
};

} // namespace Mc
