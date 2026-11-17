#include "nfoview/McNfoView.h"
#include "nfoview/NfoText.h"

#include <QAction>
#include <QActionGroup>
#include <QFont>
#include <QFontDatabase>
#include <QMenu>
#include <QSettings>
#include <QFontMetricsF>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QAbstractTextDocumentLayout>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QScreen>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

namespace Mc {

namespace {
constexpr int    kMaxImages     = 24;
constexpr qint64 kMaxImageBytes = 10 * 1024 * 1024;
constexpr int    kTimeoutMs     = 15000;

constexpr int kDefaultFontSize = 9;
constexpr int kMinFontSize     = 5;
constexpr int kMaxFontSize     = 40;
const char*   kKeyFontFamily   = "nfoViewer/fontFamily";
const char*   kKeyFontSize     = "nfoViewer/fontPointSize";
const char*   kKeyNightColors  = "nfoViewer/nightColors";
const char*   kKeySmoothShades = "nfoViewer/smoothShades";
// Offered in the Font menu when installed; the first one found is the default.
// Windows' raster "Terminal" (DAMN NFO Viewer's default) is deliberately absent: Qt
// addresses it by OEM byte value, so every accented letter comes out as the wrong glyph.
const char* const kFontFamilies[] = { "Consolas", "Courier New", "Lucida Console", "Cascadia Mono" };

constexpr qreal kDosCellHeightPerWidth = 12.0 / 5.0;

constexpr char16_t kFirstBlockElement = 0x2580;
constexpr char16_t kLastBlockElement  = 0x259F;
// CP437 0x08. In the DOS character set it is a filled cell with a hole, but
// DAMN NFO Viewer — what NFO art is checked against — shows it as an empty
// cell with a small dot, so that is what is painted here. Left to the font it
// is a boxed symbol that fits neither.
constexpr char16_t kInverseBullet = 0x25D8;
// A character the file no longer holds (see NfoText::decodeBytes). Nearly
// always it was art, so it is painted as a flat grey cell: the outline of
// the picture stays readable, without pretending to know what was there.
constexpr char16_t kLostCharacter = 0xFFFD;

// One Unicode block element (U+2580-259F) as geometry: up to two rectangles in
// eighths of the cell, or a shade (coverage in quarters, whole cell).
struct BlockShape {
	int shade = 0;                       // 1-3 for the light/medium/dark shades, else 0
	int rects[2][4] = { {}, {} };        // x0, y0, x1, y1 in eighths; x0 == x1 means unused
};

BlockShape blockShape(char16_t ch)
{
	BlockShape s;
	auto set = [&s](int i, int x0, int y0, int x1, int y1) {
		s.rects[i][0] = x0; s.rects[i][1] = y0; s.rects[i][2] = x1; s.rects[i][3] = y1;
	};
	if (ch == 0x2580) set(0, 0, 0, 8, 4);                               // upper half
	else if (ch >= 0x2581 && ch <= 0x2588) set(0, 0, 8 - (ch - 0x2580), 8, 8);   // lower n/8 … full
	else if (ch >= 0x2589 && ch <= 0x258F) set(0, 0, 0, 0x2590 - ch, 8);         // left 7/8 … 1/8
	else if (ch == 0x2590) set(0, 4, 0, 8, 8);                          // right half
	else if (ch >= 0x2591 && ch <= 0x2593) s.shade = ch - 0x2590;
	else if (ch == 0x2594) set(0, 0, 0, 8, 1);                          // upper 1/8
	else if (ch == 0x2595) set(0, 7, 0, 8, 8);                          // right 1/8
	else {
		// Quadrants, as a bitmask: 1 = upper left, 2 = upper right, 4 = lower left, 8 = lower right.
		static constexpr int kQuadrants[10] = { 4, 8, 1, 1 | 4 | 8, 1 | 8, 1 | 2 | 4, 1 | 2 | 8, 2, 2 | 4, 2 | 4 | 8 };
		const int q = kQuadrants[ch - 0x2596];
		const int upper = q & 3, lower = (q >> 2) & 3;
		// Each half-row is empty, one quadrant, or the full width.
		auto row = [&set](int i, int bits, int y0, int y1) {
			if (bits) set(i, (bits == 2) ? 4 : 0, y0, (bits == 1) ? 4 : 8, y1);
		};
		row(0, upper, 0, 4);
		row(1, lower, 4, 8);
	}
	return s;
}

// The box-drawing characters CP437 has, as four arms from the cell center:
// 2 bits each for up, right, down, left — 1 = single line, 2 = double line.
constexpr quint8 boxArmsOf(int up, int right, int down, int left)
{
	return static_cast<quint8>(up | (right << 2) | (down << 4) | (left << 6));
}
struct BoxChar { char16_t ch; quint8 arms; };
constexpr BoxChar kBoxChars[] = {
	{ 0x2500, boxArmsOf(0, 1, 0, 1) }, { 0x2502, boxArmsOf(1, 0, 1, 0) }, { 0x250C, boxArmsOf(0, 1, 1, 0) },
	{ 0x2510, boxArmsOf(0, 0, 1, 1) }, { 0x2514, boxArmsOf(1, 1, 0, 0) }, { 0x2518, boxArmsOf(1, 0, 0, 1) },
	{ 0x251C, boxArmsOf(1, 1, 1, 0) }, { 0x2524, boxArmsOf(1, 0, 1, 1) }, { 0x252C, boxArmsOf(0, 1, 1, 1) },
	{ 0x2534, boxArmsOf(1, 1, 0, 1) }, { 0x253C, boxArmsOf(1, 1, 1, 1) }, { 0x2550, boxArmsOf(0, 2, 0, 2) },
	{ 0x2551, boxArmsOf(2, 0, 2, 0) }, { 0x2552, boxArmsOf(0, 2, 1, 0) }, { 0x2553, boxArmsOf(0, 1, 2, 0) },
	{ 0x2554, boxArmsOf(0, 2, 2, 0) }, { 0x2555, boxArmsOf(0, 0, 1, 2) }, { 0x2556, boxArmsOf(0, 0, 2, 1) },
	{ 0x2557, boxArmsOf(0, 0, 2, 2) }, { 0x2558, boxArmsOf(1, 2, 0, 0) }, { 0x2559, boxArmsOf(2, 1, 0, 0) },
	{ 0x255A, boxArmsOf(2, 2, 0, 0) }, { 0x255B, boxArmsOf(1, 0, 0, 2) }, { 0x255C, boxArmsOf(2, 0, 0, 1) },
	{ 0x255D, boxArmsOf(2, 0, 0, 2) }, { 0x255E, boxArmsOf(1, 2, 1, 0) }, { 0x255F, boxArmsOf(2, 1, 2, 0) },
	{ 0x2560, boxArmsOf(2, 2, 2, 0) }, { 0x2561, boxArmsOf(1, 0, 1, 2) }, { 0x2562, boxArmsOf(2, 0, 2, 1) },
	{ 0x2563, boxArmsOf(2, 0, 2, 2) }, { 0x2564, boxArmsOf(0, 2, 1, 2) }, { 0x2565, boxArmsOf(0, 1, 2, 1) },
	{ 0x2566, boxArmsOf(0, 2, 2, 2) }, { 0x2567, boxArmsOf(1, 2, 0, 2) }, { 0x2568, boxArmsOf(2, 1, 0, 1) },
	{ 0x2569, boxArmsOf(2, 2, 0, 2) }, { 0x256A, boxArmsOf(1, 2, 1, 2) }, { 0x256B, boxArmsOf(2, 1, 2, 1) },
	{ 0x256C, boxArmsOf(2, 2, 2, 2) },
};

quint8 boxArms(char16_t ch)
{
	if (ch < 0x2500 || ch > 0x256C) return 0;
	for (const BoxChar& box : kBoxChars) {
		if (box.ch == ch) return box.arms;
	}
	return 0;
}

// Paints one box-drawing character into its cell with strokes t pixels thick.
// The cell is cut into five columns and five rows around the center — outer
// margin, first stroke of a double line, center, second stroke, outer margin —
// and each arm fills a band of them. A double arm is a 3t band with its middle
// cleared again; how far band and middle reach into the junction is what
// makes corners, tees and crossings come out right.
void paintBox(QPainter& painter, int x0, int y0, int x1, int y1, quint8 arms, const QColor& fg, const QColor& bg, int t)
{
	const int up = arms & 3, right = (arms >> 2) & 3, down = (arms >> 4) & 3, left = (arms >> 6) & 3;
	const int cx = x0 + (x1 - x0 - t) / 2;
	const int cy = y0 + (y1 - y0 - t) / 2;
	const int X[6] = { x0, cx - t, cx, cx + t, cx + 2 * t, x1 };
	const int Y[6] = { y0, cy - t, cy, cy + t, cy + 2 * t, y1 };

	// Index range in X covered by the vertical line, and in Y by the horizontal one.
	const bool vDouble = qMax(up, down) == 2, hDouble = qMax(left, right) == 2;
	const int  vLo = vDouble ? 1 : 2, vHi = vDouble ? 4 : 3;
	const int  hLo = hDouble ? 1 : 2, hHi = hDouble ? 4 : 3;
	const bool vThrough = up && down, hThrough = left && right;
	auto fill = [&painter](int ax, int ay, int bx, int by, const QColor& color) {
		if (bx > ax && by > ay) painter.fillRect(QRect(ax, ay, bx - ax, by - ay), color);
	};
	auto lo = [](int type) { return type == 2 ? 1 : 2; };
	auto hi = [](int type) { return type == 2 ? 4 : 3; };

	painter.fillRect(QRect(x0, y0, x1 - x0, y1 - y0), bg);

	// A single arm meeting a double line that runs straight through stops at
	// its near stroke; in every other case an arm runs to the far side of the
	// line it meets, which is what closes a corner.
	const bool vStopsShort = hDouble && hThrough, hStopsShort = vDouble && vThrough;
	if (up)    fill(X[lo(up)], Y[0], X[hi(up)], Y[(up == 1 && vStopsShort) ? hLo : hHi], fg);
	if (down)  fill(X[lo(down)], Y[(down == 1 && vStopsShort) ? hHi : hLo], X[hi(down)], Y[5], fg);
	if (left)  fill(X[0], Y[lo(left)], X[(left == 1 && hStopsShort) ? vLo : vHi], Y[hi(left)], fg);
	if (right) fill(X[(right == 1 && hStopsShort) ? vHi : vLo], Y[lo(right)], X[5], Y[hi(right)], fg);

	// The gap inside a double arm joins the gap of the line it meets (or of
	// its own continuation); against a single line it stops before it.
	if (up == 2)    fill(X[2], Y[0], X[3], Y[(hDouble || down) ? 3 : hLo], bg);
	if (down == 2)  fill(X[2], Y[(hDouble || up) ? 2 : hHi], X[3], Y[5], bg);
	if (left == 2)  fill(X[0], Y[2], X[(vDouble || right) ? 3 : vLo], Y[3], bg);
	if (right == 2) fill(X[(vDouble || left) ? 2 : vHi], Y[2], X[5], Y[3], bg);

	// A single line crossing a double one is drawn straight across its gap.
	if (up == 1 && down == 1)    fill(X[2], Y[1], X[3], Y[4], fg);
	if (left == 1 && right == 1) fill(X[1], Y[2], X[4], Y[3], fg);
}

// QTextBrowser that paints block elements itself, as exact cell-filling
// rectangles. Left to the font they never tile: glyph antialiasing leaves a
// seam between neighbouring columns, the line spacing leaves one between rows,
// and the shade glyphs' dither pattern turns into stripes at most sizes.
// render() makes those glyphs transparent, so only these rectangles show.
class NfoBrowser : public QTextBrowser
{
public:
	using QTextBrowser::QTextBrowser;

