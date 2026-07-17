#pragma once
#include "core/DatabaseManager.h"
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QMap>
#include <QPersistentModelIndex>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QStyledItemDelegate>

class QPainter;
class QAbstractItemView;
class QTimer;

namespace Mc {

/**
 * McCardDelegate — unified card painter for both the library list and the job queue.
 *
 * Mode::Library  — shows poster + filename + duration/size + VLC play + track badges + IMDb button.
 *                  Track badges are read-only (no click-to-toggle).
 * Mode::JobQueue — shows poster + filename + status pill + size + VLC play + track badges
 *                  (kept normal, removed struck-through) + IMDb button + checkbox + progress bar.
 *                  Proposed-job audio/subtitle badges are click-to-toggle.
 *
 * Use the thin subclass wrappers McFileCardDelegate / McJobCardDelegate so existing
 * code that creates them by name continues to work unchanged.
 */
class McCardDelegate : public QStyledItemDelegate
{
	Q_OBJECT
public:
	enum class Mode { Library, JobQueue };

	explicit McCardDelegate(Mode mode, QObject* parent = nullptr);

	void  paint(QPainter* painter, const QStyleOptionViewItem& option,
	            const QModelIndex& index) const override;
	QSize sizeHint(const QStyleOptionViewItem& option,
	               const QModelIndex& index) const override;
	bool  editorEvent(QEvent* event, QAbstractItemModel* model,
	                  const QStyleOptionViewItem& option,
	                  const QModelIndex& index) override;
	bool  helpEvent(QHelpEvent* event, QAbstractItemView* view,
	                const QStyleOptionViewItem& option,
	                const QModelIndex& index) override;
	bool  eventFilter(QObject* obj, QEvent* event) override;

	bool  handlePress(const QPoint& viewportPos, const QRect& itemRect,
	                  const QFont& viewFont, const QModelIndex& index);

	static constexpr int kPosterW = 100; // poster column width

	// Badge rendering shared with McPreviewDialog's track-badge column.
	// flagLang: language code whose country flag is drawn before the text;
	// pass empty (or an unmapped code) to render a text-only badge.
	static QString buildBadgeText(const StreamRecord& s, bool isOriginal = false);
	static QColor  badgeColor(const QString& codecType);
	static int     badgeWidthFor(const StreamRecord& s, bool isOriginal, const QFontMetrics& fm);
	static int     drawBadge(QPainter* p, int x, int y, int h,
	                         const QString& text, const QColor& bg, const QFont& font,
	                         bool removed  = false,
	                         bool hasTip   = false, const QColor& cardBg = {},
	                         bool hovered  = false,
	                         const QString& flagLang = {},
	                         const QMap<QString, bool>& streamFlags = {},
	                         bool isSDH    = false);
	static QPixmap renderSvgIcon(const QString& resourcePath, const QColor& color,
	                             int size, qreal dpr);
	static QPixmap badgePixmap(const QString& text, const QString& codecType,
	                           const QFont& baseFont, qreal dpr,
	                           const QString& flagLang = {}, bool removed = false);

	// Storage-group chip (colored disk icon + group number) — shared between
	// the card badge and McManageFoldersDialog's group picker so both render
	// identically. opacity < 1.0 darkens background, icon, and number all
	// toward black together (staying fully opaque, not alpha-blended against
	// the backdrop) — used for the "unselected" chips in the picker, so the
	// off state reads as uniformly darker rather than washed out.
	static int  groupChipWidth(int group, const QFont& baseFont);
	static void drawGroupChip(QPainter* painter, int x, int y, int h, int group,
	                         const QFont& baseFont, qreal dpr, double opacity = 1.0);

	static constexpr int kBadgeH   = 18; // height of each track badge pill
	static constexpr int kBadgePad = 6;  // horizontal text padding inside each badge pill
	static constexpr int kFlagW    = 16; // flag icon width inside a badge (4:3)
	static constexpr int kFlagH    = 12; // flag icon height inside a badge
	static constexpr int kFlagGap  = 4;  // gap between flag icon and badge text

	// Pre-populate the raw fanart cache from a freshly-downloaded QPixmap.
	// Called from model slots after a new fanart arrives so paint() never hits disk.
	static void prefetchFanart(const QString& path, QPixmap raw);

	// Load poster/fanart from disk for every card currently intersecting the viewport.
	// Called on library load and scroll so the first paint already has artwork cached.
	void prefetchVisibleArtwork() const;

	// Coalesces bursts of prefetch requests (per-file poster/fanart-ready signals,
	// per-page load batches) into a single prefetchVisibleArtwork() call via the
	// existing debounce timer, instead of each caller invoking it directly.
	void scheduleArtworkPrefetch();

