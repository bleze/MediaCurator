#pragma once
#include "core/DatabaseManager.h"
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QPersistentModelIndex>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QStyledItemDelegate>

class QPainter;
class QAbstractItemView;

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

	static constexpr int kPosterW = 110; // poster column width (sized for ~2:3 aspect at typical card height)

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
	                         const QString& flagLang = {});
	static QPixmap badgePixmap(const QString& text, const QString& codecType,
	                           const QFont& baseFont, qreal dpr,
	                           const QString& flagLang = {}, bool removed = false);

	static constexpr int kBadgeH   = 18; // height of each track badge pill
	static constexpr int kBadgePad = 6;  // horizontal text padding inside each badge pill
	static constexpr int kFlagW    = 16; // flag icon width inside a badge (4:3)
	static constexpr int kFlagH    = 12; // flag icon height inside a badge
	static constexpr int kFlagGap  = 4;  // gap between flag icon and badge text

public slots:
	void invalidateSizeCacheFor(qint64 fileId);
	void clearSizeCache();

signals:
	void playRequested(const QModelIndex& index);
	void imdbPageRequested(const QModelIndex& index);
	void streamToggleRequested(const QModelIndex& index, int streamIndex);

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
		QList<StreamRecord> allStreams;
		QSet<int>           removedIndices; // stream indices shown struck-through
		// job queue only
		QString             status;
		int                 progress       = 0;
		qint64              savedBytes     = 0;
		bool                checked        = false;
		bool                toggleable     = false;  // true for proposed jobs
	};

	CardData fetchData(const QModelIndex& index) const;

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
	                  int hoveredStreamIndex        = -1) const;

	int  hitTestBadgeStream(const QPoint& pos, const QRect& itemRect,
	                        const QList<StreamRecord>& tracks,
	                        const QFont& baseFont,
	                        bool hasImdb,
	                        const QString& originalLang = {}) const;

	bool hitTestInteractive(const QPoint& pos, const QRect& itemRect, bool hasImdb = false) const;

	Mode                  m_mode;
	QAbstractItemView*    m_view             = nullptr;
	QPersistentModelIndex m_lastHoveredIndex;
	mutable QPoint        m_lastMousePos     {-1, -1};

	mutable QHash<int, QSize> m_sizeCache;
	mutable int               m_cacheWidth   = 0;
	mutable QFont             m_badgeFont;
	mutable QFontMetrics      m_badgeFm      { QFont{} };

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
	static constexpr int kPosterGap = 0;  // gap between the poster column right edge and the content area
	static constexpr int kMinRowH   = 140; // minimum card height; ensures the poster column never looks cramped
};

} // namespace Mc
