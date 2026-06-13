#pragma once
#include "engine/TrackDecision.h"
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

private:
	QString m_mkvmergePath;
};

} // namespace Mc