	// Width actually reserved on-screen for the poster/checkbox column right now —
	// depends on whether TMDB is configured (see setTmdbConfigured). Callers outside
	// the delegate that hit-test the poster column (e.g. double-click-to-open-IMDb-
	// search) must use this instead of the fixed kPosterW constant.
	int posterColumnWidth() const;

public slots:
	void invalidateSizeCacheFor(qint64 fileId);
	void clearSizeCache();

	// Whether TMDB is configured (non-empty API key). When false, no posters will
	// ever arrive, so the poster column is collapsed entirely instead of leaving a
	// permanently-empty indent. When true but an individual card has no poster yet
	// (still fetching, or no match), a placeholder box is drawn in its place so
	// cards stay aligned with the rest of the list.
	void setTmdbConfigured(bool configured);

	// Whether the per-card storage-group disk-icon chip should render at all.
	// Gated on StorageGroupSettings::multipleGroupsInUse() — single-storage-group
	// users see no new UI. Pushed once by McMainWindow whenever roots/groups
	// change (folder add/remove, Manage Folders dialog close), never recomputed
	// inside paint().
	void setMultiGroupBadgeEnabled(bool enabled);

	// Opacity (0.0-1.0) of the fanart backdrop drawn behind each card. Settings-
	// dialog-tunable; see AppSettings key "library/fanartOpacity".
	void setFanartOpacity(double opacity);

signals:
	void playRequested(const QModelIndex& index);
	void imdbPageRequested(const QModelIndex& index);
	void streamToggleRequested(const QModelIndex& index, int streamIndex);
	void streamFlagChangeRequested(const QModelIndex& index, int streamIndex,
	                               const QString& flag, bool value);

public:
	int  hitTestBadgeStream(const QPoint& pos, const QRect& itemRect,
	                        const QList<StreamRecord>& tracks,
	                        const QFont& baseFont,
	                        bool hasImdb,
	                        const QString& originalLang = {}) const;

private:
	// Normalised card data populated from whichever model is in use.
	struct CardData {
		QString             filename;
		QString             filePath;
		qint64              sizeBytes      = 0;
		double              durationSec    = 0.0;
		QString             posterPath;
		int                 posterVersion  = 0;
		QString             imdbId;
		double              rating         = 0.0;   // TMDB vote_average; 0 = unknown
		QString             displayTitle;           // TMDB/user override (Library only)
		int                 displayYear    = 0;    // release year from TMDB, 0 = unknown (Library only)
		QString             containerTitle;         // ffprobe format tags title (Library only)
		int                 folderCount    = 1;     // files sharing the same parent folder (Library only)
		QString             originalLanguage;       // ISO 639-2 original audio language (both modes)
		int                 storageGroup   = 1;     // 1-4; see StorageGroupSettings (both modes)
		QString             fanartPath;             // w780 backdrop; empty if not yet fetched
		QList<StreamRecord> allStreams;
		QList<StreamRecord> videoStreams;
		QList<StreamRecord> audioStreams;
		QList<StreamRecord> subtitleStreams;
		QSet<int>           removedIndices; // stream indices shown struck-through
		QString             flagChangesJson; // job queue only; empty in library mode
		// job queue only
		qint64              jobId          = 0;
		QString             status;
		int                 progress       = 0;
		qint64              savedBytes     = 0;
		qint64              outputSizeBytes = 0;  // live .tmp size while running
		QString             phaseLabel;             // sub-phase label (e.g. "Copying to NAS"), empty = default "Running"
		bool                checked        = false;
		bool                toggleable     = false;  // true for proposed jobs
	};

	// Animation state for the live size bar, keyed by jobId. Two independent bars, both
	// measured as a fraction of the *same* full width (the original file size) rather
	// than one nested inside the other — red is mkvmerge's raw progress percent, blue
	// is output-bytes-written-so-far / original-size. Keeping them independent avoids
	// compounding two separately-smoothed quantities into visible desync artifacts.
	// Each is tweened from a fixed start time/value toward its latest sample, so the
	// displayed position is correct for the current wall-clock time no matter how
	// often (or unevenly) paint() gets called.
	struct TweenState {
		double startVal = 0.0, endVal = 0.0;
		qint64 segmentStartMs = 0;
	};
	struct SizeBarAnim {
		TweenState red;
		TweenState blue;
	};

	CardData fetchData(const QModelIndex& index) const;

	// Invalidates the size cache and asks the view to re-query sizeHint() for every
	// row — called (debounced) once a viewport resize settles, since narrower/wider
	// badge wrapping changes each card's required height. See m_resizeRelayoutTimer.
	void relayoutForResize();

	static QRect   playButtonRect(const QRect& contentRect);
	static QRect   imdbButtonRect(const QRect& contentRect);
	static QString formatDuration(double sec);
	static QString formatSize(qint64 bytes);
	static QString codecLabel(const StreamRecord& s);
	static QString channelStr(int channels);
	static QColor  statusColor(const QString& status);
	static QString statusLabel(const QString& status);

