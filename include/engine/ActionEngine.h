#pragma once
#include "engine/TrackDecision.h"
#include <QJsonArray>
#include <QObject>
#include <QStringList>

namespace Mc {

/** Builds mkvmerge / mkvpropedit command-line arguments from a FileDecision or flag change set. */
class ActionEngine : public QObject {
	Q_OBJECT
public:
	explicit ActionEngine(const QString& mkvmergePath, QObject* parent = nullptr);
	QStringList buildCommand(const FileDecision& decision, const QString& outputPath) const;

	// Rebuilds a remux job's mkvmerge command fresh, using CURRENT buildCommand()
	// logic, instead of replaying the literal argument list that was frozen in
	// commandArgsJson back when the job was proposed. A job can sit in
	// Proposed/Queued for a long time; if a bug fix lands in buildCommand() in the
	// meantime (e.g. the mjpeg cover-art/attachment-stripping fix), replaying the
	// frozen command would still reproduce the old, broken behavior even on an
	// updated build. The kept/removed *decision* — including any manual per-track
	// override the user made in the Job Review dialog — is recovered from
	// commandArgsJson via computeKeptStreams() and honored exactly as approved;
	// only the low-level argument construction is redone. Falls back to
	// commandArgsJson verbatim if `file` has no matching output path to rebuild
	// against (stored command not in the expected "-o", path, ... shape).
	QStringList rebuildRemuxCommand(const FileRecord& file,
	                                 const QList<StreamRecord>& streams,
	                                 const QString& commandArgsJson) const;

	// Returns extra mkvmerge flag-setting args to be inserted before the input path.
	// flagChangesJson is the JSON array stored in jobs.flag_changes_json.
	static QStringList buildFlagArgsForRemux(const QString& flagChangesJson);

	// Returns a complete mkvpropedit argument list (including the file path as first arg)
	// for a tag_edit job that only changes flags without remuxing.
	static QStringList buildPropEditArgs(const QString& filePath,
	                                     const QString& flagChangesJson);

	// Strips flag changes that target tracks which the remux is removing.
	// commandArgs is the full mkvmerge args list; streams is the current track list.
	// Returns the filtered flagChangesJson (may be empty if all entries were removed).
	static QString filterFlagChangesForRemux(const QString& flagChangesJson,
	                                          const QStringList& commandArgs,
	                                          const QList<StreamRecord>& streams);

	// Returns flagChangesJson with external (sidecar) stream entries removed.
	// Use this before passing to buildFlagArgsForRemux / buildPropEditArgs.
	static QString filterInternalFlagChanges(const QString& flagChangesJson,
	                                          const QList<StreamRecord>& streams);

	// Merges two flagChangesJson arrays keyed by (streamIndex, flag) — entries in
	// `overrides` win over `base` for the same key. Used to combine RuleEngine's
	// auto-detected Original-flag fixes (base) with a user's own in-panel edits
	// inherited from a pre-existing job (overrides) when re-proposing a job.
	static QString mergeFlagChanges(const QString& base, const QString& overrides);

	// Returns mkvmerge per-file args for sidecar subtitles that have pending flag changes.
	// Append these AFTER the main input path in the mkvmerge command.
	static QStringList buildSidecarArgsForRemux(const QList<StreamRecord>& streams,
	                                             const QString& flagChangesJson);

	// Some subtitle sources (OpenSubtitles included) sometimes serve MicroDVD
	// content — "{startFrame}{endFrame}text" — mislabeled with a .srt extension.
	// mkvmerge sniffs the real content and rejects it outright ("non-supported
	// file type"), which would otherwise fail the whole remux job. If `path`
	// looks like MicroDVD, rewrites it in place as genuine SubRip using fps to
	// turn frame numbers into timestamps. No-op (returns false) if the file
	// doesn't look like MicroDVD, fps isn't positive, or the file can't be
	// read/written — callers should just proceed with the file as-is either way.
	static bool fixMislabeledSidecarSubtitle(const QString& path, double fps);

