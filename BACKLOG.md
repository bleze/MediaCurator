# MediaCurator — Backlog

Items are tagged: `[P1]` critical path, `[P2]` high value, `[P3]` nice-to-have
Status: `[ ]` todo · `[~]` in progress · `[x]` done · `[-]` deferred

---

## 🚀 SHIP QUEUE (current sprint — Phase 2 core)
> These items unlock the end-to-end Analyze → Review → Execute flow.

- [x] `[P1]` **UserProfile** — model + JSON serialisation to DB prefs table (understood languages, keep-original toggle, preferred codec order)
- [x] `[P1]` **McSettingsDialog** — edit UserProfile: language list, codec preferences, subtitle rules
- [x] `[P1]` **"Analyze Library" action** — runs RuleEngine across all DB files, writes proposed jobs (status=`proposed`) to `jobs` table; triggered from toolbar/menu (Ctrl+Shift+A)
- [x] `[P1]` **McJobPanel** — bottom splitter panel; shows proposed/pending/running/done jobs with filename, summary, status colour-coded, MB/GB saved; checkbox per row; "Queue Selected" / "Queue All" / "Remove" / Start / Pause / Cancel toolbar
- [x] `[P1]` **Space saved per job** — after successful remux, records `original_size_bytes − output_size_bytes` in jobs table; shown per completed row and as running total in panel footer
- [x] `[P1]` **JobQueue runner** — promotes `pending` jobs one at a time via QProcess (async, no extra thread); emits progress back to McJobPanel; user can edit queue while a job runs
- [x] `[P1]` **RemuxJob** — QProcess wrapper around mkvmerge; captures stdout/stderr for progress %; writes result_code + log to DB on finish; atomic rename via std::filesystem
- [x] `[P1]` **Post-job rescan** — on successful job completion, re-scan that file via FfprobeScanner and update DB; refresh card in McFileListModel

---

## Phase 1 — Foundation

- [x] `[P1]` CMakeLists.txt — root + src/CMakeLists with Qt6, nlohmann/json via vcpkg, MSVC config
- [x] `[P1]` DatabaseManager — schema init, CRUD for files/streams/jobs tables
- [x] `[P1]` FfprobeScanner — invoke ffprobe, parse JSON, insert into DB
- [x] `[P1]` McMainWindow skeleton — menu bar, status bar, central widget
- [x] `[P1]` ScanWorker — QThread, scans folder recursively for video files, emits progress
- [x] `[P1]` ExternalTools — locate ffprobe.exe/mkvmerge.exe in tools/ relative to exe
- [x] `[P1]` McTrackTableModel — flat stream+file join model (kept for export/future use)
- [x] `[P1]` McFileListModel + McFileCardDelegate — per-file card view with codec badges (video/audio/subtitle rows with colour-coded pills, "+N" overflow)
- [x] `[P1]` Persistent window geometry (QSettings)
- [x] `[P2]` **Initial window size & layout** — on first launch (before any saved geometry), size window to primary monitor minus 50 px on each edge and centre it; set splitter to 50/50 between library and job queue
- [x] `[P2]` File context menu — right-click card: "Open Containing Folder", "Play in VLC"
- [x] `[P2]` Scan cancel support — cancel button in progress bar aborts ScanWorker cleanly
- [x] `[P2]` Incremental rescan — skip files where mtime + size unchanged since last scan
- [x] `[P2]` Multi-root library support — scan multiple root folders, show all in one view
- [x] `[P2]` Prune stale files on scan — after scan loop, delete DB rows whose path no longer exists under the scanned root
- [x] `[P2]` Remove folder from library — action (menu/toolbar) that bulk-deletes all DB files under a chosen root path
- [-] `[P2]` Basic column sort/filter — deferred; card view replaces flat table; revisit if a list-filter bar is added
- [-] `[P2]` Embedded poster extraction — deferred indefinitely (embedded art is typically low quality; TMDB is the preferred source)
- [ ] `[P3]` Movie title lookup — only relevant when multiple movie files share the same folder (e.g. a flat library), where showing the foldername per-card is redundant/wrong and the real title+year would be more useful. For single-file-per-folder libraries the foldername already serves as the title. Implementation: right-click → "Find on TMDB…" opens ImdbSearchDialog (already written); confirmed match writes a .nfo + poster_cache imdb_id; PosterManager fetches poster; card shows cover art + clean title. Infrastructure already in place: PosterManager, ImdbSearchDialog, NfoParser, poster_cache table. Missing: movie_title column in files table, right-click trigger, poster display in McFileCardDelegate, scan-time .nfo ingestion.
- [ ] `[P3]` File watcher (QFileSystemWatcher) for live library updates