	bool smoothShades = false;

protected:
	void scrollContentsBy(int dx, int dy) override
	{
		// Scrolling shifts the existing pixels and repaints only the exposed
		// strip. Cells here reach past their text line, so a strip that starts
		// mid-cell left a thin line of background behind; repainting the whole
		// viewport is cheap for a document this size and cannot tear.
		QTextBrowser::scrollContentsBy(dx, dy);
		viewport()->update();
	}

	void paintEvent(QPaintEvent* event) override
	{
		QTextBrowser::paintEvent(event);

		QPainter painter(viewport());
		const QPointF scroll(horizontalScrollBar()->value(), verticalScrollBar()->value());
		const QRectF clip = QRectF(event->rect()).translated(scroll);
		painter.translate(-scroll);

		const QAbstractTextDocumentLayout* docLayout = document()->documentLayout();
		const QTextCursor cursor = textCursor();
		const int selStart = cursor.selectionStart();
		const int selEnd   = cursor.selectionEnd();
		const QColor paper = palette().color(QPalette::Base);
		const QColor ink   = palette().color(QPalette::Text);
		const QColor selPaper = palette().color(QPalette::Highlight);
		const QColor selInk   = palette().color(QPalette::HighlightedText);
		auto mix = [](const QColor& bg, const QColor& fg, int quarters) {
			return QColor((bg.red() * (4 - quarters) + fg.red() * quarters) / 4,
			              (bg.green() * (4 - quarters) + fg.green() * quarters) / 4,
			              (bg.blue() * (4 - quarters) + fg.blue() * quarters) / 4);
		};
		// Shade dot patterns, as in the 8-pixel-wide DOS font: dots are an eighth
		// of a cell, sized in device pixels so they stay crisp on scaled displays.
		// The brush is anchored to the document, so the pattern runs unbroken
		// across neighbouring cells and rows.
		const qreal dpr = devicePixelRatioF();
		const int   dot = qMax(1, qRound(QFontMetricsF(font()).horizontalAdvance(QChar(0x2588)) * dpr / 8.0));
		auto ditherBrush = [dot, dpr](int shade, const QColor& fg) {
			QImage img(4 * dot, 2 * dot, QImage::Format_ARGB32_Premultiplied);
			img.fill(Qt::transparent);
			QPainter p(&img);
			for (int py = 0; py < 2; ++py) {
				for (int px = 0; px < 4; ++px) {
					const bool sparse = (px == (py ? 2 : 0));          // 1 dot in 4
					const bool on = shade == 1 ? sparse
					              : shade == 2 ? ((px + py) % 2 == 0)   // checkerboard
					              : !(px == (py ? 3 : 1));              // 3 dots in 4
					if (on) p.fillRect(px * dot, py * dot, dot, dot, fg);
				}
			}
			p.end();
			QBrush brush(img);
			brush.setTransform(QTransform::fromScale(1.0 / dpr, 1.0 / dpr));
			return brush;
		};
		QBrush dither[2][3];   // [selected][shade - 1], built on first use
		// Line strokes: a seventh of a cell, like the DOS font's.
		const int lineWidth = qMax(1, qRound(QFontMetricsF(font()).horizontalAdvance(QChar(0x2588)) / 7.0));
		auto firstLineTop = [](const QTextBlock& block) {
			const QTextLayout* layout = block.layout();
			return layout->position().y() + (layout->lineCount() > 0 ? layout->lineAt(0).y() : 0.0);
		};

		for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
			const QRectF blockRect = docLayout->blockBoundingRect(block);
			// A row is painted down to where the next one starts, which is below its
			// own bounding rect (taller DOS-shaped lines, row gaps). Testing the rect
			// alone skips a row whose last pixels are all a repaint asks for, and
			// scrolling then leaves a line of bare background behind.
			const QTextBlock nextBlock = block.next();
			const qreal paintedBottom = nextBlock.isValid()
				? qMax(blockRect.bottom(), docLayout->blockBoundingRect(nextBlock).top())
				: blockRect.bottom();
			if (paintedBottom < clip.top()) continue;
			if (blockRect.top() > clip.bottom()) break;

			const QString      text   = block.text();
			const QTextLayout* layout = block.layout();
			const QPointF      origin = layout->position();
			for (int l = 0; l < layout->lineCount(); ++l) {
				const QTextLine line = layout->lineAt(l);
				const qreal top = origin.y() + line.y();
				// Down to where the next row starts, so rows join without a gap —
				// unless that is further than a line away (a paragraph margin).
				qreal bottom = top + line.height();
				if (l + 1 < layout->lineCount())
					bottom = origin.y() + layout->lineAt(l + 1).y();
				else if (block.next().isValid())
					bottom = qMin(firstLineTop(block.next()), top + line.height() * 1.5);
				const int y0 = qRound(top);
				const int y1 = qRound(bottom);

				const int end = qMin(line.textStart() + line.textLength(), int(text.size()));
				for (int i = line.textStart(); i < end; ++i) {
					const char16_t ch = text[i].unicode();
					const quint8 box = boxArms(ch);
					if (!box && ch != kInverseBullet && ch != kLostCharacter
					    && (ch < kFirstBlockElement || ch > kLastBlockElement))
						continue;

					const int x0 = qRound(origin.x() + line.cursorToX(i));
					const int x1 = qRound(origin.x() + line.cursorToX(i + 1));
					const int pos = block.position() + i;
					const bool selected = pos >= selStart && pos < selEnd;
					const QColor& bg = selected ? selPaper : paper;
					const QColor& fg = selected ? selInk : ink;

					if (box) {
						paintBox(painter, x0, y0, x1, y1, box, fg, bg, lineWidth);
						continue;
					}
					if (ch == kLostCharacter) {
						painter.fillRect(QRect(x0, y0, x1 - x0, y1 - y0), mix(bg, fg, 2));
						continue;
					}
					if (ch == kInverseBullet) {
						const int dotSize = qMax(1, qRound((x1 - x0) * 0.25));
						painter.fillRect(QRect(x0, y0, x1 - x0, y1 - y0), bg);
						painter.fillRect(QRect(x0 + (x1 - x0 - dotSize) / 2, y0 + (y1 - y0 - dotSize) / 2, dotSize, dotSize), fg);
						continue;
					}
					const BlockShape shape = blockShape(ch);
					if (shape.shade && smoothShades) {
						painter.fillRect(QRect(x0, y0, x1 - x0, y1 - y0), mix(bg, fg, shape.shade));
						continue;
					}
					if (shape.shade) {
						QBrush& brush = dither[selected][shape.shade - 1];
						if (brush.style() == Qt::NoBrush)
							brush = ditherBrush(shape.shade, fg);
						painter.fillRect(QRect(x0, y0, x1 - x0, y1 - y0), bg);
						painter.fillRect(QRect(x0, y0, x1 - x0, y1 - y0), brush);
						continue;
					}
					painter.fillRect(QRect(x0, y0, x1 - x0, y1 - y0), bg);
					for (const auto& r : shape.rects) {
						if (r[0] == r[2]) continue;
						const int rx0 = x0 + qRound((x1 - x0) * r[0] / 8.0);
						const int rx1 = x0 + qRound((x1 - x0) * r[2] / 8.0);
						const int ry0 = y0 + qRound((y1 - y0) * r[1] / 8.0);
						const int ry1 = y0 + qRound((y1 - y0) * r[3] / 8.0);
						painter.fillRect(QRect(rx0, ry0, rx1 - rx0, ry1 - ry0), fg);
					}
				}
			}
		}
	}
};
} // namespace

