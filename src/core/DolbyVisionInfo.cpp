#include "core/DolbyVisionInfo.h"

namespace Mc {

QString DolbyVisionInfo::label(int profile, int blCompatId, bool elPresent,
                                const QString& elType, bool compact)
{
	if (profile < 0)
		return {};

	const QString prefix = compact ? QStringLiteral("P") : QStringLiteral("Profile ");

	QString compat;
	switch (blCompatId) {
		case 1:  compat = compact ? QStringLiteral("HDR10") : QStringLiteral("HDR10-compatible"); break;
		case 2:  compat = compact ? QStringLiteral("SDR")   : QStringLiteral("SDR-compatible");   break;
		case 4:  compat = compact ? QStringLiteral("HLG")   : QStringLiteral("HLG-compatible");   break;
		default: break;
	}

	if (profile == 8) {
		const QString sub = blCompatId >= 0 ? QString("8.%1").arg(blCompatId) : QStringLiteral("8");
		return compat.isEmpty() ? prefix + sub
		                        : QString("%1%2 (%3)").arg(prefix, sub, compat);
	}

	if (elPresent) {
		const QString layer = elType.isEmpty() ? QStringLiteral("dual-layer") : elType;
		return QString("%1%2 (%3)").arg(prefix).arg(profile).arg(layer);
	}

	return compat.isEmpty() ? QString("%1%2").arg(prefix).arg(profile)
	                        : QString("%1%2 (%3)").arg(prefix).arg(profile).arg(compat);
}

QStringList DolbyVisionInfo::filterLabels()
{
	return {
		QStringLiteral("HDR10"),
		QStringLiteral("HDR10+"),
		QStringLiteral("HLG"),
		QStringLiteral("Dolby Vision (any)"),
		QStringLiteral("Dolby Vision - Profile 5"),
		QStringLiteral("Dolby Vision - Profile 7 (dual-layer)"),
		QStringLiteral("Dolby Vision - Profile 7, FEL"),
		QStringLiteral("Dolby Vision - Profile 7, MEL"),
		QStringLiteral("Dolby Vision - Profile 7, not deep-scanned"),
		QStringLiteral("Dolby Vision - Profile 8.x"),
	};
}

QStringList DolbyVisionInfo::matchingFilterLabels(const QString& hdrFormat, int dvProfile,
                                                   const QString& dvElType)
{
	if (hdrFormat == QLatin1String("HDR10"))  return { QStringLiteral("HDR10") };
	if (hdrFormat == QLatin1String("HDR10+")) return { QStringLiteral("HDR10+") };
	if (hdrFormat == QLatin1String("HLG"))    return { QStringLiteral("HLG") };
	if (hdrFormat != QLatin1String("DolbyVision"))
		return {};

	QStringList out{ QStringLiteral("Dolby Vision (any)") };
	if (dvProfile == 5) {
		out << QStringLiteral("Dolby Vision - Profile 5");
	} else if (dvProfile == 7) {
		out << QStringLiteral("Dolby Vision - Profile 7 (dual-layer)");
		if (dvElType == QLatin1String("FEL"))
			out << QStringLiteral("Dolby Vision - Profile 7, FEL");
		else if (dvElType == QLatin1String("MEL"))
			out << QStringLiteral("Dolby Vision - Profile 7, MEL");
		else
			out << QStringLiteral("Dolby Vision - Profile 7, not deep-scanned");
	} else if (dvProfile == 8) {
		out << QStringLiteral("Dolby Vision - Profile 8.x");
	}
	return out;
}

} // namespace Mc
