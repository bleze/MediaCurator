#include "ui/McCardDelegate.h"
#include "ui/McFileListModel.h"
#include "ui/McJobListModel.h"
#include "ui/McLanguageFlags.h"
#include "classifier/RegexClassifier.h"
#include "core/ExternalTools.h"
#include "engine/TrackDecision.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFile>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QPixmap>
#include <QPixmapCache>
#include <QStyle>
#include <QStyleOption>
#include <QStyleOptionButton>
#include <QToolTip>
#include <QVariantAnimation>
#include <QtMath>
#include <algorithm>

namespace Mc {

// Raw fanart pixmaps (full-res, unscaled).  Never evicted — populated on download
// so paint() never decodes a JPEG from disk during active downloads.
static QHash<QString, QPixmap> s_fanartRawCache;

// Version counter per path: incremented each time a new raw pixmap is stored so
// that the derived scaled entries in QPixmapCache (keyed by path+version+width) are
// automatically invalidated without needing a wildcard-remove API.
static QHash<QString, int> s_fanartVersions;

// Loads an SVG resource, injects a fill color into the root element (same
// logic as SvgIcon.cpp's recolorSvg), and renders it to a cached QPixmap.
// Used by drawBadge for the SDH closed-caption icon.
QPixmap McCardDelegate::renderSvgIcon(const QString& resourcePath,
                                       const QColor& color, int targetH, qreal dpr)
{
	const QString key = QStringLiteral("svgicon_%1_%2_%3_%4")
	                    .arg(resourcePath, color.name())
	                    .arg(targetH).arg(qRound(dpr * 100));
	QPixmap cached;
	if (QPixmapCache::find(key, &cached)) return cached;

	QFile f(resourcePath);
	if (!f.open(QIODevice::ReadOnly)) return {};
	QString svg = QString::fromUtf8(f.readAll());

	const int svgStart = svg.indexOf(QLatin1String("<svg"));
	const int tagEnd   = svg.indexOf(QLatin1Char('>'), svgStart);
	if (svgStart >= 0 && tagEnd >= 0) {
		QString tag = svg.mid(svgStart, tagEnd - svgStart + 1);
		static const QRegularExpression fillAttr(R"(\s+fill="[^"]*")");
		tag.remove(fillAttr);
		tag.insert(4, QStringLiteral(R"( fill="%1")").arg(color.name()));
		svg = svg.left(svgStart) + tag + svg.mid(tagEnd + 1);
	}

	const int pxSize = qCeil(targetH * dpr);
	QPixmap pm(pxSize, pxSize);
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter ep(&pm);
	QSvgRenderer(svg.toUtf8()).render(&ep, QRectF(0, 0, targetH, targetH));
	QPixmapCache::insert(key, pm);
	return pm;
}

void McCardDelegate::prefetchFanart(const QString& path, QPixmap raw)
{
	s_fanartVersions[path]++;          // bump → old QPixmapCache entries become unreachable
	s_fanartRawCache.insert(path, std::move(raw));
}

McCardDelegate::McCardDelegate(Mode mode, QObject* parent)
	: QStyledItemDelegate(parent), m_mode(mode)
{
	if (auto* view = qobject_cast<QAbstractItemView*>(parent)) {
		m_view = view;
		view->viewport()->installEventFilter(this);
	}
	if (m_mode == Mode::JobQueue) {
		m_pulseAnim = new QVariantAnimation(this);
		m_pulseAnim->setDuration(2000);
		m_pulseAnim->setLoopCount(-1);
		m_pulseAnim->setEasingCurve(QEasingCurve::InOutSine);
		m_pulseAnim->setKeyValueAt(0.0, QColor(0x64, 0xb4, 0xf0)); // light blue
		m_pulseAnim->setKeyValueAt(0.5, QColor(0x10, 0x50, 0xb0)); // dark blue
		m_pulseAnim->setKeyValueAt(1.0, QColor(0x64, 0xb4, 0xf0)); // light blue
		connect(m_pulseAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant&) {
			if (m_view) m_view->viewport()->update();
		});
		m_pulseAnim->start();
	}
}

// ── Data fetching ──────────────────────────────────────────────────────────────

McCardDelegate::CardData McCardDelegate::fetchData(const QModelIndex& index) const
{
	CardData d;
	if (m_mode == Mode::Library) {
		const auto file    = index.data(McFileListModel::FileRole).value<FileRecord>();
		d.filename         = file.filename;
		d.filePath         = file.path;
		d.sizeBytes        = file.sizeBytes;
		d.durationSec      = file.durationSec;
		d.posterPath       = index.data(McFileListModel::PosterRole).toString();
		d.posterVersion    = index.data(McFileListModel::PosterVersionRole).toInt();
		d.imdbId           = index.data(McFileListModel::ImdbRole).toString();
		d.rating           = index.data(McFileListModel::RatingRole).toDouble();
		d.displayTitle      = index.data(McFileListModel::DisplayTitleRole).toString();
		d.displayYear       = index.data(McFileListModel::DisplayYearRole).toInt();
		d.containerTitle    = index.data(McFileListModel::ContainerTitleRole).toString();
		d.folderCount       = index.data(McFileListModel::FolderCountRole).toInt();
		d.originalLanguage  = file.originalLanguage;
		d.fanartPath        = index.data(McFileListModel::FanartRole).toString();
		d.allStreams         = index.data(McFileListModel::StreamsRole).value<QList<StreamRecord>>();
		d.removedIndices    = index.data(McFileListModel::OverridesRole).value<QSet<int>>();
	} else {
		d.filename         = index.data(McJobListModel::FilenameRole).toString();
		d.filePath         = index.data(McJobListModel::FilePathRole).toString();
		d.sizeBytes        = index.data(McJobListModel::FileSizeRole).toLongLong();
		d.posterPath       = index.data(McJobListModel::PosterRole).toString();
		d.imdbId           = index.data(McJobListModel::ImdbIdRole).toString();
		d.rating           = index.data(McJobListModel::RatingRole).toDouble();
		d.status           = index.data(McJobListModel::StatusRole).toString();
		d.savedBytes       = index.data(McJobListModel::SavedRole).toLongLong();
		d.durationSec      = index.data(McJobListModel::DurationRole).toDouble();
		d.progress         = index.data(McJobListModel::ProgressRole).toInt();
		d.checked           = (index.data(Qt::CheckStateRole).toInt() == Qt::Checked);
		d.toggleable        = (d.status == QLatin1String("proposed") || d.status == QLatin1String("queued") || d.status == QLatin1String("failed"));
		d.originalLanguage  = index.data(McJobListModel::OriginalLanguageRole).toString();
		d.fanartPath        = index.data(McJobListModel::FanartRole).toString();
		d.allStreams         = index.data(McJobListModel::AllStreamsRole).value<QList<StreamRecord>>();
		const auto kept    = index.data(McJobListModel::KeptStreamsRole).value<QList<StreamRecord>>();
		QSet<int> keptIdx;
		for (const auto& s : kept) keptIdx.insert(s.streamIndex);
		for (const auto& s : d.allStreams)
			if (!keptIdx.contains(s.streamIndex)) d.removedIndices.insert(s.streamIndex);
		d.flagChangesJson   = index.data(McJobListModel::FlagChangesRole).toString();
	}
	return d;
}

// ── Static helpers ─────────────────────────────────────────────────────────────

QString McCardDelegate::formatDuration(double sec)
{
	if (sec <= 0) return {};
	const int t = static_cast<int>(sec);
	const int h = t / 3600, m = (t % 3600) / 60, s = t % 60;
	return h > 0
		? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'))
		: QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

QString McCardDelegate::formatSize(qint64 bytes)
{
	const double gb = bytes / 1073741824.0;
	if (gb >= 1.0) return QStringLiteral("%1 GB").arg(gb, 0, 'f', 2);
	return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
}

QString McCardDelegate::codecLabel(const StreamRecord& s)
{
	const QString n = s.codecName.toLower();
	if (n == "h264")                    return "H.264";
	if (n == "hevc")                    return "H.265";
	if (n == "av1")                     return "AV1";
	if (n == "vp9")                     return "VP9";
	if (n == "vp8")                     return "VP8";
	if (n == "mpeg4")                   return "MPEG-4";
	if (n == "mpeg2video")              return "MPEG-2";
	if (n == "dts") {
		const QString p = s.codecProfile.toLower();
		const QString t = s.title.toLower();
		if (p.contains("dts:x") || p.contains(":x") || t.contains("dts:x") || t.contains("dts-x"))
			return "DTS:X";
		if (p.contains("ma") || t.contains("master audio") || t.contains("dts-hd ma"))
			return "DTS-HD MA";
		if (p.contains("hra") || t.contains("hra"))
			return "DTS-HD HRA";
		return "DTS";
	}
	if (n == "truehd") {
		const QString p = s.codecProfile.toLower();
		const QString t = s.title.toLower();
		return (p.contains("atmos") || t.contains("atmos")) ? "Atmos" : "TrueHD";
	}
	if (n == "eac3")                    return "DD+";
	if (n == "ac3")                     return "DD";
	if (n == "aac")                     return "AAC";
	if (n == "mp3")                     return "MP3";
	if (n == "flac")                    return "FLAC";
	if (n == "opus")                    return "Opus";
	if (n == "vorbis")                  return "Vorbis";
	if (n.startsWith("pcm_"))           return "PCM";
	if (n == "subrip" || n == "srt")    return "SRT";
	if (n == "ass"    || n == "ssa")    return "ASS";
	if (n == "hdmv_pgs_subtitle")       return "PGS";
	if (n == "dvd_subtitle")            return "VobSub";
	if (n == "webvtt")                  return "VTT";
	return s.codecName.toUpper();
}

QString McCardDelegate::channelStr(int ch)
{
	switch (ch) {
	case 1: return "1.0";
	case 2: return "2.0";
	case 6: return "5.1";
	case 7: return "6.1";
	case 8: return "7.1";
	default: return ch > 0 ? QString::number(ch) + "ch" : QString();
	}
}

static bool isCommentaryTrack(const StreamRecord& s)
{
	if (s.isCommentary || s.trackType == QLatin1String("commentary"))
		return true;
	return s.title.contains(QLatin1String("comment"),  Qt::CaseInsensitive)
	    || s.title.contains(QLatin1String("komment"),  Qt::CaseInsensitive)
	    || s.title.contains(QLatin1String("director"), Qt::CaseInsensitive);
}

// Matches RegexClassifier's SDH detection: hearing_impaired disposition flag OR
// title keywords ("sdh", "hearing", "cc") — same logic used by RuleEngine.
static bool streamIsSDH(const StreamRecord& s)
{
	if (s.codecType != QLatin1String("subtitle")) return false;
	if (s.isHearingImpaired) return true;
	const QString lt = s.title.toLower();
	return lt.contains(QLatin1String("sdh"))
	    || lt.contains(QLatin1String("hearing"))
	    || lt.contains(QLatin1String("cc"));
}

QString McCardDelegate::buildBadgeText(const StreamRecord& s, bool isOriginal)
{
	QString t = codecLabel(s);
	if (s.codecType == "video") {
		if (s.width > 0)
			t += QStringLiteral("  %1\xC3\x97%2").arg(s.width).arg(s.height);
		if (!s.hdrFormat.isEmpty())
			t += "  " + s.hdrFormat;
	} else if (s.codecType == "audio") {
		const QString ch = channelStr(s.channels);
		if (!ch.isEmpty()) t += "  " + ch;
		// Language is shown as a flag icon when mapped; fall back to the code
		if (!s.language.isEmpty() && s.language != "und"
		    && McLanguageFlags::countryForLanguage(s.language).isEmpty())
			t += "  " + s.language.toUpper();
		if (s.isDefault)        t += "  \xE2\x98\x85"; // ★
		if (isOriginal)         t += "  \xE2\x97\x8E"; // ◎
		if (isCommentaryTrack(s)) t += "  \xE2\x9C\x8E"; // ✎
	} else if (s.codecType == "subtitle") {
		if (!s.language.isEmpty() && s.language != "und"
		    && McLanguageFlags::countryForLanguage(s.language).isEmpty())
			t += "  " + s.language.toUpper();
		if (s.isForced)           t += "  \xE2\x97\x8F";
		if (s.isDefault)          t += "  \xE2\x98\x85"; // ★
		if (isCommentaryTrack(s)) t += "  \xE2\x9C\x8E"; // ✎
		if (s.isExternal)         t += "  \xE2\x86\x93"; // ↓ = sidecar file
		// isHearingImpaired (🦻) is drawn as a pixmap in drawBadge — not embedded in text.
	}
	return t;
}

int McCardDelegate::badgeWidthFor(const StreamRecord& s, bool isOriginal, const QFontMetrics& fm)
{
	int w = fm.horizontalAdvance(buildBadgeText(s, isOriginal)) + 2 * kBadgePad;
	if (s.codecType != QLatin1String("video")
	    && !McLanguageFlags::countryForLanguage(s.language).isEmpty())
		w += kFlagW + kFlagGap;
	if (streamIsSDH(s))
		w += kFlagH + kFlagGap; // SDH icon drawn as pixmap on the right side of the badge
	return w;
}

QColor McCardDelegate::badgeColor(const QString& codecType)
{
	if (codecType == "video")    return { 0xa0, 0x50, 0x00 };
	if (codecType == "audio")    return { 0x10, 0x6a, 0xc0 };
	if (codecType == "subtitle") return { 0x1a, 0x86, 0x4a };
	return { 0x60, 0x60, 0x60 };
}

QColor McCardDelegate::statusColor(const QString& status)
{
	if (status == "proposed")     return { 0x88, 0x88, 0x88 };
	if (status == "queued")       return { 0xc0, 0x80, 0x00 };
	if (status == "running")      return { 0x10, 0x6a, 0xc0 };
	if (status == "done")         return { 0x1a, 0x86, 0x4a };
	if (status == "failed")       return { 0xcc, 0x22, 0x22 };
	if (status == "cancelled")    return { 0x88, 0x88, 0x88 };
	if (status == "needs_review") return { 0xb8, 0x60, 0x00 };
	if (status == "source")       return { 0x44, 0x72, 0x98 };
	if (status == "output")       return { 0x1a, 0x86, 0x4a };
	return { 0x60, 0x60, 0x60 };
}

QString McCardDelegate::statusLabel(const QString& status)
{
	if (status == "proposed")     return "Proposed";
	if (status == "queued")       return "Queued";
	if (status == "running")      return "Running\xE2\x80\xA6";
	if (status == "done")         return "Done";
	if (status == "failed")       return "Failed";
	if (status == "cancelled")    return "Cancelled";
	if (status == "needs_review") return "Review\xE2\x80\xA6";
	if (status == "source")       return "Source File";
	if (status == "output")       return "Remux File";
	return status;
}

// ── Geometry helpers ───────────────────────────────────────────────────────────

QRect McCardDelegate::playButtonRect(const QRect& contentRect)
{
	const int bY = contentRect.top() + kFolderH + kFolderGap + (kHeaderH - kPlayBtnW) / 2;
	return QRect(contentRect.right() - kPlayBtnW, bY, kPlayBtnW, kPlayBtnW);
}

QRect McCardDelegate::imdbButtonRect(const QRect& contentRect)
{
	const int y = contentRect.top() + kFolderH + kFolderGap + kHeaderH + kSepGap;
	return QRect(contentRect.right() - kImdbBtnW, y, kImdbBtnW, kImdbBtnW);
}

// ── Badge drawing ──────────────────────────────────────────────────────────────

int McCardDelegate::drawBadge(QPainter* p, int x, int y, int h,
                               const QString& text, const QColor& bg, const QFont& font,
                               bool removed, bool hasTip, const QColor& cardBg, bool hovered,
                               const QString& flagLang,
                               const QMap<QString, bool>& streamFlags,
                               bool isSDH)
{
	const qreal dpr     = p->device()->devicePixelRatioF();
	const QPixmap flagPm = McLanguageFlags::flag(flagLang, kFlagH, dpr);
	const bool hasFlag = !flagPm.isNull();
	const int sdhW     = isSDH ? (kFlagH + kFlagGap) : 0;
	const int  w = QFontMetrics(font).horizontalAdvance(text) + 2 * kBadgePad
	             + (hasFlag ? kFlagW + kFlagGap : 0)
	             + sdhW;
	const QRect r(x, y, w, h);

	p->save();
	p->setRenderHint(QPainter::Antialiasing);

	const QColor actualBg = removed
		? (hovered ? bg.darker(150) : bg.darker(180))
		: (hovered ? bg.lighter(130) : bg);
	p->setBrush(actualBg);

	if (hasTip && cardBg.isValid()) {
		const qreal luma = 0.299 * cardBg.redF()
		                 + 0.587 * cardBg.greenF()
		                 + 0.114 * cardBg.blueF();
		QColor border = actualBg.lighter(luma < 0.5 ? 210 : 165);
		p->setPen(QPen(border, 1));
		p->drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
	} else {
		p->setPen(Qt::NoPen);
		p->drawRoundedRect(r, 3, 3);
	}

	int textLeft = r.left() + kBadgePad;
	if (hasFlag) {
		const QRect fr(textLeft, r.top() + (h - kFlagH) / 2, kFlagW, kFlagH);
		if (removed) p->setOpacity(0.4);
		p->drawPixmap(fr, flagPm);
		if (removed) p->setOpacity(1.0);
		textLeft += kFlagW + kFlagGap;
	}

	p->setFont(font);
	// Build set of flag chars to highlight in gold (pending additions, value=true)
	// Removed tracks always render all-white — no point highlighting a removed track's flags.
	static const QHash<QString, QChar> kFlagChar = [] {
		QHash<QString, QChar> m;
		m.insert(QStringLiteral("default"),    QChar(0x2605)); // ★
		m.insert(QStringLiteral("original"),   QChar(0x25CE)); // ◎
		m.insert(QStringLiteral("commentary"), QChar(0x270E)); // ✎
		m.insert(QStringLiteral("forced"),     QChar(0x25CF)); // ●
		return m;
	}();
	static const QColor kFlagAddedColor(0xFF, 0xB8, 0x00); // gold

	QSet<QChar> goldChars;
	if (!removed) {
		for (auto it = streamFlags.constBegin(); it != streamFlags.constEnd(); ++it) {
			if (it.value()) { // value=true means flag is being added
				const QChar c = kFlagChar.value(it.key());
				if (!c.isNull()) goldChars.insert(c);
			}
		}
	}

	const int textRectW = r.right() - kBadgePad - sdhW - textLeft + 1;
	const QColor normalColor = removed ? QColor(0xff, 0xff, 0xff, 90) : Qt::white;

	if (goldChars.isEmpty()) {
		p->setPen(normalColor);
		p->drawText(QRect(textLeft, r.top(), textRectW, h), Qt::AlignVCenter | Qt::AlignLeft, text);
	} else {
		// Draw text in segments, coloring pending-add flag chars gold
		const QFontMetrics fm(font);
		int xOff     = 0;
		int segStart = 0;

		const auto flushSegment = [&](int end) {
			if (end <= segStart) return;
			const QString seg = text.mid(segStart, end - segStart);
			p->setPen(normalColor);
			p->drawText(QRect(textLeft + xOff, r.top(), textRectW - xOff, h),
			            Qt::AlignVCenter | Qt::AlignLeft, seg);
			xOff += fm.horizontalAdvance(seg);
			segStart = end;
		};

		for (int i = 0; i < text.size(); ++i) {
			if (goldChars.contains(text[i])) {
				flushSegment(i);
				const QString ch = text.mid(i, 1);
				p->setPen(kFlagAddedColor);
				p->drawText(QRect(textLeft + xOff, r.top(), textRectW - xOff, h),
				            Qt::AlignVCenter | Qt::AlignLeft, ch);
				xOff += fm.horizontalAdvance(ch);
				segStart = i + 1;
			}
		}
		flushSegment(text.size()); // flush any trailing normal chars
	}

	// SDH icon (closed-caption) — rendered from SVG so it scales and dims correctly.
	// Must come before the strikethrough line so the line renders on top.
	if (isSDH) {
		const QPixmap sdhPm = renderSvgIcon(
		    QStringLiteral(":/icons/closed_caption.svg"), Qt::white, kFlagH, dpr);
		if (!sdhPm.isNull()) {
			const QRect sr(r.right() - kBadgePad - kFlagH,
			               r.top() + (h - kFlagH) / 2, kFlagH, kFlagH);
			if (removed) p->setOpacity(0.4);
			p->drawPixmap(sr, sdhPm);
			if (removed) p->setOpacity(1.0);
		}
	}

	if (removed) {
		const int mid = r.top() + r.height() / 2;
		p->setRenderHint(QPainter::Antialiasing, false);
		p->setPen(QPen(QColor(0xe8, 0x30, 0x30, 180), 2));
		p->drawLine(r.left() + 3, mid, r.right() - 3, mid);
	}

	p->restore();
	return w;
}

QPixmap McCardDelegate::badgePixmap(const QString& text, const QString& codecType,
                                     const QFont& baseFont, qreal dpr,
                                     const QString& flagLang, bool removed)
{
	QFont f = baseFont;
	f.setPointSizeF(baseFont.pointSizeF() * 0.82); // same scale the card views use

	const QFontMetrics fm(f);
	int w = fm.horizontalAdvance(text) + 2 * kBadgePad;
	if (!McLanguageFlags::countryForLanguage(flagLang).isEmpty())
		w += kFlagW + kFlagGap;

	QPixmap pm(qCeil(w * dpr), qCeil(kBadgeH * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	drawBadge(&p, 0, 0, kBadgeH, text, badgeColor(codecType), f,
	          removed, false, {}, false, flagLang);
	p.end();
	return pm;
}

int McCardDelegate::drawBadgeRow(QPainter* p, QRect rowRect,
                                  const QList<StreamRecord>& tracks,
                                  const QSet<int>& removedIndices,
                                  const QFont& badgeFont,
                                  const QColor& cardBg,
                                  const QString& originalLang,
                                  int hoveredStreamIndex,
                                  const QString& flagChangesJson) const
{
	if (tracks.isEmpty()) return 0;

	QList<StreamRecord> sorted = tracks;
	std::sort(sorted.begin(), sorted.end(), [](const StreamRecord& a, const StreamRecord& b) {
		return a.streamIndex < b.streamIndex;
	});

	// Parse pending flag changes: streamIndex → {flagName → newValue}
	QHash<int, QMap<QString, bool>> pendingChanges;
	if (!flagChangesJson.isEmpty()) {
		const QJsonArray arr = QJsonDocument::fromJson(flagChangesJson.toUtf8()).array();
		for (const QJsonValue& v : arr) {
			const QJsonObject o = v.toObject();
			pendingChanges[o[QLatin1String("streamIndex")].toInt()]
			              [o[QLatin1String("flag")].toString()] = o[QLatin1String("value")].toBool();
		}
	}

	const QColor      color = badgeColor(sorted.first().codecType);
	const QFontMetrics fm(badgeFont);
	const int maxX = rowRect.left() + rowRect.width();
	int       x    = rowRect.left();
	int       row  = 0;

	for (int i = 0; i < sorted.size(); ++i) {
		const StreamRecord& s       = sorted.at(i);
		const bool          isOrig  = s.codecType == QLatin1String("audio")
		                           && !originalLang.isEmpty()
		                           && s.language.compare(originalLang, Qt::CaseInsensitive) == 0;

		// Apply pending flag changes to a copy so the badge text reflects desired state
		// even when allStreams still holds the original DB values (e.g. after restart).
		StreamRecord displayS = s;
		const auto   pcIt     = pendingChanges.constFind(s.streamIndex);
		if (pcIt != pendingChanges.constEnd()) {
			for (auto fc = pcIt->constBegin(); fc != pcIt->constEnd(); ++fc) {
				const bool v = fc.value();
				if      (fc.key() == QLatin1String("default"))  displayS.isDefault  = v;
				else if (fc.key() == QLatin1String("forced"))   displayS.isForced   = v;
				else if (fc.key() == QLatin1String("original")) displayS.isOriginal = v;
			}
		}

		const QString text    = buildBadgeText(displayS, isOrig);
		const int     bW      = badgeWidthFor(displayS, isOrig, fm);
		const bool    removed = removedIndices.contains(s.streamIndex);
		const bool    isHov   = (s.streamIndex == hoveredStreamIndex);
		const bool    hasTip  = !displayS.title.isEmpty() || displayS.isDefault || displayS.isForced
		                     || displayS.isOriginal || displayS.isCommentary || displayS.isHearingImpaired || isOrig;
		const bool    isLast  = (i == sorted.size() - 1);
		const bool    isSDH   = streamIsSDH(displayS);

		if (x > rowRect.left() && x + bW > maxX) {
			row++;
			x = rowRect.left();
		}
		const int bY = rowRect.top() + row * (kBadgeH + kRowGap);
		x += drawBadge(p, x, bY, kBadgeH, text, color, badgeFont,
		               removed, hasTip, cardBg, isHov,
		               s.codecType == QLatin1String("video") ? QString() : s.language,
		               pendingChanges.value(s.streamIndex),
		               isSDH);
		if (!isLast) x += kBadgeGap;
	}
	return row + 1;
}

// ── Interactive helpers ────────────────────────────────────────────────────────

bool McCardDelegate::hitTestInteractive(const QPoint& pos, const QRect& itemRect, bool hasImdb) const
{
	const QRect content = itemRect.adjusted(kPosterW + kPosterGap, kPadV, -kPadH, -kPadBottom);
	if (playButtonRect(content).contains(pos)) return true;
	if (hasImdb && imdbButtonRect(content).contains(pos)) return true;
	return false;
}

int McCardDelegate::hitTestBadgeStream(const QPoint& pos, const QRect& itemRect,
                                        const QList<StreamRecord>& tracks,
                                        const QFont& baseFont,
                                        bool hasImdb,
                                        const QString& originalLang) const
{
	const QRect content = itemRect.adjusted(kPosterW + kPosterGap, kPadV, -kPadH, -kPadBottom);
	QFont badgeFont = baseFont;
	badgeFont.setPointSizeF(baseFont.pointSizeF() * 0.82);
	const QFontMetrics fm(badgeFont);
	const int badgeAreaW = content.width() - (hasImdb ? kImdbBtnW + kBadgeGap : 0);

	int y = content.top() + kFolderH + kFolderGap + kHeaderH + kSepGap;

	for (const QString& type : {QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("subtitle")}) {
		QList<StreamRecord> group;
		for (const auto& s : tracks)
			if (s.codecType == type) group << s;
		if (group.isEmpty()) continue;
		std::sort(group.begin(), group.end(), [](const StreamRecord& a, const StreamRecord& b) {
			return a.streamIndex < b.streamIndex;
		});

		int numRows = 1, rx = 0;
		for (const auto& s : group) {
			const bool isOrig = s.codecType == QLatin1String("audio")
			                 && !originalLang.isEmpty()
			                 && s.language.compare(originalLang, Qt::CaseInsensitive) == 0;
			const int bW = badgeWidthFor(s, isOrig, fm);
			if (rx > 0 && rx + bW > badgeAreaW) { numRows++; rx = 0; }
			rx += bW + kBadgeGap;
		}
		const int typeH = numRows * kBadgeH + (numRows - 1) * kRowGap;

		if (type != QStringLiteral("video")) {
			const QRect typeRect(content.left(), y, badgeAreaW, typeH);
			if (typeRect.contains(pos)) {
				int x = content.left(), row = 0;
				for (const auto& s : group) {
					const bool    isOrig = s.codecType == QLatin1String("audio")
					                   && !originalLang.isEmpty()
					                   && s.language.compare(originalLang, Qt::CaseInsensitive) == 0;
					const int bW = badgeWidthFor(s, isOrig, fm);
					if (x > content.left() && x + bW > content.left() + badgeAreaW)
						{ row++; x = content.left(); }
					const int by = y + row * (kBadgeH + kRowGap);
					if (QRect(x, by, bW, kBadgeH).contains(pos))
						return s.streamIndex;
					x += bW + kBadgeGap;
				}
				return -1;
			}
		}
		y += typeH + kRowGap;
	}
	return -1;
}

bool McCardDelegate::editorEvent(QEvent*, QAbstractItemModel*, const QStyleOptionViewItem&, const QModelIndex&)
{
	return false;
}

bool McCardDelegate::eventFilter(QObject* obj, QEvent* event)
{
	if (!m_view || obj != m_view->viewport()) return false;

	switch (event->type()) {
	case QEvent::MouseMove: {
		const auto*  me  = static_cast<const QMouseEvent*>(event);
		const QPoint pos = me->position().toPoint();
		m_lastMousePos = pos;

		const QModelIndex cur = m_view->indexAt(pos);
		if (QPersistentModelIndex(cur) != m_lastHoveredIndex) {
			if (m_lastHoveredIndex.isValid())
				m_view->viewport()->update(m_view->visualRect(m_lastHoveredIndex));
			m_lastHoveredIndex = QPersistentModelIndex(cur);
		}
		if (cur.isValid()) {
			m_view->viewport()->update(m_view->visualRect(cur));
			const bool hasImdb = m_mode == Mode::Library
				? !cur.data(McFileListModel::ImdbRole).toString().isEmpty()
				: !cur.data(McJobListModel::ImdbIdRole).toString().isEmpty();

			bool overInteractive = hitTestInteractive(pos, m_view->visualRect(cur), hasImdb);
			if (!overInteractive && m_mode == Mode::JobQueue
			    && (cur.data(McJobListModel::StatusRole).toString() == QLatin1String("proposed")
			        || cur.data(McJobListModel::StatusRole).toString() == QLatin1String("queued")
			        || cur.data(McJobListModel::StatusRole).toString() == QLatin1String("failed"))) {
				const auto    streams  = cur.data(McJobListModel::AllStreamsRole).value<QList<StreamRecord>>();
				const QString origLang = cur.data(McJobListModel::OriginalLanguageRole).toString();
				overInteractive = (hitTestBadgeStream(pos, m_view->visualRect(cur),
				                                      streams, m_view->font(), hasImdb, origLang) >= 0);
			}
			m_view->viewport()->setCursor(overInteractive ? Qt::PointingHandCursor : Qt::ArrowCursor);
		} else {
			m_view->viewport()->setCursor(Qt::ArrowCursor);
		}
		break;
	}
	case QEvent::Leave:
		m_lastMousePos = QPoint(-1, -1);
		if (m_lastHoveredIndex.isValid()) {
			m_view->viewport()->update(m_view->visualRect(m_lastHoveredIndex));
			m_lastHoveredIndex = QPersistentModelIndex();
		}
		m_view->viewport()->setCursor(Qt::ArrowCursor);
		break;

	default: break;
	}
	return false;
}

bool McCardDelegate::handlePress(const QPoint& pos, const QRect& itemRect,
                                  const QFont& viewFont, const QModelIndex& index)
{
	if (!index.isValid()) return false;
	const QRect content = itemRect.adjusted(kPosterW + kPosterGap, kPadV, -kPadH, -kPadBottom);

	if (playButtonRect(content).contains(pos)) {
		emit playRequested(index);
		return true;
	}

	const bool hasImdb = m_mode == Mode::Library
		? !index.data(McFileListModel::ImdbRole).toString().isEmpty()
		: !index.data(McJobListModel::ImdbIdRole).toString().isEmpty();
	if (hasImdb && imdbButtonRect(content).contains(pos)) {
		emit imdbPageRequested(index);
		return true;
	}

	if (m_mode == Mode::JobQueue
	    && (index.data(McJobListModel::StatusRole).toString() == QLatin1String("proposed")
	        || index.data(McJobListModel::StatusRole).toString() == QLatin1String("queued")
	        || index.data(McJobListModel::StatusRole).toString() == QLatin1String("failed"))) {
		const auto    streams  = index.data(McJobListModel::AllStreamsRole).value<QList<StreamRecord>>();
		const QString origLang = index.data(McJobListModel::OriginalLanguageRole).toString();
		const int streamIdx = hitTestBadgeStream(pos, itemRect, streams, viewFont, hasImdb, origLang);
		if (streamIdx >= 0) {
			emit streamToggleRequested(index, streamIdx);
			return true;
		}
	}

	return false;
}

bool McCardDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view,
                                const QStyleOptionViewItem& option,
                                const QModelIndex& index)
{
	if (event->type() != QEvent::ToolTip)
		return QStyledItemDelegate::helpEvent(event, view, option, index);

	const QRect content = option.rect.adjusted(kPosterW + kPosterGap, kPadV, -kPadH, -kPadBottom);
	const CardData d = fetchData(index);

	// IMDb button tooltip
	if (!d.imdbId.isEmpty() && imdbButtonRect(content).contains(event->pos())) {
		QToolTip::showText(event->globalPos(), tr("Open IMDb page — %1").arg(d.imdbId), view);
		return true;
	}

	// Play button tooltip
	if (playButtonRect(content).contains(event->pos())) {
		const QString tip = ExternalTools::instance().isVlcAvailable()
			? tr("Play in VLC")
			: tr("Play  (VLC not detected — will open in default player)");
		QToolTip::showText(event->globalPos(), tip, view);
		return true;
	}

	QFont badgeFont = option.font;
	badgeFont.setPointSizeF(option.font.pointSizeF() * 0.82);
	const QFontMetrics fm(badgeFont);
	const int badgeAreaW = content.width() - (!d.imdbId.isEmpty() ? kImdbBtnW + kBadgeGap : 0);

	int y = content.top() + kFolderH + kFolderGap + kHeaderH + kSepGap;

	for (const QString& type : {QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("subtitle")}) {
		QList<StreamRecord> group;
		for (const auto& s : d.allStreams)
			if (s.codecType == type) group << s;
		if (group.isEmpty()) continue;

		int numRows = 1, rx = 0;
		for (const auto& s : group) {
			const bool isOrig = s.codecType == QLatin1String("audio")
			                 && !d.originalLanguage.isEmpty()
			                 && s.language.compare(d.originalLanguage, Qt::CaseInsensitive) == 0;
			const int bW = badgeWidthFor(s, isOrig, fm);
			if (rx > 0 && rx + bW > badgeAreaW) { numRows++; rx = 0; }
			rx += bW + kBadgeGap;
		}
		const int typeH = numRows * kBadgeH + (numRows - 1) * kRowGap;
		const QRect typeRect(content.left(), y, badgeAreaW, typeH);

		if (typeRect.contains(event->pos())) {
			int x = content.left(), row = 0;
			for (const auto& s : group) {
				const bool    isOrig = s.codecType == QLatin1String("audio")
				                   && !d.originalLanguage.isEmpty()
				                   && s.language.compare(d.originalLanguage, Qt::CaseInsensitive) == 0;
				const int bW = badgeWidthFor(s, isOrig, fm);
				if (x > content.left() && x + bW > content.left() + badgeAreaW)
					{ row++; x = content.left(); }
				const int by = y + row * (kBadgeH + kRowGap);
				if (QRect(x, by, bW, kBadgeH).contains(event->pos())) {
					static const RegexClassifier clf;
					const auto cls = clf.classify(s.title, s.language, s.codecName);
					QStringList lines;
					if (!s.title.isEmpty())  lines << s.title;
					if (!s.language.isEmpty() && s.language != QLatin1String("und"))
						lines << tr("Language: %1 (%2)")
						         .arg(McLanguageFlags::displayName(s.language), s.language);
					if (s.isDefault)         lines << tr("\xE2\x98\x85  Default track");
					if (s.isForced)          lines << tr("\xE2\x97\x8F  Forced");
					if (s.isOriginal)        lines << tr("\xE2\x97\x8E  Original language (flag)");
					else if (isOrig)         lines << tr("\xE2\x97\x8E  Original audio language");
					if (s.isCommentary)      lines << tr("\xE2\x9C\x8E  Commentary");
					if (streamIsSDH(s)) lines << tr("SDH / Hearing impaired");
					if (cls.type != TrackType::Main) {
						QString typeName;
						switch (cls.type) {
						case TrackType::Commentary:      typeName = tr("Commentary");       break;
						case TrackType::Sdh:             typeName = tr("SDH");              break;
						case TrackType::HearingImpaired: typeName = tr("Hearing impaired"); break;
						case TrackType::Forced:          typeName = tr("Forced subtitle");  break;
						case TrackType::Signs:           typeName = tr("Signs / songs");    break;
						default: break;
						}
						if (!typeName.isEmpty())
							lines << tr("%1  (%2% confidence)").arg(typeName).arg(qRound(cls.confidence * 100));
					}
					if (!lines.isEmpty())
						QToolTip::showText(event->globalPos(), lines.join('\n'), view);
					else
						QToolTip::hideText();
					return true;
				}
				x += bW + kBadgeGap;
			}
		}
		y += typeH + kRowGap;
	}

	QToolTip::hideText();
	return true;
}

// ── sizeHint ──────────────────────────────────────────────────────────────────

QSize McCardDelegate::sizeHint(const QStyleOptionViewItem& option,
                                const QModelIndex& index) const
{
	const int w = m_view ? m_view->viewport()->width() : option.rect.width();
	if (m_cacheWidth == 0 || w != m_cacheWidth) {
		m_sizeCache.clear();
		m_cacheWidth = w;
	}
	const int row = index.row();
	const auto cached = m_sizeCache.constFind(row);
	if (cached != m_sizeCache.constEnd())
		return *cached;

	const CardData d = fetchData(index);
	const bool hasImdb = !d.imdbId.isEmpty();

	{
		QFont badgeFont = option.font;
		badgeFont.setPointSizeF(option.font.pointSizeF() * 0.82);
		if (badgeFont != m_badgeFont) {
			m_badgeFont = badgeFont;
			m_badgeFm   = QFontMetrics(badgeFont);
		}
	}
	const QFontMetrics& fm = m_badgeFm;
	const int totalContentW = m_cacheWidth - kPosterW - kPosterGap - kPadH;
	const int rightReserve  = hasImdb ? kImdbBtnW + kBadgeGap : 0;
	const int badgeAreaW    = totalContentW - rightReserve;

	int totalRows = 0;
	for (const QString& type : {QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("subtitle")}) {
		QList<StreamRecord> group;
		for (const auto& s : d.allStreams)
			if (s.codecType == type) group << s;
		if (group.isEmpty()) continue;

		int rows = 1, x = 0;
		for (const auto& s : group) {
			const bool isOrig = s.codecType == QLatin1String("audio")
			                 && !d.originalLanguage.isEmpty()
			                 && s.language.compare(d.originalLanguage, Qt::CaseInsensitive) == 0;
			const int bW = badgeWidthFor(s, isOrig, fm);
			if (x > 0 && x + bW > badgeAreaW) { rows++; x = 0; }
			x += bW + kBadgeGap;
		}
		totalRows += rows;
	}

	// Each group advances y by rows*(kBadgeH+kRowGap), including a trailing kRowGap.
	// Use the same formula so sizeHint matches what paint actually draws.
	// Enforce a minimum of 3 track rows so cards never shrink below a consistent
	// floor; cards with more rows than 3 simply grow to show them all.
	const int trackH     = qMax(totalRows, 3) * (kBadgeH + kRowGap);
	const int badgeAreaH = hasImdb ? qMax(trackH, kImdbBtnW) : trackH;
	const int h          = qMax(kPadV + kFolderH + kFolderGap + kHeaderH + kSepGap + badgeAreaH + kPadBottom,
	                             kMinRowH);
	const QSize result{m_cacheWidth, h};
	m_sizeCache.insert(row, result);
	return result;
}

void McCardDelegate::clearSizeCache()   { m_sizeCache.clear(); }
void McCardDelegate::invalidateSizeCacheFor(qint64) { m_sizeCache.clear(); }

// ── paint ─────────────────────────────────────────────────────────────────────

void McCardDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                            const QModelIndex& index) const
{
	const CardData d = fetchData(index);

	painter->save();

	// ── Background ────────────────────────────────────────────────────────────
	const bool      sel = option.state & QStyle::State_Selected;
	const QPalette& pal = option.palette;
	const QColor    bg  = sel
		? pal.color(QPalette::Highlight)
		: (index.row() % 2 == 0
			? pal.color(QPalette::Base)
			: pal.color(QPalette::AlternateBase));

	painter->fillRect(option.rect, bg);
	painter->setPen(QPen(pal.color(QPalette::Mid), 1));
	painter->drawLine(option.rect.bottomLeft(), option.rect.bottomRight());

	// ── Fanart backdrop (dimmed background) ───────────────────────────────────
	if (!d.fanartPath.isEmpty()) {
		QPixmap fanart;
		const QString fanartKey = QStringLiteral("fanart:%1:%2:%3")
		                              .arg(d.fanartPath)
		                              .arg(s_fanartVersions.value(d.fanartPath, 0))
		                              .arg(option.rect.width());
		if (!QPixmapCache::find(fanartKey, &fanart)) {
			// Raw cache: populated by prefetchFanart() on download (never evicts).
			// Falls back to disk only for pre-existing fanart on first access.
			QPixmap raw = s_fanartRawCache.value(d.fanartPath);
			if (raw.isNull()) {
				raw = QPixmap(d.fanartPath);
				if (!raw.isNull()) {
					if (raw.height() > 300) {
						const int y = (raw.height() - 300) / 2;
						raw = raw.copy(0, y, raw.width(), 300);
					}
					s_fanartRawCache.insert(d.fanartPath, raw);
				}
			}
			if (!raw.isNull()) {
				fanart = raw.scaled(option.rect.size(), Qt::KeepAspectRatioByExpanding,
				                    Qt::SmoothTransformation);
				QPixmapCache::insert(fanartKey, fanart);
			}
		}
		if (!fanart.isNull()) {
			painter->save();
			painter->setClipRect(option.rect);
			painter->setOpacity(0.05);
			const int ox = (fanart.width()  - option.rect.width())  / 2;
			const int oy = (fanart.height() - option.rect.height()) / 2;
			painter->drawPixmap(option.rect.left() - ox, option.rect.top() - oy, fanart);
			painter->restore();
		}
	}

	// ── Poster column ─────────────────────────────────────────────────────────
	const QRect posterRect(option.rect.left(), option.rect.top(),
	                       kPosterW, option.rect.height());
	if (!d.posterPath.isEmpty()) {
		QPixmap pm;
		// Cache keyed by path+version only — NOT card height — so poster survives
		// resize events where badge reflow changes card height (avoids disk re-read
		// and SmoothTransformation re-scale on every resize).
		const QString cacheKey = QStringLiteral("poster:%1:%2")
		                             .arg(d.posterPath).arg(d.posterVersion);
		if (!QPixmapCache::find(cacheKey, &pm)) {
			const QPixmap src(d.posterPath);
			if (!src.isNull()) {
				pm = src.scaledToWidth(kPosterW, Qt::SmoothTransformation);
				QPixmapCache::insert(cacheKey, pm);
			}
		}
		if (!pm.isNull()) {
			const int px = posterRect.left();
			const int py = posterRect.top() + (option.rect.height() - pm.height()) / 2;
			// Clip to card bounds so tall posters (natural height > card height)
			// don't bleed into the adjacent card above/below.
			painter->save();
			painter->setClipRect(posterRect);
			painter->drawPixmap(px, py, pm);
			painter->restore();
		}
	}

	// Job queue: checkbox overlaid on bottom-left of poster column for proposed jobs
	if (m_mode == Mode::JobQueue && d.status == QLatin1String("proposed")) {
		QStyleOptionButton cbOpt;
		cbOpt.rect  = QRect(posterRect.left() + 3, posterRect.bottom() - 19, 16, 16);
		cbOpt.state = QStyle::State_Enabled | (d.checked ? QStyle::State_On : QStyle::State_Off);
		QApplication::style()->drawControl(QStyle::CE_CheckBox, &cbOpt, painter);
	}

	// ── Content area ─────────────────────────────────────────────────────────
	const QRect content = option.rect.adjusted(kPosterW + kPosterGap, kPadV, -kPadH, -kPadBottom);
	const QColor textColor = sel ? pal.color(QPalette::HighlightedText) : pal.color(QPalette::Text);
	const QColor dimColor  = sel ? textColor.darker(140) : pal.color(QPalette::PlaceholderText);

	painter->setClipRect(QRect(option.rect.left() + kPosterW, option.rect.top(),
	                           option.rect.width() - kPosterW, option.rect.height()));

	// ── Folder / title row: smart title (left) | duration · size | rating ─────
	{
		const QRect folderRect(content.left(), content.top(), content.width(), kFolderH);

		// Smart title priority: displayTitle > containerTitle (if not filename-like) > folder name
		const QString stemmed    = QFileInfo(d.filePath).completeBaseName();
		const QString folderName = QFileInfo(d.filePath).dir().dirName();
		QString smartTitle;
		if (!d.displayTitle.isEmpty()) {
			smartTitle = (d.displayYear > 0)
			    ? d.displayTitle + QStringLiteral(" (") + QString::number(d.displayYear) + QStringLiteral(")")
			    : d.displayTitle;
		} else if (!d.containerTitle.isEmpty()
		           && d.containerTitle.compare(stemmed, Qt::CaseInsensitive) != 0) {
			smartTitle = d.containerTitle;
		} else {
			smartTitle = folderName;
		}

		QFont starFont = option.font;
		QFont numFont  = option.font;
		numFont.setBold(true);
		numFont.setPointSizeF(option.font.pointSizeF() * 0.80);
		QFont tenFont = option.font;
		tenFont.setPointSizeF(option.font.pointSizeF() * 0.70);
		QFont metaFont = option.font;
		metaFont.setPointSizeF(option.font.pointSizeF() * 0.80);

		const bool hasRating = (d.rating > 0.0);

		// Build meta text: duration + size (both modes show this in the folder row)
		QString metaText;
		if (d.durationSec > 0) metaText = formatDuration(d.durationSec);
		if (d.sizeBytes    > 0) metaText += (metaText.isEmpty() ? QString() : QStringLiteral("  ")) + formatSize(d.sizeBytes);

		// Draw right-side elements from right → left, tracking the right edge
		int rightEdge = folderRect.right();
		const int midY = folderRect.top() + folderRect.height() / 2;

		// Rating: ★ X.X/10
		if (hasRating) {
			const QString starText  = QStringLiteral("\xe2\x98\x85");
			const QString scoreText = QStringLiteral("%1").arg(d.rating, 0, 'f', 1);
			const QString tenText   = QStringLiteral("/10");
			const QFontMetrics tenFm(tenFont), numFm(numFont), starFm(starFont);
			const int tenW     = tenFm.horizontalAdvance(tenText);
			const int scoreW   = numFm.horizontalAdvance(scoreText);
			const int starW    = starFm.horizontalAdvance(starText);
			const int numBase  = midY + numFm.ascent()  / 2;
			const int starBase = midY + starFm.ascent() / 2;

			int x = rightEdge - tenW;
			painter->save();
			painter->setFont(tenFont);
			painter->setPen(sel ? dimColor : pal.color(QPalette::PlaceholderText));
			painter->drawText(x, numBase, tenText);
			x -= scoreW;
			painter->setFont(numFont);
			painter->setPen(sel ? textColor : pal.color(QPalette::Text));
			painter->drawText(x, numBase, scoreText);
			x -= starW + 3;
			painter->setFont(starFont);
			painter->setPen(sel ? QColor(0xF5, 0xC5, 0x18).darker(110) : QColor(0xF5, 0xC5, 0x18));
			painter->drawText(x, starBase, starText);
			painter->restore();

			rightEdge = x - 8;  // gap between rating and meta text
		}

		// Duration · size
		if (!metaText.isEmpty()) {
			const QFontMetrics mfm(metaFont);
			const int mW   = mfm.horizontalAdvance(metaText);
			const int mBase = midY + mfm.ascent() / 2;
			painter->save();
			painter->setFont(metaFont);
			painter->setPen(dimColor);
			painter->drawText(rightEdge - mW, mBase, metaText);
			painter->restore();
			rightEdge -= mW + 8;  // gap between meta and title
		}

		// Smart title — fills remaining left portion
		QFont titleFont = option.font;
		titleFont.setBold(true);
		const int titleW = rightEdge - folderRect.left() - 4;
		if (titleW > 20) {
			const QRect titleRect(folderRect.left(), folderRect.top(), titleW, folderRect.height());
			painter->setFont(titleFont);
			painter->setPen(textColor);
			painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, smartTitle);
		}
	}

	// ── Header: VLC button (right) | right-side meta | filename (left) ───────
	{
		QFont fnFont = option.font;
		fnFont.setPointSizeF(option.font.pointSizeF() * 0.85);

		const QRect hdr(content.left(), content.top() + kFolderH + kFolderGap,
		                content.width(), kHeaderH);

		// VLC logo — rightmost element, vertically centred in header
		const QRect playBtn = playButtonRect(content);
		const bool  hovered = playBtn.contains(m_lastMousePos);
		{
			QPixmap vlcLogo;
			const QString cacheKey = QStringLiteral("vlc_logo_%1").arg(playBtn.height());
			if (!QPixmapCache::find(cacheKey, &vlcLogo)) {
				vlcLogo = QPixmap(":/icons/vlc.svg").scaled(
				    playBtn.height(), playBtn.height(),
				    Qt::KeepAspectRatio, Qt::SmoothTransformation);
				QPixmapCache::insert(cacheKey, vlcLogo);
			}
			painter->save();
			painter->setOpacity(hovered ? 1.0 : 0.80);
			const int ox = playBtn.left() + (playBtn.width()  - vlcLogo.width())  / 2;
			const int oy = playBtn.top()  + (playBtn.height() - vlcLogo.height()) / 2;
			painter->drawPixmap(ox, oy, vlcLogo);
			painter->restore();
		}

		// Right-side meta: library → play only / job → status pill
		int rightEdgeForFilename = playBtn.left() - kBadgeGap;

		if (m_mode == Mode::JobQueue) {
			QFont pillFont = option.font;
			pillFont.setPointSizeF(option.font.pointSizeF() * 0.80);

			// Status pill (with progress/saved annotation if relevant)
			QString pillText = statusLabel(d.status);
			if (d.status == QLatin1String("running") && d.progress > 0)
				pillText = QStringLiteral("Running %1%\xE2\x80\xA6").arg(d.progress);

			const bool isPending = (d.status == QLatin1String("proposed")
			                     || d.status == QLatin1String("queued")
			                     || d.status == QLatin1String("source"));
			qint64 displaySavedBytes = (isPending && d.sizeBytes > 0)
				? estimateSavingBytes(d.allStreams, d.removedIndices, d.sizeBytes, d.durationSec)
				: d.savedBytes;
			const bool isEstimate = isPending && (displaySavedBytes > 0);
			if (displaySavedBytes > 0) {
				const QString prefix = isEstimate ? QStringLiteral("~-") : QStringLiteral("-");
				const double  gb     = displaySavedBytes / 1073741824.0;
				const QString saved  = gb >= 1.0
					? QStringLiteral("%1%2 GB").arg(prefix).arg(gb, 0, 'f', 2)
					: QStringLiteral("%1%2 MB").arg(prefix).arg(displaySavedBytes / 1048576.0, 0, 'f', 1);
				pillText += QStringLiteral("  %1").arg(saved);
			}
			const int pillW = QFontMetrics(pillFont).horizontalAdvance(pillText) + 2 * kBadgePad;
			const int pillX = playBtn.left() - kBadgeGap - pillW;
			const int pillY = hdr.top() + (hdr.height() - kBadgeH) / 2;
			drawBadge(painter, pillX, pillY, kBadgeH,
			          pillText, statusColor(d.status), pillFont);

			rightEdgeForFilename = pillX;
		}

		// Filename — fills remaining left space
		painter->setFont(fnFont);
		painter->setPen(dimColor);
		painter->drawText(QRect(hdr.left(), hdr.top(), rightEdgeForFilename - hdr.left() - 4, hdr.height()),
		                  Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, d.filename);
	}

	// ── Badge area: IMDb button (right) + track rows (left) ─────────────────
	const bool hasImdb  = !d.imdbId.isEmpty();
	const int rightReserve = hasImdb ? kImdbBtnW + kBadgeGap : 0;
	const int badgeAreaW   = content.width() - rightReserve;

	if (hasImdb) {
		const QRect ir = imdbButtonRect(content);
		QPixmap logo;
		if (!QPixmapCache::find("imdb_logo", &logo)) {
			logo = QPixmap(":/icons/imdb.png").scaled(
			    ir.width(), ir.height(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
			QPixmapCache::insert("imdb_logo", logo);
		}
		painter->save();
		painter->setOpacity(ir.contains(m_lastMousePos) ? 1.0 : 0.75);
		const int ox = ir.left() + (ir.width()  - logo.width())  / 2;
		const int oy = ir.top()  + (ir.height() - logo.height()) / 2;
		painter->drawPixmap(ox, oy, logo);
		painter->restore();
	}

	// Hover-highlight the badge under the mouse for toggleable job cards
	const int hoveredStreamIdx = (m_mode == Mode::JobQueue && d.toggleable && m_lastMousePos.x() >= 0)
		? hitTestBadgeStream(m_lastMousePos, option.rect, d.allStreams, option.font, hasImdb, d.originalLanguage)
		: -1;

	QFont badgeFont = option.font;
	badgeFont.setPointSizeF(option.font.pointSizeF() * 0.82);
	int y = content.top() + kFolderH + kFolderGap + kHeaderH + kSepGap;

	const auto drawGroup = [&](const QString& type) {
		QList<StreamRecord> group;
		for (const auto& s : d.allStreams)
			if (s.codecType == type) group << s;
		if (group.isEmpty()) return;
		const int rows = drawBadgeRow(painter,
		                              QRect(content.left(), y, badgeAreaW, kBadgeH),
		                              group, d.removedIndices, badgeFont, bg,
		                              d.originalLanguage, hoveredStreamIdx,
		                              d.flagChangesJson);
		y += rows * (kBadgeH + kRowGap);
	};

	drawGroup("video");
	drawGroup("audio");
	drawGroup("subtitle");

	// Job queue: progress bar at the bottom of the card (painted last, no clip)
	if (m_mode == Mode::JobQueue && d.status == QLatin1String("running") && d.progress > 0) {
		painter->setClipping(false);
		const QRect barRect(option.rect.left(), option.rect.bottom() - 2,
		                    option.rect.width(), 3);
		painter->fillRect(barRect, QColor(0x30, 0x30, 0x40));
		const int fillW = barRect.width() * d.progress / 100;
		if (fillW > 0) {
			const QColor fillColor = m_pulseAnim
				? m_pulseAnim->currentValue().value<QColor>()
				: QColor(0x10, 0x6a, 0xc0);
			painter->fillRect(QRect(barRect.left(), barRect.top(), fillW, barRect.height()),
			                  fillColor);
		}
	}

	painter->restore();
}

} // namespace Mc