McNfoView::McNfoView(QWidget* parent)
	: QWidget(parent)
{
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	m_browser = new NfoBrowser(this);
	m_browser->setOpenExternalLinks(true);
	m_browser->setLineWrapMode(QTextEdit::NoWrap);
	layout->addWidget(m_browser);

	const QSettings settings;
	QString family = settings.value(kKeyFontFamily).toString();
	if (family.isEmpty() || !QFontDatabase::hasFamily(family)) {
		family = QStringLiteral("Courier New");
		for (const char* name : kFontFamilies) {
			if (QFontDatabase::hasFamily(QString::fromLatin1(name))) {
				family = QString::fromLatin1(name);
				break;
			}
		}
	}
	QFont mono(family, qBound(kMinFontSize, settings.value(kKeyFontSize, kDefaultFontSize).toInt(), kMaxFontSize));
	mono.setStyleHint(QFont::Monospace);
	m_browser->setFont(mono);
	setColors(settings.value(kKeyNightColors, false).toBool() ? Colors::Night : Colors::Paper);
	setSmoothShades(settings.value(kKeySmoothShades, false).toBool());

	buildViewActions();
	m_browser->installEventFilter(this);
	m_browser->document()->setUndoRedoEnabled(false);
	m_browser->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(m_browser, &QWidget::customContextMenuRequested, this, &McNfoView::showContextMenu);
}

