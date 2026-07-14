#include "engine/SubtitleSyncMatcher.h"
#include "core/DatabaseManager.h"
#include "core/ExternalTools.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcess>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcSyncMatcher, "mc.subtitlesync")

namespace Mc {

QList<CueTiming> parseSrtCueTimings(const QByteArray& srtContent)
{
	static const QRegularExpression re(QStringLiteral(
		R"((\d{2}):(\d{2}):(\d{2})[,.](\d{3})\s*-->\s*(\d{2}):(\d{2}):(\d{2})[,.](\d{3}))"));
	const QString text = QString::fromUtf8(srtContent);
	QRegularExpressionMatchIterator it = re.globalMatch(text);

	QList<CueTiming> cues;
	while (it.hasNext()) {
		const QRegularExpressionMatch m = it.next();
		const double startSec = m.captured(1).toDouble() * 3600.0 + m.captured(2).toDouble() * 60.0
		                       + m.captured(3).toDouble() + m.captured(4).toDouble() / 1000.0;
		const double endSec   = m.captured(5).toDouble() * 3600.0 + m.captured(6).toDouble() * 60.0
		                       + m.captured(7).toDouble() + m.captured(8).toDouble() / 1000.0;
		cues.append({startSec, endSec});
	}
	return cues;
}

// Subtitle packets are interleaved throughout a container, so ffprobe would otherwise have to
// demux the *entire* file just to find them — many minutes on a large remux. Sampling two
// short windows near the start and end (via seeking, not a full linear read) is bounded
// regardless of file size, and is enough spread to judge both alignment and drift (the drift
// check only ever compares early-vs-late cues anyway — a middle sample doesn't add signal a
// two-point comparison doesn't already have, only more seeking).
static QString buildSampledReadIntervals(double durationSec)
{
	if (durationSec <= 0.0)
		return {}; // unknown duration — fall back to an unbounded read rather than guess
	constexpr double kWindowSec = 300.0; // 5 min per window
	const double starts[] = {
		0.0,
		qMax(0.0, durationSec - kWindowSec),
	};
	QStringList parts;
	for (double start : starts)
		parts << QString::number(start, 'f', 1) + QStringLiteral("%+") + QString::number(kWindowSec, 'f', 0);
	return parts.join(QLatin1Char(','));
}

QList<CueTiming> extractCueTimingsViaFfprobe(const QString& sourcePath, int streamIndex,
                                              double durationSec, const QAtomicInt* cancelFlag)
{
	QProcess proc;
	QStringList args = {"-v", "quiet", "-print_format", "json", "-show_packets"};
	if (streamIndex >= 0)
		args << "-select_streams" << QStringLiteral("%1").arg(streamIndex);
	const QString intervals = buildSampledReadIntervals(durationSec);
	if (!intervals.isEmpty())
		args << "-read_intervals" << intervals;
	args << sourcePath;

	// Deliberately NOT ExternalTools::applyBackgroundPriority() here, unlike FfprobeScanner's
	// ambient library scan — this runs synchronously in front of a modal dialog the user is
	// actively watching, so throttling it works directly against responsiveness.
	proc.start(ExternalTools::instance().ffprobePath(), args);

	// Poll rather than a single blocking wait — lets a stuck/slow extraction be cancelled
	// promptly instead of only after its own timeout, and lets that timeout be generous
	// (durationSec is usually known, so this should finish in seconds; when it isn't known,
	// the read is unbounded and could genuinely be slow on a large file).
	QElapsedTimer timer;
	timer.start();
	constexpr qint64 kMaxWaitMs = 20000;
	while (proc.state() != QProcess::NotRunning) {
		if (cancelFlag && cancelFlag->loadRelaxed()) {
			qCDebug(lcSyncMatcher) << "Cancelled extracting cue timings from" << sourcePath;
			proc.kill();
			proc.waitForFinished(1000);
			return {};
		}
		if (timer.hasExpired(kMaxWaitMs)) {
			qWarning(lcSyncMatcher) << "ffprobe timeout extracting cue timings from" << sourcePath;
			proc.kill();
			proc.waitForFinished(1000);
			return {};
		}
		proc.waitForFinished(150);
	}
	if (proc.exitCode() != 0) {
		qWarning(lcSyncMatcher) << "ffprobe exited" << proc.exitCode() << "for" << sourcePath
		                        << "—" << QString::fromUtf8(proc.readAllStandardError()).trimmed();
		return {};
	}

	const QJsonObject root    = QJsonDocument::fromJson(proc.readAllStandardOutput()).object();
	const QJsonArray  packets = root.value(QStringLiteral("packets")).toArray();

	QList<CueTiming> cues;
	cues.reserve(packets.size());
	for (const QJsonValue& v : packets) {
		const QJsonObject pkt = v.toObject();
		bool okStart = false, okDur = false;
		// ffprobe's JSON output represents these as strings, not numbers, to preserve precision.
		const double startSec = pkt.value(QStringLiteral("pts_time")).toString().toDouble(&okStart);
		const double durSec   = pkt.value(QStringLiteral("duration_time")).toString().toDouble(&okDur);
		if (!okStart) continue;
		cues.append({startSec, okDur ? startSec + durSec : startSec});
	}
	return cues;
}

static constexpr int kMinReferenceCues = 50;

std::optional<ReferenceSubtitle> pickAndExtractReferenceSubtitle(
	const QString& videoPath, const QList<StreamRecord>& existingStreams,
	double durationSec, const QAtomicInt* cancelFlag)
{
	// Priority-ordered candidates: embedded before sidecar (an embedded track is guaranteed
	// authored for this exact file), excluding forced (sparse) and commentary (different
	// dialogue entirely) tracks — both would make a poor or misleading reference.
	QList<const StreamRecord*> candidates;
	QList<const StreamRecord*> sidecarCandidates;
	for (const StreamRecord& s : existingStreams) {
		if (s.codecType != QLatin1String("subtitle")) continue;
		if (s.isForced || s.isCommentary) continue;
		if (s.isExternal) sidecarCandidates.append(&s);
		else              candidates.append(&s);
	}
	candidates.append(sidecarCandidates);

	// Bounds worst-case seek time: a remux with many embedded language tracks shouldn't pay
	// for extracting every one of them just to find a usable reference.
	constexpr int kMaxCandidatesTried = 2;
	for (int i = 0; i < candidates.size() && i < kMaxCandidatesTried; ++i) {
		const StreamRecord* s = candidates.at(i);
		if (cancelFlag && cancelFlag->loadRelaxed())
			return std::nullopt;

		const QString source   = s->isExternal ? s->externalPath : videoPath;
		const int     streamIdx = s->isExternal ? -1 : s->streamIndex;
		const QList<CueTiming> cues = extractCueTimingsViaFfprobe(source, streamIdx, durationSec, cancelFlag);
		if (cues.size() < kMinReferenceCues) {
			qCDebug(lcSyncMatcher) << "Skipping reference candidate" << source
			                       << "— only" << cues.size() << "cues, need" << kMinReferenceCues;
			continue;
		}

		const QString label = s->isExternal
			? QStringLiteral("sidecar %1").arg(QFileInfo(s->externalPath).fileName())
			: QStringLiteral("embedded %1 track").arg(s->language.isEmpty() ? QStringLiteral("?") : s->language);
		qCDebug(lcSyncMatcher) << "Using reference" << label << "(" << cues.size() << "cues)";
		return ReferenceSubtitle{cues, label};
	}
	return std::nullopt;
}

static double medianOf(QList<double> values)
{
	if (values.isEmpty()) return 0.0;
	std::sort(values.begin(), values.end());
	const int n = values.size();
	return (n % 2 == 1) ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

// Nearest reference start value (not a delta) via binary search — reference starts are
// already time-sorted (ffprobe emits packets in stream order).
static double nearestRefStart(const QList<double>& sortedRefStarts, double candidateStart)
{
	auto it = std::lower_bound(sortedRefStarts.begin(), sortedRefStarts.end(), candidateStart);
	if (it == sortedRefStarts.begin())
		return *it;
	if (it == sortedRefStarts.end())
		return *std::prev(it);
	const double after  = *it;
	const double before = *std::prev(it);
	return (after - candidateStart) < (candidateStart - before) ? after : before;
}

static constexpr double kCueAlignToleranceSec = 2.0;
// Reference cues only come from a handful of sampled windows (see
// buildSampledReadIntervals), not the whole film — most candidate cues have no reference
// data anywhere near them. "Nearest reference cue" for one of those is from a disconnected,
// unrelated part of the film, not a real sync measurement, and must be discarded rather than
// treated as a (wildly wrong) offset. The gaps between sampled windows run to tens of minutes,
// so anything under this rules out a cross-window false match while still covering a cue near
// a window's edge plus a generous sync-offset margin.
static constexpr double kMaxRefProximitySec = 400.0;
static constexpr int    kMinEvaluatedCues   = 20;

SyncMatchResult compareCueTimings(const QList<CueTiming>& candidate, const QList<CueTiming>& reference)
{
	SyncMatchResult result;
	if (candidate.isEmpty() || reference.isEmpty())
		return result; // hasReference stays false

	QList<double> refStarts;
	refStarts.reserve(reference.size());
	for (const CueTiming& c : reference)
		refStarts.append(c.startSec);
	std::sort(refStarts.begin(), refStarts.end());

	QList<double> deltas;
	deltas.reserve(candidate.size());
	for (const CueTiming& c : candidate) {
		const double refStart = nearestRefStart(refStarts, c.startSec);
		if (std::abs(refStart - c.startSec) > kMaxRefProximitySec)
			continue; // no reference data near this cue — not evaluable, not a mismatch either
		deltas.append(refStart - c.startSec);
	}

	result.hasReference = true;
	if (deltas.size() < kMinEvaluatedCues)
		return result; // too little overlap with the sampled windows to judge (alignedFraction stays 0)

	result.medianOffsetSec = medianOf(deltas);

	int aligned = 0;
	for (double d : deltas)
		if (std::abs(d - result.medianOffsetSec) <= kCueAlignToleranceSec)
			++aligned;
	result.alignedFraction = double(aligned) / double(deltas.size());

	// Drift: does the offset grow between early cues and late cues, rather than staying
	// constant? A fixed intro-offset stays flat here; a framerate mismatch does not.
	const int third = qMax(1, deltas.size() / 3);
	const QList<double> earlyDeltas = deltas.mid(0, third);
	const QList<double> lateDeltas  = deltas.mid(deltas.size() - third, third);
	result.driftSec = medianOf(lateDeltas) - medianOf(earlyDeltas);

	return result;
}

bool syncMatchPasses(const SyncMatchResult& result)
{
	if (!result.hasReference) return false;
	return result.alignedFraction >= kMinAlignedFraction && std::abs(result.driftSec) <= kMaxDriftSec;
}

} // namespace Mc