---

## Phase 2 — Rule Engine & Preview

- [x] `[P1]` **UserProfile** — see Ship Queue
- [x] `[P1]` **McSettingsDialog** — see Ship Queue
- [x] `[P1]` **OriginalLanguageDetector** — heuristic: first non-`und` audio track language; updates DB on first analyze
- [~] `[P1]` **CodecHierarchy** — baked into RuleEngine (ac3/eac3/dts→lossless sibling check); standalone class deferred
- [x] `[P1]` **RuleEngine::evaluateFile()** — Tier 1 codec redundancy + Tier 2 language policy; returns TrackDecision per stream
- [x] `[P1]` **"Analyze Library" action** — see Ship Queue
- [x] `[P1]` **McPreviewDialog** — before/after track list for one file; shows decision + reason per track; opened from card right-click or row double-click in job panel
- [x] `[P2]` Filter bar — text search + "only files with proposed removals" / "only files with unknown-language tracks"
- [x] `[P2]` Bulk preview — apply rules across selected files, show aggregate summary (N files · M tracks · X GB)
- [-] `[P3]` Rule conflict detection — deferred; `alwaysKeepOriginalAudio` (default on) + OriginalLanguageDetector already protect the only-track case; codec redundancy can never remove the last sibling; badge visualisation surfaces any edge case before execution
- [x] `[P3]` Per-file override — covered by badge toggle on proposed job cards; click any badge to flip keep/remove and it persists to DB

---

## Phase 3 — Action Engine

- [x] `[P1]` **ActionEngine::buildCommand()** — see Ship Queue
- [x] `[P1]` **RemuxJob** — see Ship Queue
- [x] `[P1]` **JobQueue runner** — see Ship Queue
- [x] `[P1]` **McJobPanel** — see Ship Queue
- [x] `[P1]` **Space saved per job** — see Ship Queue
- [x] `[P1]` **Post-job rescan** — see Ship Queue
- [x] `[P1]` Dry-run mode — "Preview Command" button shows the mkvmerge invocation without executing; shown in a dialog or the log area
- [x] `[P1]` Atomic rename — `<file.path>.tmp` then `std::filesystem::rename` over original only after mkvmerge exits 0
- [ ] `[P2]` Job history log — persist all completed jobs in DB; viewable in a separate History tab
- [x] `[P2]` Space-saved tracker — cumulative GB freed shown in main window status bar (sum across all completed jobs)
- [ ] `[P2]` Undo via trash — move original to OS recycle bin before overwrite (Windows: SHFileOperation / IFileOperation)
- [x] `[P3]` **Status combobox badge delegate** — the job-panel status filter combobox items (proposed / pending / running / done / failed) should render using the same pill style as the card badges: coloured background, rounded rect, matching font size and padding; implement as a `QStyledItemDelegate` on the combobox's view
- [x] `[P2]` **ETA display in job panel** — show estimated time remaining for the current job (based on elapsed time vs progress %) and for the whole queue (sum of per-job estimates using average MB/s from completed jobs); displayed in the McJobPanel footer next to the space-saved total
- [ ] `[P3]` Parallel jobs — configurable worker count (2–4); useful on NAS/RAID where I/O is not the bottleneck
- [ ] `[P3]` Job export — save queue as .bat/.sh mkvmerge script for manual review

---

## Phase 4 — Track Classifier

- [x] `[P1]` `data/track_classifier.json` — regex/keyword rules for commentary, SDH, forced, signs
- [x] `[P1]` **RegexClassifier** — keyword matching on track title; classifies commentary/SDH/forced/signs/main
- [x] `[P1]` Commentary tracks — keep/remove policy based on understood languages + user toggle
- [x] `[P2]` SDH/HI subtitles — 4-mode preference (Keep / Remove / PreferNonSdh / PreferSdh) + subtitle format priority (PGS vs SRT vs ASS etc.) with per-language redundancy removal
- [x] `[P2]` **Cover-art removal** — detect video streams with codec `mjpeg` or `png` (embedded cover/thumbnail art); add toggle in Settings (default ON); RuleEngine marks them for removal; they appear in the job queue badge row with the red strikethrough like any other removed track
- [x] `[P2]` Forced subtitle detection — flag + title heuristic
- [x] `[P2]` Classifier confidence shown as tooltip on badge in card view
- [-] `[P3]` **OnnxClassifier** — deferred; regex classifier sufficient for current needs
- [-] `[P3]` **ApiClassifier** — deferred
- [-] `[P3]` User-trainable classifier — deferred