	int  drawBadgeRow(QPainter* p, QRect rowRect,
	                  const QList<StreamRecord>& tracks,
	                  const QSet<int>& removedIndices,
	                  const QFont& badgeFont,
	                  const QColor& cardBg,
	                  const QString& originalLang  = {},
	                  int hoveredStreamIndex        = -1,
	                  const QString& flagChangesJson = {}) const;

	bool hitTestInteractive(const QPoint& pos, const QRect& itemRect, bool hasImdb = false) const;

	// Left inset of the content area from the card's left edge — kPosterW + kPosterGap
	// when TMDB is configured, otherwise just enough for a checkbox column (job queue)
	// or nothing at all (library), so cards don't sit indented for a poster that will
	// never arrive. See setTmdbConfigured.
	int leftContentInset() const;

	Mode                  m_mode;
	bool                  m_tmdbConfigured   = true;
	bool                  m_showGroupBadge   = false;
	double                m_fanartOpacity    = 0.05;
	QAbstractItemView*    m_view             = nullptr;
	QPersistentModelIndex m_lastHoveredIndex;
	mutable QPoint        m_lastMousePos     {-1, -1};

	// Fires at a fixed cadence and repaints only the rows with a job actually running
	// (via QAbstractItemView::update(index), not a full viewport update) — the pulse
	// color and size-bar position are both computed from wall-clock time, so this timer
	// only needs to trigger a redraw, not advance any state itself.
	QTimer*               m_animTimer        = nullptr;
	QTimer*               m_artworkPrefetchTimer = nullptr;
	// Debounces viewport resize: a live drag-resize fires many QEvent::Resize
	// ticks, and re-querying sizeHint() for every row on each one would be janky
	// with thousands of cards — only the width at rest, once dragging settles,
	// triggers the size-cache invalidation + relayout.
	QTimer*               m_resizeRelayoutTimer = nullptr;
	// Tracks the viewport width as of the last resize tick we acted on — kept
	// separate from m_cacheWidth, which sizeHint() also mutates independently
	// (e.g. from incidental hover/scrollbar-driven queries mid-drag), so this
	// decision doesn't get fooled by a coincidental match against that shared value.
	int                   m_lastResizeWidth = -1;

	// Cached sizeHint() result plus the viewport width it was computed for. The
	// width is checked on every lookup (see sizeHint()) so an off-screen row that
	// cached a height for some transient mid-drag width — and was never touched
	// by relayoutForResize() because it wasn't visible when a resize settled —
	// self-corrects the next time it's actually queried at the current width,
	// instead of keeping a stale (sometimes wildly oversized) height forever.
	// This is a per-entry check, not an eager whole-cache clear, so it doesn't
	// reintroduce the live-drag sluggishness relayoutForResize()'s design note
	// below describes.
	struct CachedSize { int width; QSize size; };
	mutable QHash<qint64, CachedSize> m_sizeCache;
	// Width sizeHint() last computed a fresh entry with — informational only
	// (e.g. for debugging); no longer gates cache invalidation, since eagerly
	// clearing the whole cache on every width tick during a live drag is what
	// made resizing sluggish. relayoutForResize() owns invalidation instead.
	mutable int               m_cacheWidth   = 0;
	mutable QFont             m_badgeFont;
	mutable QFontMetrics      m_badgeFm      { QFont{} };

	mutable QHash<qint64, SizeBarAnim> m_sizeBarAnim;

	static constexpr int kPadH      = 10; // horizontal inset from card edge to poster and content
	static constexpr int kPadV      = 4;  // vertical padding above the folder row
	static constexpr int kPadBottom = 7;  // vertical padding below the last badge row
	static constexpr int kFolderH   = 20; // height of the title/meta row (movie title, duration, size, rating)
	static constexpr int kFolderGap = 0;  // explicit gap between the title row and the filename row
	static constexpr int kHeaderH   = 24; // height of the filename row (also the play-button size)
	static constexpr int kSepGap    = 3;  // gap between the filename row and the first badge row; set to kFolderGap+(kFolderH-12)/2 for equal visual gaps
	static constexpr int kRowGap    = 4;  // vertical gap between badge rows
	static constexpr int kBadgeGap  = 4;  // horizontal gap between adjacent badges within a row
	static constexpr int kPlayBtnW  = 24; // width and height of the play (▶) button on the right
	static constexpr int kImdbBtnW  = 24; // width and height of the IMDb shortcut button on the right
	static constexpr int kPosterGap = 8;  // gap between the poster column right edge and the content area
	static constexpr int kMinRowH   = 140; // minimum card height; ensures the poster column never looks cramped
	static constexpr int kCheckboxColW = 24; // poster-column width when TMDB isn't configured but a per-row checkbox (job queue) still needs room
};

} // namespace Mc