	// Parses an ffprobe-style frame rate string ("24000/1001", "25/1", "25") into
	// a plain fps double. Returns 0.0 if empty/unparseable.
	static double parseFrameRate(const QString& frameRate);

	// Returns the new sidecar path with the forced indicator added or removed.
	// E.g. "movie.da.srt" + forced=true → "movie.da.forced.srt"
	static QString computeRenamedSidecarPath(const QString& currentPath, bool wantForced);

	// Returns the new sidecar path with an ISO 639-2 language token inserted right
	// after the video's base name. Only meaningful when currentPath has no existing
	// language token (i.e. the sidecar was previously unlabeled).
	// E.g. currentPath="movie.forced.srt", videoBaseName="movie", langCode="eng"
	//      → "movie.eng.forced.srt"
	static QString insertLanguageIntoSidecarPath(const QString& currentPath,
	                                              const QString& videoBaseName,
	                                              const QString& langCode);

	// Serializes a stream list into the compact JSON snapshot format stored in
	// jobs.original_streams_json — a frozen "before" picture so a completed job
	// can still show removed/merged tracks after the live DB has moved on.
	static QString serializeStreamSnapshot(const QList<StreamRecord>& streams);
	// Reverses serializeStreamSnapshot().
	static QList<StreamRecord> deserializeStreamSnapshot(const QString& json);

	// Returns the subset of `all` that a remux job built from commandArgsJson would
	// keep (i.e. everything NOT excluded by --no-audio/--audio-tracks and the
	// equivalent video/subtitle flags). Single source of truth shared by the job
	// panel's display order and JobQueue's live "largest savings first" pick order —
	// they must agree, or the queue can process jobs out of the order shown on screen.
	//
	// includeUnmergedSidecars controls whether an external/sidecar subtitle counts as
	// "kept" even when it won't actually be absorbed into the mkvmerge output. Display
	// call sites want true (the sidecar file still exists, still belongs to the movie).
	// Post-mux verification against a container-only ffprobe rescan must instead pass
	// the same condition JobQueue used to decide whether to append the sidecar to the
	// mkvmerge command (JobQueue::m_mergeSidecarSubtitles) — otherwise the expectation
	// promises a track that was never going to be muxed in.
	static QList<StreamRecord> computeKeptStreams(const QList<StreamRecord>& all,
	                                               const QString& commandArgsJson,
	                                               bool includeUnmergedSidecars = true);

	// Result of comparing the streams a remux was expected to produce against what
	// actually came out of mkvmerge.
	struct StreamDiff {
		QList<StreamRecord> missing;    // expected to survive but absent from the output
		QList<StreamRecord> unexpected; // present in the output but not expected
		bool isEmpty() const { return missing.isEmpty() && unexpected.isEmpty(); }
	};

	// Compares `expected` (e.g. computeKeptStreams() run against the pre-remux
	// snapshot) against `actual` (a fresh ffprobe scan of the remuxed output).
	// Matching is attribute-based (codec/language/channels/resolution/disposition
	// flags) rather than by streamIndex or isExternal, since mkvmerge renumbers
	// tracks and folds absorbed sidecar subtitles into regular internal tracks.
	static StreamDiff diffStreams(const QList<StreamRecord>& expected,
	                               const QList<StreamRecord>& actual);

private:
	// Runs "mkvmerge -J <filePath>" and returns its "attachments" array (each entry
	// has "id" and "content_type"). Attachment IDs are assigned by mkvmerge itself
	// and do NOT correspond to ffprobe stream indices, so they can only be obtained
	// this way. Returns an empty array on any failure (missing tool, timeout,
	// unreadable file) — callers must treat that as "nothing known to strip" and
	// fall back to mkvmerge's default of keeping every attachment.
	QJsonArray identifyAttachments(const QString& filePath) const;

	QString m_mkvmergePath;
};

} // namespace Mc