McNfoView::~McNfoView()
{
	// Ctrl+wheel zooms the text widget directly, so the size is read back here
	// rather than tracked.
	QSettings().setValue(kKeyFontSize, m_browser->font().pointSize());
}

void McNfoView::buildViewActions()
{
	// Shortcuts work while the view has focus, in whatever window hosts it.
	auto add = [this](const QString& text, const QList<QKeySequence>& keys, auto slot) {
		auto* act = new QAction(text, this);
		act->setShortcuts(keys);
		act->setShortcutContext(Qt::WidgetWithChildrenShortcut);
		connect(act, &QAction::triggered, this, slot);
		addAction(act);
		m_viewActions << act;
		return act;
	};
	add(tr("Zoom &In"), { QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal) },
	    [this]() { zoomTo(m_browser->font().pointSize() + 1); });
	add(tr("Zoom &Out"), { QKeySequence::ZoomOut }, [this]() { zoomTo(m_browser->font().pointSize() - 1); });
	add(tr("&Reset Zoom"), { QKeySequence(Qt::CTRL | Qt::Key_0) }, [this]() { zoomTo(kDefaultFontSize); });

	auto* separator = new QAction(this);
	separator->setSeparator(true);
	m_viewActions << separator;

	auto* fontMenu  = new QMenu(tr("F&ont"), this);
	auto* fontGroup = new QActionGroup(this);
	for (const char* name : kFontFamilies) {
		const QString family = QString::fromLatin1(name);
		if (!QFontDatabase::hasFamily(family)) continue;
		QAction* act = fontMenu->addAction(family);
		act->setCheckable(true);
		act->setChecked(fontFamily().compare(family, Qt::CaseInsensitive) == 0);
		fontGroup->addAction(act);
		connect(act, &QAction::triggered, this, [this, family]() {
			QSettings().setValue(kKeyFontFamily, family);
			setFontFamily(family);
			emit cellSizeChanged();
		});
	}
	m_viewActions << fontMenu->menuAction();

	QAction* night = add(tr("&Night Colors (Inverted)"), { QKeySequence(Qt::CTRL | Qt::Key_I) }, [this](bool on) {
		QSettings().setValue(kKeyNightColors, on);
		setColors(on ? Colors::Night : Colors::Paper);
	});
	night->setCheckable(true);
	night->setChecked(m_colors == Colors::Night);

	QAction* smooth = add(tr("&Smooth Shading"), {}, [this](bool on) {
		QSettings().setValue(kKeySmoothShades, on);
		setSmoothShades(on);
	});
	smooth->setCheckable(true);
	smooth->setChecked(m_smoothShades);
}

