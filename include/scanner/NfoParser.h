#pragma once
#include <QSet>
#include <QString>

namespace Mc {

class NfoParser {
public:
	// Canonical NFO path for a video file: same dir, same base name, .nfo extension
	static QString nfoPathFor(const QString& videoPath);

	// Read the first IMDb ID found in the NFO for this video (empty if none)
	static QString readImdbId(const QString& videoPath);

	// Write (or update) the NFO file next to videoPath.
	// If a Kodi-style <movie> XML already exists, only the <uniqueid type="imdb">
	// and <id> elements are touched (any legacy <imdbid> tag is dropped); all
	// other content is preserved.  If no NFO exists, a minimal one is created
	// with title, year, and both id tags.
	// Also registers nfoPath in the own-write suppression set so a future
	// QFileSystemWatcher callback can skip it via checkAndClearOwnWrite().
	static bool writeMovieNfo(const QString& videoPath,
	                           const QString& imdbId,
	                           const QString& title = {},
	                           int year = 0);

	// Extract a search-friendly title from a video filename.
	// "The.Dark.Knight.2008.BluRay.1080p.mkv" → "The Dark Knight 2008"
	static QString titleFromFilename(const QString& filename);

	// File-watcher suppression — call from the watcher's changed() slot:
	// returns true (and removes the entry) if this path was written by us,
	// so the watcher should skip re-scanning it.
	static bool checkAndClearOwnWrite(const QString& nfoPath);

private:
	static QSet<QString>& ownWrites();
};

} // namespace Mc