---

## Phase 5 — Non-MKV & Packaging

- [x] `[P1]` Non-MKV remux flow — mkvmerge from mp4/avi/iso input with track filter; output is new `.mkv` alongside original; original deleted on success; DB path updated; creation timestamp preserved
- [x] `[P1]` **ISO → MKV conversion** — `.iso` scanned via `bluray:` / `dvd://` ffprobe prefix; `iso-bluray` / `iso-dvd` container stored in DB; ActionEngine injects `bluray://` / `dvd://` for mkvmerge; post-job rescan updates DB to new `.mkv` path
- [x] `[P2]` **Filter bar — extended search + quick-filter pills + sort** — text search now covers filename, parent folder, and stream metadata (codec name/profile/HDR format/title); pill toggles for 4K / DV / HDR / Atmos / TrueHD / DTS-HD / DTS:X with Material icons; status filter combo (All / With proposals / Missing poster); sort combo (Name / Newest / Oldest / Largest); AND between pill groups, OR within the audio group; `created_ms` already in DB
- [x] `[P1]` scripts/setup_tools.ps1 — auto-download ffprobe + mkvmerge portable (Windows)
- [ ] `[P1]` scripts/setup_tools.sh — same for Linux/macOS
- [x] `[P2]` CPack NSIS installer (Windows) — bundles Qt DLLs, creates desktop + Start Menu shortcuts, installer/uninstaller icon
- [x] `[P2]` CPack DMG (macOS) — DragNDrop bundle via macdeployqt
- [x] `[P2]` CPack DEB (Linux) — Debian package with Qt6 runtime dependencies
- [x] `[P2]` About dialog — version, license info, Ko-fi / GitHub Sponsors link
- [x] `[P2]` GitHub Actions CI — Windows build + test on push
- [x] `[P3]` macOS/Linux CI jobs — all three platforms build and package on push
- [ ] `[P3]` Automatic update check (compare GitHub release tag)

---

## Future / Icebox