void McNfoView::zoomTo(int pointSize)
{
	QFont font = m_browser->font();
	font.setPointSize(qBound(kMinFontSize, pointSize, kMaxFontSize));
	m_browser->setFont(font);
	emit cellSizeChanged();
}

void McNfoView::showContextMenu(const QPoint& viewportPos)
{
	// The standard menu (Copy, Copy Link Location, Select All) takes document coordinates.
	const QPoint docPos = viewportPos + QPoint(m_browser->horizontalScrollBar()->value(),
	                                           m_browser->verticalScrollBar()->value());
	QMenu* menu = m_browser->createStandardContextMenu(docPos);
	menu->addSeparator();
	menu->addActions(m_viewActions);
	menu->exec(m_browser->viewport()->mapToGlobal(viewportPos));
	delete menu;
}

void McNfoView::setColors(Colors colors)
{
	m_colors = colors;
	const bool paper = colors == Colors::Paper;
	QPalette pal = m_browser->palette();
	pal.setColor(QPalette::Base, paper ? QColor(Qt::white) : QColor(Qt::black));
	pal.setColor(QPalette::Text, paper ? QColor(Qt::black) : QColor(0xD0, 0xD0, 0xD0));
	// The theme's link colors are picked for the theme's own background.
	pal.setColor(QPalette::Link, paper ? QColor(0x00, 0x55, 0xCC) : QColor(0x6C, 0xB6, 0xFF));
	pal.setColor(QPalette::LinkVisited, paper ? QColor(0x6A, 0x3D, 0xB8) : QColor(0xC4, 0xA5, 0xFF));
	m_browser->setPalette(pal);
	if (!m_text.isEmpty())
		render();
}

