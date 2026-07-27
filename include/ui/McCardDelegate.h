#pragma once
#include "core/DatabaseManager.h"
#include "ui/McFileListModel.h"
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
 * Mode::Library  — shows poster + filename + duration/size + play + track badges + IMDb button.
 *                  Track badges are read-only (no click-to-toggle).
 * Mode::JobQueue — shows poster + filename + status pill + size + play + track badges
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
	// Splits an edition field ("Theatrical & IMAX") into one badge per token
	// instead of one "&"-joined badge — EditionDetector can match multiple
	// tokens in a single filename and packs them into one string; this draws
	// each as its own distinct badge, consistent with how 4K/3D each get
	// their own badge. Returns total width advanced (all badges + gaps),
	// same convention as drawBadge's return value.
	static int drawEditionBadges(QPainter* p, int x, int y, int h,
	                             const QString& editionField, const QFont& font);
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
	void tmdbPageRequested(const QModelIndex& index);
	void streamToggleRequested(const QModelIndex& index, int streamIndex);
	void streamFlagChangeRequested(const QModelIndex& index, int streamIndex,
	                               const QString& flag, bool value);
	// Mega card (Group by Movie) only — a specific edition row's play icon was
	// clicked; fileId identifies which sibling file, unlike playRequested's index
	// (which only ever identified the representative file).
	void groupMemberPlayRequested(const QModelIndex& index, qint64 fileId);