- [ ] `[P2]` **IMDb/TMDB rating display and filter** — when the TMDB dialog fetches movie data, also store the vote_average (rating) and vote_count in the poster_cache table (new columns); show the rating as a small "★ 7.4" badge on file and job cards; add a rating range slider (e.g. 0–10) to McFilterPanel so the user can filter to only show movies with a rating below 5 or above 8; the TMDB search results list in ImdbSearchDialog should also show the rating next to each result to help pick the right entry
- [x] `[P3]` **Dropped vcpkg — nlohmann/json vendored** — removed vcpkg.json, vcpkg toolchain from presets, and vcpkg bootstrap from CI; nlohmann/json 3.11.3 single-header vendored at `third_party/nlohmann/json.hpp`; CI now builds with no external package manager
- [ ] `[P2]` **Preview dialog — richer track info columns** — the Before/After tables currently show Type, Codec, Language, Bitrate, Est. Size; add: a "Flags" column showing "Default", "Forced", "SDH", "Commentary" badges/tags where applicable; show the file's `originalLanguage` either as a column value or as a highlighted row (e.g. subtle background tint or a "★ original" tag) so it's obvious which audio track is protected by the keep-original-audio policy; also consider showing channel count (e.g. "5.1") in the Codec column or a dedicated Channels column
- [x] `[P1]` **Lossless audio track size estimate wildly wrong (e.g. 44 GB for TrueHD)** — replaced "remaining bytes" heuristic with per-codec lookup table (TrueHD/DTS-HD MA ~700 kbps/ch, FLAC ~450 kbps/ch, PCM exact from sampleRate×depth/8); returns -1/shows "—" for unknown codecs; fixed in McCardDelegate.cpp, McPreviewDialog.cpp, McBulkSummaryDialog.cpp
- [x] `[P1]` **Batch "Edit IMDb Link" broken** — file list context menu only had single-file IMDb edit (no batch); added multi-selection collection + dynamic "Edit IMDb Links (N files)…" label + batch loop to McMainWindow's file list context menu; also added `repaintCards()` call at end of job panel batch handler so the job panel refreshes after the loop completes
- [x] `[P1]` **Duplicate jobs in queue** — both `analyzeSingleFile` (McMainWindow) and `AnalyzeWorker::analyzeFile` now call `hasActiveJobForFile(fileId)` and silently skip when a proposed or queued job already exists for the file; added a dedup migration that removes older proposed duplicates on startup + partial unique index `idx_jobs_file_proposed` on `jobs(file_id) WHERE status='proposed'` as a DB-level backstop; also fixed `PosterWorker::processNext` to fall back to the DB-stored `imdb_id` when the NFO file is absent or has no ID (needed when the user pastes an IMDb ID directly into the dialog without a TMDB search result)
- [x] `[P2]` **Job queue toggle in View menu should match toolbar badge style** — replaced plain `addAction` with a `QWidgetAction` hosting a badge-styled `QToolButton` with `setDefaultAction(m_actToggleQueue)`; the button gets the same highlight-colour checked background as the toolbar button, stays in sync automatically
- [ ] `[P2]` **Original language badge in job queue cards** — show the file's `originalLanguage` field as a small labelled badge (similar to the "Default" flag badge) on the audio track row in McJobCardDelegate; add a tooltip explaining why the track is protected ("Original language: eng — kept by alwaysKeepOriginalAudio policy"); makes it easy to spot when the heuristic has set the wrong language before running jobs
- [ ] `[P2]` **Queue running-status multi-card view** — when a job is running, show queued/pending jobs below the running card so the user can see the list being emptied in real time (currently only the active card is visible); requires special handling in McJobPanel/McJobListModel to apply a combined status filter (running + queued) while a job is in progress, then revert to single-status filter when idle
- [x] `[P2]` **Ignore / hide files from library** — right-click on any card → "Ignore this file" action; marks the file in DB (new `ignored` column on `files` table); library view filter hides ignored files by default with a toggle ("Show ignored") to reveal them; use case: Extras/ folders and bonus content the user doesn't want cluttering the view after first review
- [ ] `[P2]` **Duplicate lower-tier audio tracks → commentary heuristic (Option A + C)** — gap found with an old movie: DTS-HD MA 2.0 (default, original) + two DTS 2.0 tracks in the same language; the stereo-as-commentary heuristic requires a surround sibling (none exists on a 2.0-only file), so both DTS tracks fall through to `isRedundantAudio()` and are removed as "higher-quality sibling exists" — but two identical compat cores make no sense, so at least one is almost certainly commentary/alternate mix. **Option A:** extend the commentary pre-pass in `RuleEngine::evaluateFile()` — when ≥2 audio tracks share the same language, format id, and channel count AND a strictly higher-tier track exists in that language, mark the duplicates as heuristic commentary (reason: "Heuristic commentary (duplicate lower-quality tracks)") so the existing commentary keep/remove policy applies; a single lower-tier sibling keeps today's redundant-removal behavior (compatibility core). **Option C (combined):** if the commentary policy would still remove them, downgrade `Decision::Remove` to `Decision::Unsure` for these duplicates so they surface for manual review in the preview dialog instead of being silently dropped.
- [ ] `[P2]` **Wait cursor for long operations** — show `Qt::WaitCursor` during file analysis, batch IMDb fetch, scan, and any synchronous DB operations that may take >200 ms; restore `Qt::ArrowCursor` on completion; prevents the UI from appearing frozen during batch workflows
- [ ] `[P2]` **Parallel TMDB enrichment** — PosterWorker currently processes one file at a time with a synchronous QEventLoop HTTP call (~200–400 ms per round-trip), so 3 000 files takes ~20–30 min to enrich. TMDB's rate limit is ~40–50 req/s per IP; each file costs 1 req (IMDb ID known) or 2 req (title search). Approach: replace the single-timer loop with a fixed pool of N concurrent workers (suggest N = 8, well under the rate limit). Each worker is a `QObject` living on its own `QThread` with its own `QNetworkAccessManager`; the shared queue is protected by a `QMutex`. Alternatively, keep the single thread but switch from QEventLoop-blocking calls to chained `QNetworkReply::finished` lambdas so N requests are in-flight simultaneously without extra threads. Either approach should reduce enrichment time for 3 000 files from ~25 min to ~3–4 min.
- [ ] `[P3]` HDR metadata display (MaxCLL, MaxFALL, mastering display)
- [ ] `[P3]` Duplicate file detection (same title, different quality)
- [ ] `[P3]` Chapter editor (view/remove chapters)
- [ ] `[P3]` Attachment management (embedded fonts, cover art)
- [ ] `[P3]` Export library to CSV/JSON for external scripting
- [x] `[P3]` Dark mode / theme support — Qt 6 picks up the Windows system dark palette; UI and icons adapt automatically
- [ ] `[P3]` Keyboard shortcuts for power users
- [x] `[P2]` **Subtitle download** — right-click "Download Subtitles…" on any file; searches OpenSubtitles.com by IMDb ID; shows per-language coverage; parallel download threads; saves `.srt` sidecar files with Kodi/Plex-compatible naming (`Movie.Title.2001.en.srt`); card refreshes automatically after download; available from both library and job queue context menus; requires free OpenSubtitles API key in Settings
- [ ] `[P2]` **Poster selection** — instead of auto-using the first TMDB poster, let the user browse all available posters (grid of thumbnails from TMDB `/movie/{id}/images`); click to preview full size; "Use This" saves it to the poster cache and refreshes the card; accessible from the existing ImdbSearchDialog after confirming a match, or via right-click → "Change Poster…" on any card that already has a link
- [ ] `[P2]` **Fanart / backdrop selection** — fetch backdrop images from TMDB for the matched movie; let the user pick one and save it as `<MovieFolder>/fanart.jpg` (Kodi) or `<MovieFolder>/<title>-fanart.jpg` (Plex); shown in a similar thumbnail-grid picker; adds `fanart_path` column to `poster_cache`; long term: display as a subtle blurred background in the file card
- [x] `[P3]` **"What if" simulation** — `McWhatIfDialog` + `SimulateWorker`; runs RuleEngine across entire library in-memory (no jobs written); shows files affected / tracks removed / ~GB saved with breakdown by removal reason and language; "Analyze Library →" button promotes straight to analysis

