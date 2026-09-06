#pragma once
#include <QString>
#include <QStringList>

namespace Mc {

/** Formats Dolby Vision profile/compatibility metadata for display and filtering. */
class DolbyVisionInfo {
public:
	// profile: dv_profile (5, 7, 8, ...), -1 = not Dolby Vision.
	// blCompatId: dv_bl_signal_compatibility_id (1=HDR10, 2=SDR, 4=HLG), -1 = unknown.
	// elPresent: true for dual-layer profiles (e.g. Profile 7).
	// elType: "FEL"/"MEL" once a deep scan has determined it, empty otherwise.
	// compact: short form ("P8.1 (HDR10)") for space-constrained badges, vs. the
	// full form ("Profile 8.1 (HDR10-compatible)") for tables/tooltips.
	static QString label(int profile, int blCompatId, bool elPresent,
	                      const QString& elType = QString(), bool compact = false);

	// Fixed, ordered set of options for the library/job-queue "HDR/DV" filter
	// dropdown (McFilterPanel/McJobPanel — same McMultiCheckDropdown widget as
	// the Edition filter, but with a hardcoded list instead of one queried from
	// the DB, since these values are a known, closed set rather than free text).
	static QStringList filterLabels();

	// Every filterLabels() entry this one video stream matches — usually one
	// (e.g. "HDR10"), but a Dolby Vision stream matches several at once (e.g.
	// both "Dolby Vision (any)" and "Dolby Vision — Profile 7, FEL"), so a
	// selection of any of them should show the file.
	static QStringList matchingFilterLabels(const QString& hdrFormat, int dvProfile,
	                                         const QString& dvElType);
};

} // namespace Mc