public:
	int  hitTestBadgeStream(const QPoint& pos, const QRect& itemRect,
	                        const QList<StreamRecord>& tracks,
	                        const QFont& baseFont,
	                        bool hasImdb,
	                        bool hasTmdb = false) const;

	// Mega card only: returns the fileId of the specific edition row under pos, or
	// -1 if pos is elsewhere on the card (a card-level click). Used both for the
	// play-icon click above and by McMainWindow's context-menu code to decide
	// row-level vs. card-level actions.
	qint64 hitTestGroupMember(const QPoint& pos, const QRect& itemRect,
	                          const QModelIndex& index) const;

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
		int                 tmdbId         = 0;     // TMDB movie/tv numeric id; 0 = unknown
		double              rating         = 0.0;   // TMDB vote_average; 0 = unknown
		QString             displayTitle;           // TMDB/user override (Library only)
		int                 displayYear    = 0;    // release year from TMDB, 0 = unknown (Library only)
		QString             edition;                // detected/user edition (e.g. "3D"); empty = undetected (Library only)
		QString             mediaType;              // MediaTypes::* (Library only; empty = unknown)
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
		bool                toggleable     = false;  // true for proposed jobs
		qint64              finishedAt     = 0;      // epoch seconds job last left "running"; 0 if never (status pill tooltip)

		// Library "Group by Movie" mode only — see McFileListModel::rebuildGroupedEntries.
		bool            isGroupCard      = false;
		bool            isGroupRedundant = false;
		GroupMemberList groupMembers;
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

	// Invalidates the size cache for the visible (+buffer) rows and asks the view
	// to re-query their sizeHint() — called (debounced) once a viewport resize
	// settles (narrower/wider badge wrapping changes each card's required height)
	// or once scrolling settles (brings rows into view that may hold a size
	// cached at some earlier, no-longer-current width). See m_resizeRelayoutTimer.
	void relayoutVisibleRows();

	static QRect   playButtonRect(const QRect& contentRect);

	// Mega card layout: each GroupMember gets a header row (edition badge + filename
	// + play icon) followed by its own real track-badge rows (video/audio/subtitle),
	// so height varies per member depending on how many tracks it has. Computed once
	// and shared by sizeHint(), paint(), and hit-testing so they can never disagree.
	struct GroupMemberLayout {
		QRect headerRect;     // edition badge + filename + play icon
		int   badgeRows = 0;  // combined video+audio+subtitle row count below headerRect
		int   blockH    = 0;  // headerRect.height() + badge-row area — this member's full row span
	};
	struct GroupCardLayout {
		QList<GroupMemberLayout> members;
		int totalContentH = 0;   // height consumed below the title row, from content.top()+kFolderH+kFolderGap
	};
	// IMDb/TMDB buttons are drawn once for the whole card (it doesn't make sense
	// per-edition) in the shared title row, so they don't factor into this layout —
	// every member gets the full row width for its own track badges.
	static GroupCardLayout layoutGroupCard(const QRect& contentRect,
	                                       const GroupMemberList& members,
	                                       const QFontMetrics& fm);
	// Shared by the normal single-file layout and the mega card (drawn once, using
	// the representative file's ids) — same rects as imdbButtonRect()/tmdbButtonRect().
	void drawImdbTmdbButtons(QPainter* painter, const QRect& content, bool hasImdb, bool hasTmdb) const;
	// Dry-run wrap count for one codec-type group at the given width — mirrors
	// drawBadgeRow()'s own wrap condition exactly, so counts never drift from what
	// actually gets drawn.
	static int     badgeRowCount(const QList<StreamRecord>& group, int areaW,
	                             const QFontMetrics& fm);
	static QRect   groupMemberPlayButtonRect(const QRect& headerRect);
	static QRect   imdbButtonRect(const QRect& contentRect);
	// hasImdb: whether the IMDb button is also present — shifts the TMDB button
	// one slot left so the two never overlap. IMDb always anchors the rightmost slot.
	static QRect   tmdbButtonRect(const QRect& contentRect, bool hasImdb);
	// Total width the badge rows must yield to the right for whichever of the
	// IMDb/TMDB buttons are present (0, 1, or 2), including the gap before the badges.
	static int     rightButtonsReserve(bool hasImdb, bool hasTmdb);
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
	                  int hoveredStreamIndex        = -1,
	                  const QString& flagChangesJson = {}) const;

	bool hitTestInteractive(const QPoint& pos, const QRect& itemRect,
	                        bool hasImdb = false, bool hasTmdb = false,
	                        const QModelIndex& index = {}) const;

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
	// Debounces viewport resize AND scroll settling: a live drag-resize fires many
	// QEvent::Resize ticks, and a scroll can move many rows through the visible
	// range in quick succession — re-querying sizeHint() for every row on each
	// tick would be janky with thousands of cards, so only once things are at
	// rest (dragging stopped, or scrolling stopped) does this fire relayoutVisibleRows().
	QTimer*               m_resizeRelayoutTimer = nullptr;
	// Tracks the viewport width as of the last resize tick we acted on, so
	// eventFilter's QEvent::Resize case only restarts the timer on an actual
	// width change, not every incidental resize tick with the same width.
	int                   m_lastResizeWidth = -1;

	// Cached sizeHint() result, keyed by item id. Deliberately NOT keyed or
	// gated by viewport width — paint() always draws with the live width
	// regardless of what sizeHint() returns, so a briefly-stale cached height
	// (e.g. an off-screen row that hasn't been touched since the last resize)
	// is harmless to keep serving. Invalidation is owned entirely by
	// relayoutVisibleRows() (visible rows + a small buffer, called once resize
	// or scroll settles) — never by an eager whole-cache clear or a per-lookup
	// width check, both of which force every one of potentially thousands of
	// rows into a slow cache-miss recompute the moment Qt's own internal
	// scrollbar-range bookkeeping walks the full list (setUniformItemSizes is
	// false), which is what previously made resizing/restoring stall for
	// several seconds.
	mutable QHash<qint64, QSize> m_sizeCache;
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
	static constexpr int kImdbBtnW  = 20; // width and height of the IMDb/TMDB buttons — matches the play icon's visual size
	// Fixed estimate of the rating text's rendered width ("★ 8.4/10"), reserved in
	// the title row whether or not a given card actually has a rating — lets
	// imdbButtonRect()/tmdbButtonRect() anchor purely from contentRect (needed for
	// hit-testing) while still sitting to the left of wherever rating ends up.
	static constexpr int kRatingReserveW = 46;
	static constexpr int kPosterGap = 8;  // gap between the poster column right edge and the content area
	static constexpr int kMinRowH   = 140; // minimum card height; ensures the poster column never looks cramped
};

} // namespace Mc
