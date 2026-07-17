#pragma once
#include <QSet>
#include <QString>

namespace Mc {

// Optional TMDB-sourced metadata for writeMovieNfo(), beyond the always-required
// IMDb id. Every field defaults to "unknown" (0 / empty) — a caller that only
// has the id gets the original id-only behavior.
struct NfoMovieMeta {
	int     tmdbId        = 0;       // <uniqueid type="tmdb">
	QString title;                  // <title> — localized per caller's language choice
	QString originalTitle;          // <originaltitle> — TMDB's original_title, always
	int     year           = 0;      // <year>
	QString premiered;              // <premiered> — full release date, YYYY-MM-DD
	double  voteAverage    = 0.0;    // <ratings><rating name="themoviedb">...
	int     voteCount      = 0;
};

class NfoParser {
public:
	// Canonical NFO path for a video file: same dir, same base name, .nfo extension
	static QString nfoPathFor(const QString& videoPath);

	// Read the first IMDb ID found in the NFO for this video (empty if none)
	static QString readImdbId(const QString& videoPath);

	// Write (or update) the NFO file next to videoPath. Three cases:
	//   - Kodi-style <movie> XML already exists: only the <uniqueid type="imdb">,
	//     <uniqueid type="tmdb"> (if meta.tmdbId > 0), <id>, <title>,
	//     <originaltitle>, <year>, <premiered> and <ratings> elements are
	//     touched — one per non-empty/non-zero field in meta (any legacy
	//     <imdbid> tag is dropped); all other content is preserved.
	//   - Non-XML NFO already exists (e.g. a scene-release NFO with an IMDb
	//     id/URL embedded in free text): the id text is corrected in place —
	//     the file is never truncated/replaced. If no id-shaped token is found
	//     at all, an IMDb URL is appended rather than touching existing content.
	//     meta is ignored for this case.
	//   - No NFO exists: a minimal one is created with the id tags plus
	//     whatever meta fields the caller has. Pass a default-constructed
	//     NfoMovieMeta to fall back to the original id-only behavior, letting
	//     Kodi's own scraper fill the rest in.
	// Also registers nfoPath in the own-write suppression set so a future
	// QFileSystemWatcher callback can skip it via checkAndClearOwnWrite().
	static bool writeMovieNfo(const QString& videoPath, const QString& imdbId,
	                          const NfoMovieMeta& meta = {});

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