void McNfoView::setFontFamily(const QString& family)
{
	QFont font = m_browser->font();
	font.setFamily(family);
	m_browser->setFont(font);
	if (!m_text.isEmpty())
		render();
}

void McNfoView::setSmoothShades(bool smooth)
{
	m_smoothShades = smooth;
	static_cast<NfoBrowser*>(m_browser)->smoothShades = smooth;
	m_browser->viewport()->update();
}

QString McNfoView::fontFamily() const
{
	return m_browser->font().family();
}

void McNfoView::setText(const QString& nfoText)
{
	m_text = nfoText;
	++m_generation;
	m_images.clear();
	m_loadedUrls.clear();
	m_autoSize = QSize();
	// Parented replies die with the manager, aborting whatever is still in flight.
	delete m_net;
	m_net = nullptr;
	// Drops the previous text's image resources, which setHtml alone keeps.
	m_browser->document()->clear();

	render();
	m_browser->horizontalScrollBar()->setValue(0);
	m_browser->verticalScrollBar()->setValue(0);
	if (m_inlineImages)
		fetchImages();
}

void McNfoView::setInlineImagesEnabled(bool enabled)
{
	if (m_inlineImages == enabled) return;
	m_inlineImages = enabled;
	if (m_text.isEmpty()) return;
	if (enabled) {
		if (m_images.isEmpty())
			fetchImages();
		else
			m_loadedUrls = QSet<QString>(m_images.keyBegin(), m_images.keyEnd());
	} else {
		m_loadedUrls.clear();
	}
	render();
}

