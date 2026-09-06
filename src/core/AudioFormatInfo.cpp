#include "core/AudioFormatInfo.h"

namespace Mc {

QStringList AudioFormatInfo::filterLabels()
{
	return {
		QStringLiteral("Dolby Atmos"),
		QStringLiteral("Dolby TrueHD"),
		QStringLiteral("Dolby Digital Plus (E-AC3)"),
		QStringLiteral("Dolby Digital (AC3)"),
		QStringLiteral("DTS"),
		QStringLiteral("DTS-HD MA/HRA"),
		QStringLiteral("DTS:X"),
		QStringLiteral("PCM/LPCM"),
		QStringLiteral("FLAC"),
		QStringLiteral("AAC"),
		QStringLiteral("MP3"),
		QStringLiteral("MP2"),
		QStringLiteral("WMA"),
	};
}

QStringList AudioFormatInfo::matchingFilterLabels(const QString& codecName, const QString& codecProfile,
                                                   const QString& title)
{
	const QString n = codecName.toLower();
	const QString p = codecProfile.toLower();
	const QString t = title.toLower();
	QStringList out;

	const bool isAtmos = (n == QLatin1String("truehd") || n == QLatin1String("eac3")) &&
	                      (p.contains(QLatin1String("atmos")) || t.contains(QLatin1String("atmos")));
	if (isAtmos) out << QStringLiteral("Dolby Atmos");
	if (n == QLatin1String("truehd")) out << QStringLiteral("Dolby TrueHD");
	if (n == QLatin1String("eac3") && !isAtmos) out << QStringLiteral("Dolby Digital Plus (E-AC3)");
	if (n == QLatin1String("ac3")) out << QStringLiteral("Dolby Digital (AC3)");

	const bool isDtsHd = n == QLatin1String("dts") &&
	                      (p.contains(QLatin1String("ma")) || p.contains(QLatin1String("hra")) ||
	                       t.contains(QLatin1String("dts-hd")));
	const bool isDtsX  = n == QLatin1String("dts") &&
	                      (p.contains(QLatin1String("dts:x")) || p.contains(QLatin1String("dts-x")) ||
	                       t.contains(QLatin1String("dts:x")));
	if (n == QLatin1String("dts")) out << QStringLiteral("DTS");
	if (isDtsHd || isDtsX) out << QStringLiteral("DTS-HD MA/HRA");
	if (isDtsX) out << QStringLiteral("DTS:X");

	if (n.startsWith(QLatin1String("pcm"))) out << QStringLiteral("PCM/LPCM");
	if (n == QLatin1String("flac")) out << QStringLiteral("FLAC");
	if (n == QLatin1String("aac")) out << QStringLiteral("AAC");
	if (n == QLatin1String("mp3")) out << QStringLiteral("MP3");
	if (n == QLatin1String("mp2")) out << QStringLiteral("MP2");
	if (n.startsWith(QLatin1String("wma"))) out << QStringLiteral("WMA");

	return out;
}

} // namespace Mc