---

## Bug Tracker

> Add bugs here as found during development.

- [ ] **Pause/Resume button shows wrong state** — the button's text/icon is only mutated inside the `onPause()` / `onCancel()` click handlers (McJobPanel.cpp ~928-948) and never derived from the queue's actual state, so it desyncs: e.g. pressing Start (or `m_queue->start()` from elsewhere) while paused leaves it saying "Resume" while running; queue finishing while paused leaves "Resume" stuck; the enable rule `running > 0 && queued > 0` (~line 1104) also disables it mid-pause. Fix: make a single `syncPauseButton()` that derives text/icon/enabled from `m_queue->isPaused()` + running/queued counts, called from refresh/updateFooter and the queue's jobStarted/allFinished signals, instead of setting state imperatively in click handlers.
- [x] **Delete key not handled in job queue** — pressing Delete with items selected in McJobPanel should trigger the Remove action (same as the toolbar Remove button); currently does nothing

---

## Done ✅

- [x] CMakeLists.txt — root + src/CMakeLists, Qt6 6.8.3 LTS, nlohmann/json via vcpkg, MSVC/VS2026 config, CMakePresets.json + CMakeUserPresets.json
- [x] DatabaseManager — schema init, CRUD for files/streams/jobs/scan_runs/prefs tables; `jobStatusChanged` signal; `allJobsForPanel()` JOIN query
- [x] FfprobeScanner — ffprobe subprocess, JSON parse, DB upsert
- [x] McMainWindow — splitter layout (file list top / job panel bottom); Scan Folder + Analyze Library + Settings menu; status bar
- [x] ScanWorker — QThread, recursive video file scan, progress signals
- [x] ExternalTools — locate ffprobe/mkvmerge relative to exe
- [x] UserProfile — understood languages, keep-original-audio, subtitle preferences; JSON to DB prefs
- [x] McSettingsDialog — edit UserProfile; 30 ISO 639-2 language presets + free-type combo; Fusion palette
- [x] McTrackTableModel — flat stream+file join model (kept for future export/filter use)
- [x] McFileListModel + McFileCardDelegate — per-file card view; codec badge rows; colour-coded pills; "+N" overflow; alternating rows
- [x] Persistent window geometry + state (QSettings)
- [x] OriginalLanguageDetector — heuristic from first non-`und` audio track
- [x] RuleEngine — Tier 1 codec hierarchy (DTS-HD MA / TrueHD beats lossy siblings); Tier 2 language policy
- [x] ActionEngine — builds mkvmerge --audio-tracks / --subtitle-tracks args from FileDecision
- [x] RemuxJob — async QProcess; progress parsing; atomic rename; savedBytes measurement
- [x] JobQueue — serial async queue; start/pause/cancel; DB status updates; savedBytes persisted
- [x] McJobPanel — job table with checkbox, summary, colour-coded status, MB/GB saved; footer total; Queue Selected / Queue All / Remove / Start / Pause / Cancel
- [x] RegexClassifier — keyword matching: commentary / SDH / forced / signs / main
