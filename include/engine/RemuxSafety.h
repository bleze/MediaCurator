#pragma once
#include <QByteArray>
#include <QString>

// Checks that decide whether a remux's output may replace the original file.
// Kept apart from RemuxJob so they can be tested against real logs and files.
//
// Why they exist (2026-11-17): the NAS connection dropped while mkvmerge was
// reading Inside Job (2010). mkvmerge reported it as a *warning* (exit code 1),
// finished the output with almost nothing in it, and the track-layout check
// passed because every expected track was there, just empty. The 24 GB
// original was replaced by a 140 KB file. The output's container duration
// still read 1:44:15, because a merged subtitle ran to the end, so a plain
// duration check would have passed it too.
namespace Mc::RemuxSafety {

// mkvmerge -J results that matter here.
struct Identify {
	bool    ok = false;              // ran and recognized the file
	QString mainType;                // "video", or "audio" for a file without video
	double  mainTrackSec  = -1.0;    // longest statistics DURATION among tracks of mainType
	qint64  mainTrackBytes = -1;     // their NUMBER_OF_BYTES, summed
	double  containerSec  = -1.0;    // segment duration
};

[[nodiscard]] Identify parseIdentifyJson(const QByteArray& json);

// Runs mkvmerge -J on path (blocking: call off the UI thread).
[[nodiscard]] Identify identify(const QString& mkvmergePath, const QString& path);

// Empty when the output is complete enough to replace the source, else a
// sentence saying why not. source may be !ok (e.g. a disc image mkvmerge
// can't identify); fallbackSourceSec is then the reference (ffprobe's
// duration of the source, <= 0 if unknown). With no reference at all the
// output is refused unless sourceMayBeUnreadable — set only for disc images,
// which neither tool may be able to read. For anything else an unreadable
// source means the share is flaky right now, the worst moment to swap files.
[[nodiscard]] QString incompleteOutputReason(const Identify& source, const Identify& output, double fallbackSourceSec,
                                             bool sourceMayBeUnreadable = false);

// True when mkvmerge's log has a warning saying it could not read (or write)
// everything — the exit code is then 1, "warnings", but the output is short.
[[nodiscard]] bool hasIncompleteReadWarning(const QString& mkvmergeLog);

// For a failed job: a short, user-facing cause when the log points at the
// environment (network share, full disk) rather than at this one file —
// later jobs would fail the same way. Empty otherwise.
[[nodiscard]] QString environmentalFailureReason(const QString& log);

} // namespace Mc::RemuxSafety
