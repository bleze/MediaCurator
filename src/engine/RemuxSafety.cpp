#include "engine/RemuxSafety.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>

namespace Mc::RemuxSafety {

namespace {

// mkvmerge's statistics tags write durations as "HH:MM:SS.nnnnnnnnn".
double parseTagDuration(const QString& text)
{
	static const QRegularExpression re(QStringLiteral(R"(^(\d+):(\d{1,2}):(\d{1,2}(?:\.\d+)?)$)"));
	const QRegularExpressionMatch m = re.match(text.trimmed());
	if (!m.hasMatch()) return -1.0;
	return m.captured(1).toDouble() * 3600.0 + m.captured(2).toDouble() * 60.0 + m.captured(3).toDouble();
}

QString formatDuration(double sec)
{
	const qint64 s = qRound64(qMax(0.0, sec));
	return QStringLiteral("%1:%2:%3")
	    .arg(s / 3600)
	    .arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
	    .arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString tr(const char* text)
{
	return QCoreApplication::translate("RemuxSafety", text);
}

} // namespace

Identify parseIdentifyJson(const QByteArray& json)
{
	Identify info;
	const QJsonObject root = QJsonDocument::fromJson(json).object();
	const QJsonObject container = root.value(QStringLiteral("container")).toObject();
	if (!container.value(QStringLiteral("recognized")).toBool()) return info;
	info.ok = true;

	// Nanoseconds, written as a JSON number.
	const double containerNs = container.value(QStringLiteral("properties")).toObject()
	                               .value(QStringLiteral("duration")).toDouble(-1.0);
	if (containerNs > 0) info.containerSec = containerNs / 1e9;

	const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
	for (const QString type : { QStringLiteral("video"), QStringLiteral("audio") }) {
		bool any = false;
		double longest = -1.0;
		qint64 bytes = 0;
		bool   bytesKnown = false;
		for (const QJsonValue& v : tracks) {
			const QJsonObject track = v.toObject();
			if (track.value(QStringLiteral("type")).toString() != type) continue;
			any = true;
			const QJsonObject props = track.value(QStringLiteral("properties")).toObject();
			longest = std::max(longest, parseTagDuration(props.value(QStringLiteral("tag_duration")).toString()));
			const QJsonValue b = props.value(QStringLiteral("tag_number_of_bytes"));
			if (!b.isUndefined()) {
				bytes += b.isString() ? b.toString().toLongLong() : static_cast<qint64>(b.toDouble());
				bytesKnown = true;
			}
		}
		if (!any) continue;
		info.mainType       = type;
		info.mainTrackSec   = longest;
		info.mainTrackBytes = bytesKnown ? bytes : -1;
		break;
	}
	return info;
}

Identify identify(const QString& mkvmergePath, const QString& path)
{
	QProcess p;
	p.start(mkvmergePath, { QStringLiteral("-J"), path });
	if (!p.waitForFinished(180000)) {
		p.kill();
		p.waitForFinished(5000);
		return {};
	}
	// -J exits 0, or 1 with warnings; 2 means it could not read the file.
	if (p.exitStatus() != QProcess::NormalExit || p.exitCode() > 1) return {};
	return parseIdentifyJson(p.readAllStandardOutput());
}

QString incompleteOutputReason(const Identify& source, const Identify& output, double fallbackSourceSec,
                               bool sourceMayBeUnreadable)
{
	if (!output.ok)
		return tr("The output could not be read back to check that it is complete.");
	if (output.mainType.isEmpty())
		return tr("The output has no video or audio track.");
	if (output.mainTrackSec <= 0.0 || output.mainTrackBytes == 0)
		return tr("The output's %1 track is empty.").arg(output.mainType);

	// Reference length, most comparable first: the same kind of track's own
	// length in the source, then the source's overall length.
	double reference = -1.0;
	if (source.ok && source.mainType == output.mainType && source.mainTrackSec > 0.0)
		reference = source.mainTrackSec;
	else if (source.ok && source.containerSec > 0.0)
		reference = source.containerSec;
	else if (fallbackSourceSec > 0.0)
		reference = fallbackSourceSec;
	if (reference <= 0.0) {
		if (sourceMayBeUnreadable)
			return {};   // nothing to compare with; the empty-track test above still applies
		return tr("The source file could not be read to compare the output with.");
	}

	// Removing tracks never shortens the ones that are kept. Allow for rounding
	// and for a source whose overall length is a little longer than its video.
	const double tolerance = std::max(5.0, reference * 0.01);
	if (output.mainTrackSec < reference - tolerance)
		return tr("The output's %1 runs %2 but the source's runs %3, so mkvmerge did not process the whole file.")
		    .arg(output.mainType, formatDuration(output.mainTrackSec), formatDuration(reference));
	return {};
}

bool hasIncompleteReadWarning(const QString& mkvmergeLog)
{
	static const QRegularExpression re(
	    QStringLiteral(R"(^Warning:.*(exception occurred|will not be processed further|end of file|network error|could not (be )?read|could not write))"),
	    QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
	return re.match(mkvmergeLog).hasMatch();
}

QString environmentalFailureReason(const QString& log)
{
	static const QRegularExpression diskFullRe(
	    QStringLiteral(R"(not enough space|disk (is )?full|no space left|error 112\b)"),
	    QRegularExpression::CaseInsensitiveOption);
	if (diskFullRe.match(log).hasMatch())
		return tr("The disk ran out of space.");

	// Windows error 59 is "An unexpected network error occurred"; mkvmerge
	// writes it as e.g. "Could not write to the destination file: 59 (A)",
	// which the "could not write" pattern covers. (A bare ": 59" would also
	// match mkvmerge's "Progress: 59%" lines.)
	static const QRegularExpression networkRe(
	    QStringLiteral(R"(network error|network name is no longer available|could not be opened for reading|could not write to the destination|error 59\b|did not process the whole file|could not be read back)"),
	    QRegularExpression::CaseInsensitiveOption);
	if (networkRe.match(log).hasMatch())
		return tr("Files on the share could not be read or written, which usually means the network "
		          "connection to the NAS dropped.");
	return {};
}

} // namespace Mc::RemuxSafety