void McNfoView::render()
{
	const int hPos = m_browser->horizontalScrollBar()->value();
	const int vPos = m_browser->verticalScrollBar()->value();

	// Registered before setHtml so the document resolves each <img src> from
	// memory — QTextBrowser itself never loads http(s) resources.
	for (auto it = m_images.cbegin(); it != m_images.cend(); ++it)
		m_browser->document()->addResource(QTextDocument::ImageResource, QUrl(it.key()), it.value());

	// Links take their color from the application palette when the HTML is
	// parsed, not from the widget's, so the view has to style them itself.
	m_browser->document()->setDefaultStyleSheet(
		QStringLiteral("a { color: %1; }").arg(m_browser->palette().color(QPalette::Link).name()));

	// <pre> preserves whitespace/alignment for the art; bbcodeToHtml already
	// HTML-escaped everything else, so this is safe to feed straight in.
	// <pre> hardcodes its own font family, so the widget's has to be restated.
	m_browser->setHtml(QStringLiteral("<pre style=\"font-family:'%1'\">%2</pre>")
	                       .arg(fontFamily(), NfoText::bbcodeToHtml(m_text, &m_loadedUrls)));
	applyCellFormats();

	m_browser->horizontalScrollBar()->setValue(hPos);
	m_browser->verticalScrollBar()->setValue(vPos);
}

// Two per-character fixes on the freshly set document:
//  - Characters NfoBrowser paints itself get a transparent foreground, so the
//    font's own glyph (and its antialiased edge) doesn't show around the paint.
//  - Characters the font lacks come from a fallback font with a different
//    advance, which would shift the rest of the line off the grid; letter
//    spacing makes up the difference so every character is one cell wide.
void McNfoView::applyCellFormats()
{
	QTextDocument* doc = m_browser->document();
	const QFontMetricsF metrics(m_browser->font());
	const qreal cell = metrics.horizontalAdvance(QLatin1Char('0'));

	QTextCursor cursor(doc);
	cursor.beginEditBlock();
	for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
		const QString text = block.text();
		for (int i = 0; i < text.size();) {
			const QChar ch = text[i];
			const char16_t u = ch.unicode();
			if (u < 0x80 || ch.isSurrogate()) { ++i; continue; }
			const bool  painted = boxArms(u) || u == kInverseBullet || u == kLostCharacter
			                      || (u >= kFirstBlockElement && u <= kLastBlockElement);
			const qreal spacing = metrics.inFont(ch) ? 0.0 : cell - metrics.horizontalAdvance(ch);
			if (!painted && qFuzzyIsNull(spacing)) { ++i; continue; }

			// One format change per run of the same character (art is mostly runs).
			int end = i + 1;
			while (end < text.size() && text[end] == ch) ++end;
			QTextCharFormat format;
			if (painted)
				format.setForeground(Qt::transparent);
			if (!qFuzzyIsNull(spacing)) {
				format.setFontLetterSpacingType(QFont::AbsoluteSpacing);
				format.setFontLetterSpacing(spacing);
			}
			cursor.setPosition(block.position() + i);
			cursor.setPosition(block.position() + end, QTextCursor::KeepAnchor);
			cursor.mergeCharFormat(format);
			i = end;
		}
	}

	// DOS text mode cells are 5:12 (80x25 characters on a 4:3 screen), and NFO
	// art is drawn for that shape. Fonts like Consolas are wider for their
	// height, which stretches the art sideways; taller lines put it right. A
	// minimum, not a fixed height, so a line holding an inline image can grow.
	const qreal dosLineHeight = cell * kDosCellHeightPerWidth;
	if (dosLineHeight > metrics.lineSpacing()) {
		QTextBlockFormat blockFormat;
		blockFormat.setLineHeight(qRound(dosLineHeight), QTextBlockFormat::MinimumHeight);
		cursor.select(QTextCursor::Document);
		cursor.mergeBlockFormat(blockFormat);
	}
	cursor.endEditBlock();
}

