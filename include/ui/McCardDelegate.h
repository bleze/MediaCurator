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

	static constexpr int kPosterW = 80;

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
	static QString buildBadgeText(const StreamRecord& s);
	static QColor  badgeColor(const QString& codecType);
	static QColor  statusColor(const QString& status);
	static QString statusLabel(const QString& status);

	static int  drawBadge(QPainter* p, int x, int y, int h,
	                      const QString& text, const QColor& bg, const QFont& font,
	                      bool removed  = false,
	                      bool hasTip   = false, const QColor& cardBg = {},
	                      bool hovered  = false);

	int  drawBadgeRow(QPainter* p, QRect rowRect,
	                  const QList<StreamRecord>& tracks,
	                  const QSet<int>& removedIndices,
	                  const QFont& badgeFont,
	                  const QColor& cardBg,
	                  int hoveredStreamIndex = -1) const;

	int  hitTestBadgeStream(const QPoint& pos, const QRect& itemRect,
	                        const QList<StreamRecord>& tracks,
	                        const QFont& baseFont,
	                        bool hasImdb) const;

	bool hitTestInteractive(const QPoint& pos, const QRect& itemRect, bool hasImdb = false) const;

	Mode                  m_mode;
	QAbstractItemView*    m_view             = nullptr;
	QPersistentModelIndex m_lastHoveredIndex;
	mutable QPoint        m_lastMousePos     {-1, -1};

	mutable QHash<int, QSize> m_sizeCache;
	mutable int               m_cacheWidth   = 0;
	mutable QFont             m_badgeFont;
	mutable QFontMetrics      m_badgeFm      { QFont{} };

	static constexpr int kPadH      = 10;
	static constexpr int kPadV      = 6;
	static constexpr int kPadBottom = 8;
	static constexpr int kFolderH   = 13;
	static constexpr int kFolderGap = 1;
	static constexpr int kHeaderH   = 22;
	static constexpr int kSepGap    = 4;
	static constexpr int kBadgeH    = 18;
	static constexpr int kRowGap    = 4;
	static constexpr int kBadgeGap  = 4;
	static constexpr int kBadgePad  = 6;
	static constexpr int kPlayBtnW  = 24;
	static constexpr int kImdbBtnW  = 24;
	static constexpr int kPosterGap = 8;
};

} // namespace Mc
