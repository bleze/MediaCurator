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

	// Returns mkvmerge per-file args for sidecar subtitles that have pending flag changes.
	// Append these AFTER the main input path in the mkvmerge command.
	static QStringList buildSidecarArgsForRemux(const QList<StreamRecord>& streams,
	                                             const QString& flagChangesJson);

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
	static QList<StreamRecord> computeKeptStreams(const QList<StreamRecord>& all,
	                                               const QString& commandArgsJson);

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