bool McNfoView::eventFilter(QObject* watched, QEvent* event)
{
	// Letter spacing is in pixels, so any font change — including Ctrl+wheel
	// zoom, which the text widget does on its own — needs the formats redone.
	if (watched == m_browser && event->type() == QEvent::FontChange && !m_text.isEmpty())
		QMetaObject::invokeMethod(this, [this]() { render(); }, Qt::QueuedConnection);
	return QWidget::eventFilter(watched, event);
}

QSize McNfoView::contentSize() const
{
	QTextDocument* doc = m_browser->document();
	const qreal oldTextWidth = doc->textWidth();
	doc->setTextWidth(-1);   // unconstrained — measure the content's natural width
	const int idealW = qRound(doc->idealWidth());
	doc->setTextWidth(idealW);   // now measure the height that width needs
	const int idealH = qRound(doc->size().height());
	doc->setTextWidth(oldTextWidth);
	return { idealW, idealH };
}

void McNfoView::fitWindow(Fit mode, const QSize& chrome)
{
	QWidget* win = window();
	const QWidget* anchor = win->parentWidget() ? win->parentWidget()->window() : nullptr;
	const QScreen* scr    = anchor ? anchor->screen() : win->screen();
	if (!scr) return;
	// 50px margin on each edge.
	const QSize maxSize = scr->availableGeometry().size() - QSize(100, 100);

	const QSize content = contentSize();
	QSize target(qBound(400, content.width() + chrome.width(), maxSize.width()),
	             qBound(200, content.height() + chrome.height(), maxSize.height()));

	QPoint center;
	if (mode == Fit::GrowOnly) {
		if (win->size() != m_autoSize) return;
		target = target.expandedTo(win->size());
		if (target == win->size()) return;
		center = win->frameGeometry().center();
	} else if (anchor) {
		center = anchor->frameGeometry().center();
	} else if (win->isVisible()) {
		center = win->frameGeometry().center();
	} else {
		center = scr->availableGeometry().center();
	}

	win->resize(target);
	m_autoSize = target;
	QPoint topLeft = center - QPoint(target.width() / 2, target.height() / 2);
	const QRect avail = scr->availableGeometry();
	topLeft.setX(qBound(avail.left(), topLeft.x(), qMax(avail.left(), avail.right() - target.width())));
	topLeft.setY(qBound(avail.top(), topLeft.y(), qMax(avail.top(), avail.bottom() - target.height())));
	win->move(topLeft);
}

void McNfoView::fetchImages()
{
	const QStringList urls = NfoText::imageUrls(m_text).mid(0, kMaxImages);
	if (urls.isEmpty()) return;

	if (!m_net)
		m_net = new QNetworkAccessManager(this);
	const int generation = m_generation;
	for (const QString& urlStr : urls) {
		const QUrl url(urlStr);
		if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")))
			continue;

		QNetworkRequest req(url);
		req.setHeader(QNetworkRequest::UserAgentHeader, m_userAgent);
		req.setTransferTimeout(kTimeoutMs);
		QNetworkReply* reply = m_net->get(req);

		connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64 total) {
			if (received > kMaxImageBytes || total > kMaxImageBytes)
				reply->abort();
		});
		connect(reply, &QNetworkReply::finished, this, [this, reply, urlStr, generation]() {
			reply->deleteLater();
			if (generation != m_generation || reply->error() != QNetworkReply::NoError) return;

			// Dead image hosts commonly answer with an HTML page instead of
			// the image — that just fails to decode and stays a link.
			QImage img;
			if (!img.loadFromData(reply->readAll()) || img.isNull()) return;
			if (img.width() > m_maxImageSide || img.height() > m_maxImageSide)
				img = img.scaled(m_maxImageSide, m_maxImageSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);

			m_images.insert(urlStr, img);
			if (!m_inlineImages) return;
			m_loadedUrls.insert(urlStr);
			render();
			emit contentGrew();
		});
	}
}

} // namespace Mc
