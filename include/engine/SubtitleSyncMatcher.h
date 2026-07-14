#pragma once

#include <QAtomicInt>
#include <QByteArray>
#include <QList>
#include <QString>
#include <optional>

namespace Mc {

struct StreamRecord;

/**
 * SubtitleSyncMatcher — cross-references a candidate subtitle's cue timing against an
 * existing subtitle track already on the same file (embedded or sidecar, any language,
 * any codec) to verify sync. Dialogue happens at the same moments regardless of
 * language, so an existing track's timestamps are a much stronger signal than comparing
 * total durations — it catches fixed offsets (different intro length between rips) and
 * growing drift (framerate mismatch), neither of which a duration-only check can see.
 *
 * Works for image-based subtitles (PGS/VobSub) exactly as well as text ones, since only
 * cue timestamps are compared — no text/OCR involved.
 */

struct CueTiming {
	double startSec = 0.0;
	double endSec   = 0.0;
};

// Parses SRT-style "-->" timestamp pairs from in-memory content. Candidates are always
// downloaded as .srt (OpenSubtitlesClient always saves as .srt), so this stays a cheap
// in-memory regex parse — no subprocess needed for the side we just downloaded.
QList<CueTiming> parseSrtCueTimings(const QByteArray& srtContent);

// Invokes `ffprobe -show_packets` against sourcePath and returns each packet's
// [pts_time, pts_time + duration_time) as a cue. Works uniformly for embedded streams and
// standalone sidecar files, and for text and image subtitle codecs alike — ffprobe exposes
// packet timestamps the same way regardless of codec.
//
// streamIndex >= 0 selects one stream within a container via `-select_streams`; pass -1 to
// treat sourcePath itself as the subtitle (a sidecar file opened directly).
//
// Subtitle packets are interleaved throughout a container, so a naive full-file scan would
// mean demuxing through the *entire* file just to pull out the (comparatively rare) subtitle
// packets — many minutes on a large Blu-ray remux. durationSec, when known, bounds the scan
// to a handful of short seeked time-windows (`-read_intervals`) spread across the runtime
// instead — still enough spread to judge alignment and drift, in seconds rather than minutes.
// cancelFlag, when non-null, is polled periodically so a stuck/slow extraction can be aborted
// promptly rather than only after its own timeout.
QList<CueTiming> extractCueTimingsViaFfprobe(const QString& sourcePath, int streamIndex,
                                              double durationSec, const QAtomicInt* cancelFlag = nullptr);

// A chosen reference track's extracted timings plus a human-readable label for diagnostics,
// e.g. "embedded eng track" or "sidecar dan.srt".
struct ReferenceSubtitle {
	QList<CueTiming> cues;
	QString          label;
};

// Picks the best existing subtitle stream on this file to use as a sync reference and
// extracts its timings.
//
// Priority: embedded over sidecar (an embedded track is guaranteed authored for this exact
// file; a sidecar could in principle have been placed there for a different rip). Excludes
// forced tracks (sparse, foreign-dialogue-only) and commentary tracks (different dialogue
// entirely) — both would make a poor or misleading reference. A candidate is only trusted
// once its extracted cue count reaches kMinReferenceCues; thinner ones are skipped in favor
// of the next candidate.
//
// Returns nullopt if no existing subtitle track qualifies — callers must fall back to a
// coarser check (e.g. duration-only) in that case.
std::optional<ReferenceSubtitle> pickAndExtractReferenceSubtitle(
	const QString& videoPath, const QList<StreamRecord>& existingStreams,
	double durationSec, const QAtomicInt* cancelFlag = nullptr);

struct SyncMatchResult {
	bool   hasReference    = false; // false if no reference cues were available to compare
	double alignedFraction = 0.0;   // fraction of candidate cues landing near a reference cue
	double medianOffsetSec = 0.0;   // best-fit constant offset between the two tracks
	double driftSec        = 0.0;   // change in offset from early cues to late cues
};

// Thresholds behind syncMatchPasses() — exposed (not file-local) so callers can build
// detailed diagnostic messages, e.g. distinguishing "drifts out of sync" from "just doesn't
// align at all", rather than duplicating these numbers.
inline constexpr double kMinAlignedFraction = 0.85;
inline constexpr double kMaxDriftSec        = 8.0;

// For each candidate cue, finds the nearest reference cue by start time (reference cues are
// naturally time-sorted, so this binary-searches rather than scanning) and takes the delta.
// medianOffsetSec is the median of all deltas — robust to the handful of cues a translator
// split or merged differently. alignedFraction is the fraction of deltas within
// kCueAlignToleranceSec of medianOffsetSec once corrected for that offset. driftSec compares
// the median offset from the first third of cues against the last third — large means the
// offset is growing over the runtime (the framerate-mismatch signature), not merely constant.
SyncMatchResult compareCueTimings(const QList<CueTiming>& candidate, const QList<CueTiming>& reference);

// alignedFraction >= kMinAlignedFraction AND |driftSec| <= kMaxDriftSec. Thresholds are
// initial estimates, tunable against real results the same way the duration-check constants
// were tuned from real testing.
bool syncMatchPasses(const SyncMatchResult& result);

} // namespace Mc
