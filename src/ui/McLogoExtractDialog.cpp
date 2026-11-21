#include "ui/McLogoExtractDialog.h"
#include "core/AppSettings.h"
#include "ui/McWindowGeometry.h"
#include "ui/SvgIcon.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <cstring>
#include <functional>
#include <numbers>

namespace Mc {

namespace {

constexpr int kPreviewMaxW = 1400;
constexpr int kPreviewMaxH = 700;
constexpr int kMaxUndo     = 40;

enum class Tool { Pick = 0, Keep = 1, Erase = 2 };
enum class PreviewBg { Checker = 0, Black = 1, White = 2, Grey = 3 };

// Largest rect of aspect `size` centred inside `box`.
QRectF fitRect(const QSize& size, const QRectF& box)
{
	if (size.isEmpty() || box.isEmpty()) return {};
	const double s = std::min(box.width() / size.width(), box.height() / size.height());
	const QSizeF  r(size.width() * s, size.height() * s);
	return QRectF(box.center() - QPointF(r.width() / 2, r.height() / 2), r);
}

// Only the cells inside `dirty` — brush strokes repaint small areas.
void paintChecker(QPainter& p, const QRectF& r, const QRect& dirty)
{
	const QRectF area = r.intersected(QRectF(dirty));
	if (area.isEmpty()) return;
	p.save();
	p.setClipRect(area);
	p.fillRect(area, QColor(0xcc, 0xcc, 0xcc));
	constexpr int cell = 10;
	const int ox = int(r.left()), oy = int(r.top());
	const int cx0 = (int(area.left()) - ox) / cell, cx1 = (int(std::ceil(area.right())) - ox) / cell;
	const int cy0 = (int(area.top()) - oy) / cell, cy1 = (int(std::ceil(area.bottom())) - oy) / cell;
	for (int cy = cy0; cy <= cy1; ++cy)
		for (int cx = cx0; cx <= cx1; ++cx)
			if ((cx + cy) % 2)
				p.fillRect(ox + cx * cell, oy + cy * cell, cell, cell, QColor(0x99, 0x99, 0x99));
	p.restore();
}

// Fills a disk of `value` into a Grayscale8 image.
void stampDisk(QImage& img, QPointF c, double r, uchar value)
{
	const int x0 = std::max(0, int(std::floor(c.x() - r)));
	const int x1 = std::min(img.width() - 1, int(std::ceil(c.x() + r)));
	const int y0 = std::max(0, int(std::floor(c.y() - r)));
	const int y1 = std::min(img.height() - 1, int(std::ceil(c.y() + r)));
	const double rr = r * r;
	for (int y = y0; y <= y1; ++y) {
		uchar* line = img.scanLine(y);
		for (int x = x0; x <= x1; ++x) {
			const double dx = x + 0.5 - c.x(), dy = y + 0.5 - c.y();
			if (dx * dx + dy * dy <= rr) line[x] = value;
		}
	}
}

// Fills a square of half-side r, rotated by angleDeg around c, into a Grayscale8 image.
void stampSquare(QImage& img, QPointF c, double r, double angleDeg, uchar value)
{
	const double rad = angleDeg * std::numbers::pi / 180.0;
	const double cs = std::cos(rad), sn = std::sin(rad);
	const double ext = r * (std::abs(cs) + std::abs(sn));
	const int x0 = std::max(0, int(std::floor(c.x() - ext)));
	const int x1 = std::min(img.width() - 1, int(std::ceil(c.x() + ext)));
	const int y0 = std::max(0, int(std::floor(c.y() - ext)));
	const int y1 = std::min(img.height() - 1, int(std::ceil(c.y() + ext)));
	for (int y = y0; y <= y1; ++y) {
		uchar* line = img.scanLine(y);
		const double dy = y + 0.5 - c.y();
		for (int x = x0; x <= x1; ++x) {
			const double dx = x + 0.5 - c.x();
			// Into the square's own axes.
			const double u = dx * cs + dy * sn, v = -dx * sn + dy * cs;
			if (std::abs(u) <= r && std::abs(v) <= r) line[x] = value;
		}
	}
}

QString safeFileName(QString s)
{
	static const QRegularExpression bad(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])"));
	s.replace(bad, QStringLiteral(" "));
	return s.simplified();
}

// ── Painted tool icons ────────────────────────────────────────────────────────
// Drawn rather than shipped as SVGs so they can carry a second accent colour
// (green keep / red erase) and still follow the theme's text colour.

QPixmap toolPixmap(Tool t, const QColor& fg, qreal dpr)
{
	constexpr int kSize = 28;
	QPixmap pm(QSize(kSize, kSize) * dpr);
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.scale(kSize / 24.0, kSize / 24.0);  // draw on a 24-unit grid
	const QColor keep(0x3c, 0xc8, 0x5a), erase(0xf0, 0x55, 0x55);
	QPen pen(fg, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);

	if (t == Tool::Pick) {
		// Eyedropper: squeeze bulb top-right, glass tube down-left, a drop at the tip.
		p.setPen(Qt::NoPen);
		p.setBrush(fg);
		p.save();
		p.translate(17.5, 6.5);
		p.rotate(45);
		p.drawRoundedRect(QRectF(-2.6, -4.5, 5.2, 7.0), 2.6, 2.6);  // bulb
		p.drawRect(QRectF(-4.2, 2.0, 8.4, 1.8));                    // collar
		p.restore();
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		p.drawLine(QPointF(14.5, 9.5), QPointF(5.5, 18.5));          // tube
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0x3d, 0xae, 0xe9));
		p.drawEllipse(QPointF(3.4, 21.0), 1.9, 1.9);
		return pm;
	}
	if (t == Tool::Keep) {
		// Paint brush with a green "+" badge.
		p.save();
		p.translate(12, 12);
		p.rotate(45);
		p.setPen(Qt::NoPen);
		p.setBrush(fg);
		p.drawRoundedRect(QRectF(-1.4, -11.0, 2.8, 9.0), 1.4, 1.4);  // handle
		p.drawRect(QRectF(-2.4, -2.6, 4.8, 3.0));                    // ferrule
		QPainterPath tip;                                             // bristles
		tip.moveTo(-2.6, 0.6);
		tip.lineTo(2.6, 0.6);
		tip.cubicTo(2.8, 4.5, 1.0, 7.5, 0, 9.5);
		tip.cubicTo(-1.0, 7.5, -2.8, 4.5, -2.6, 0.6);
		p.setBrush(keep);
		p.drawPath(tip);
		p.restore();
		p.setPen(Qt::NoPen);
		p.setBrush(keep);
		p.drawEllipse(QPointF(18.5, 18.5), 5.0, 5.0);
		p.setPen(QPen(Qt::white, 1.8, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(18.5, 16.0), QPointF(18.5, 21.0));
		p.drawLine(QPointF(16.0, 18.5), QPointF(21.0, 18.5));
		return pm;
	}
	// Eraser: a tilted two-tone block over a baseline.
	p.save();
	p.translate(12.5, 11.0);
	p.rotate(-40);
	p.setPen(QPen(fg, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	p.setBrush(erase);
	p.drawRoundedRect(QRectF(-8.5, -4.0, 7.5, 8.0), 1.6, 1.6);   // rubber end
	p.setBrush(Qt::NoBrush);
	p.drawRoundedRect(QRectF(-8.5, -4.0, 17.0, 8.0), 1.6, 1.6);  // body outline
	p.restore();
	p.setPen(QPen(fg, 1.7, Qt::SolidLine, Qt::RoundCap));
	p.drawLine(QPointF(8.0, 21.5), QPointF(21.5, 21.5));
	return pm;
}

QIcon toolIcon(Tool t, const QPalette& pal, qreal dpr)
{
	QIcon icon;
	icon.addPixmap(toolPixmap(t, pal.color(QPalette::ButtonText), dpr), QIcon::Normal, QIcon::Off);
	icon.addPixmap(toolPixmap(t, pal.color(QPalette::HighlightedText), dpr), QIcon::Normal, QIcon::On);
	icon.addPixmap(toolPixmap(t, pal.color(QPalette::Disabled, QPalette::ButtonText), dpr), QIcon::Disabled, QIcon::Off);
	return icon;
}

// Round swatch for a preview-background toggle.
QIcon previewBgIcon(PreviewBg bg, qreal dpr)
{
	constexpr int kSize = 18;
	QPixmap pm(QSize(kSize, kSize) * dpr);
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	QPainterPath circle;
	circle.addEllipse(QRectF(1, 1, kSize - 2, kSize - 2));
	p.setClipPath(circle);
	switch (bg) {
	case PreviewBg::Checker:
		p.fillRect(QRectF(0, 0, kSize, kSize), QColor(0xcc, 0xcc, 0xcc));
		for (int y = 0; y < 3; ++y)
			for (int x = 0; x < 3; ++x)
				if ((x + y) % 2) p.fillRect(QRectF(x * 6.0, y * 6.0, 6.0, 6.0), QColor(0x88, 0x88, 0x88));
		break;
	case PreviewBg::Black: p.fillRect(QRectF(0, 0, kSize, kSize), Qt::black); break;
	case PreviewBg::White: p.fillRect(QRectF(0, 0, kSize, kSize), Qt::white); break;
	case PreviewBg::Grey:  p.fillRect(QRectF(0, 0, kSize, kSize), QColor(0x55, 0x55, 0x55)); break;
	}
	p.setClipping(false);
	p.setPen(QPen(QColor(0x80, 0x80, 0x80), 1));
	p.setBrush(Qt::NoBrush);
	p.drawEllipse(QRectF(1, 1, kSize - 2, kSize - 2));
	return QIcon(pm);
}

// Bold, dimmed caption at the top of a control card.
QLabel* cardTitle(const QString& text, QWidget* parent)
{
	auto* l = new QLabel(text.toUpper(), parent);
	QFont f = l->font();
	f.setBold(true);
	f.setPointSizeF(f.pointSizeF() * 0.82);
	f.setLetterSpacing(QFont::PercentageSpacing, 108);
	l->setFont(f);
	QPalette pal = l->palette();
	pal.setColor(QPalette::WindowText, pal.color(QPalette::PlaceholderText));
	l->setPalette(pal);
	return l;
}

// Rounded, outlined panel grouping related controls.
QFrame* makeCard(QWidget* parent)
{
	auto* f = new QFrame(parent);
	f->setObjectName(QStringLiteral("mcLogoCard"));
	return f;
}

} // namespace

namespace LogoExtractUi {

// Live preview of the brush size next to its slider.
class BrushDot : public QWidget {
public:
	explicit BrushDot(QWidget* parent) : QWidget(parent) { setFixedSize(34, 34); }
	void setRadius(int r) { m_r = r; update(); }
	void setShape(bool square, int angleDeg)
	{
		m_square = square;
		m_angle = angleDeg;
		update();
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		// A rotated square reaches ~1.41x further — keep it inside the widget.
		const double r = std::min(m_square ? 10.5 : 15.0, 1.5 + m_r * 0.2);
		const QColor c = palette().color(QPalette::ButtonText);
		p.translate(QRectF(rect()).center());
		if (m_square) p.rotate(m_angle);
		const QRectF shape(-r, -r, 2 * r, 2 * r);
		p.setPen(QPen(c, 1.2));
		p.setBrush(QColor(c.red(), c.green(), c.blue(), 60));
		if (m_square) p.drawRect(shape);
		else          p.drawEllipse(shape);
	}

private:
	int  m_r = 12;
	bool m_square = false;
	int  m_angle = 0;
};

// Small round/square glyph for the brush-shape toggle.
QIcon brushShapeIcon(bool square, const QPalette& pal, qreal dpr)
{
	QIcon icon;
	for (const auto& [state, role] : {std::pair{QIcon::Off, QPalette::ButtonText}, std::pair{QIcon::On, QPalette::HighlightedText}}) {
		QPixmap pm(QSize(16, 16) * dpr);
		pm.setDevicePixelRatio(dpr);
		pm.fill(Qt::transparent);
		QPainter p(&pm);
		p.setRenderHint(QPainter::Antialiasing);
		const QColor c = pal.color(role);
		p.setPen(QPen(c, 1.4));
		p.setBrush(QColor(c.red(), c.green(), c.blue(), 90));
		if (square) p.drawRect(QRectF(2.5, 2.5, 11, 11));
		else        p.drawEllipse(QRectF(2, 2, 12, 12));
		p.end();
		icon.addPixmap(pm, QIcon::Normal, state);
	}
	return icon;
}

} // namespace LogoExtractUi

namespace {

} // namespace

namespace LogoExtractUi {

// Full artwork; drag to choose the area to cut from. The box stays editable:
// drag an edge or corner to resize it, inside it to move it, or use the arrow
// keys (1 px, Shift = 10 px; Ctrl+arrows resize from the bottom-right). The
// mouse wheel zooms around the cursor, right/middle-drag pans, and a
// double-click fits the whole image again.
class SourceView : public QWidget {
public:
	// live: true while a drag/key-repeat is still going (the dialog throttles
	// those), false once the change is final (mouse released).
	std::function<void(QRect, bool)> onSelection;

	explicit SourceView(QWidget* parent) : QWidget(parent)
	{
		setMinimumSize(240, 240);
		setMouseTracking(true);
		setFocusPolicy(Qt::StrongFocus);
		setCursor(Qt::CrossCursor);
	}
	void setImage(const QImage& img)
	{
		m_img = img;
		m_scaled = {};
		m_sel = {};
		m_message.clear();
		resetView();
	}
	void setMessage(const QString& msg)
	{
		m_message = msg;
		update();
	}

protected:
	enum class Hit { None, New, Move, L, R, T, B, TL, TR, BL, BR };

	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		p.fillRect(rect(), QColor(0x20, 0x20, 0x20));
		if (m_img.isNull()) {
			p.setPen(QColor(0xd0, 0xd0, 0xd0));
			p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, m_message);
			return;
		}
		const QRectF ir = imageRect();
		if (m_zoom <= 1.0001) {
			// Fitted: one cached, pre-scaled pixmap.
			const qreal dpr  = devicePixelRatioF();
			const QSize want = (ir.size() * dpr).toSize();
			if (m_scaled.isNull() || m_scaled.size() != want) {
				m_scaled = QPixmap::fromImage(m_img.scaled(want, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
				m_scaled.setDevicePixelRatio(dpr);
			}
			p.drawPixmap(ir.topLeft(), m_scaled);
		} else {
			// Zoomed: let the painter scale just the visible part.
			p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom < 4.0);
			p.drawImage(ir, m_img);
		}
		if (!m_sel.isNull()) {
			const QRectF s = toWidget(m_sel);
			QPainterPath dim;
			dim.addRect(ir);
			dim.addRect(s);
			p.fillPath(dim, QColor(0, 0, 0, 140));
			const QColor accent(0x3d, 0xae, 0xe9);
			p.setPen(QPen(accent, 1.5));
			p.setBrush(Qt::NoBrush);
			p.drawRect(s);
			// Resize handles: corners and edge midpoints.
			p.setPen(QPen(Qt::black, 1));
			p.setBrush(accent);
			const QPointF c = s.center();
			for (const QPointF& h : {s.topLeft(), s.topRight(), s.bottomLeft(), s.bottomRight(),
			                         QPointF(c.x(), s.top()), QPointF(c.x(), s.bottom()),
			                         QPointF(s.left(), c.y()), QPointF(s.right(), c.y())})
				p.drawRect(QRectF(h - QPointF(3.5, 3.5), QSizeF(7, 7)));
		}
		// Controls hint, bottom-left.
		QFont f = font();
		f.setPointSizeF(f.pointSizeF() * 0.85);
		p.setFont(f);
		const QString hint = m_zoom > 1.0001
			? tr("Scroll to zoom · right-drag to pan · double-click to fit")
			: tr("Scroll to zoom · arrow keys nudge the box");
		const QRect hr = QFontMetrics(f).boundingRect(hint).adjusted(-6, -3, 6, 3);
		const QRect box(QPoint(8, height() - hr.height() - 8), hr.size());
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 150));
		p.drawRoundedRect(box, 4, 4);
		p.setPen(QColor(0xe0, 0xe0, 0xe0));
		p.drawText(box, Qt::AlignCenter, hint);
	}
	void resizeEvent(QResizeEvent*) override
	{
		m_scaled = {};
		clampCenter();
	}
	void mousePressEvent(QMouseEvent* e) override
	{
		if (m_img.isNull()) return;
		if (e->button() == Qt::RightButton || e->button() == Qt::MiddleButton) {
			m_panning   = true;
			m_panFrom   = e->position();
			m_panCenter = m_center;
			setCursor(Qt::ClosedHandCursor);
			return;
		}
		if (e->button() != Qt::LeftButton) return;
		m_drag       = hitTest(e->position());
		m_selAtPress = m_sel;
		m_dragFrom   = toImageF(e->position());
		m_firedLive  = false;
		if (m_drag == Hit::None) {
			m_drag    = Hit::New;
			m_newFrom = toImageClamped(e->position());
		}
	}
	void mouseMoveEvent(QMouseEvent* e) override
	{
		if (m_panning) {
			m_center = m_panCenter - (e->position() - m_panFrom) / scale();
			clampCenter();
			update();
			return;
		}
		if (m_drag == Hit::None) {
			setCursor(cursorFor(hitTest(e->position())));
			return;
		}
		const QRect r = draggedRect(e->position());
		if (r == m_sel) return;
		m_sel = r;
		update();
		if (validSize(r) && onSelection) {
			m_firedLive = true;
			onSelection(r, true);
		}
	}
	void mouseReleaseEvent(QMouseEvent* e) override
	{
		if (m_panning && (e->button() == Qt::RightButton || e->button() == Qt::MiddleButton)) {
			m_panning = false;
			setCursor(cursorFor(hitTest(e->position())));
			return;
		}
		if (e->button() != Qt::LeftButton || m_drag == Hit::None) return;
		const Hit drag = m_drag;
		m_drag = Hit::None;
		// A click or tiny drag outside the box keeps the box you had.
		if (!validSize(m_sel) || (drag == Hit::New && !validSize(m_sel)))
			m_sel = m_selAtPress;
		update();
		if (!m_sel.isNull() && (m_sel != m_selAtPress || m_firedLive) && onSelection)
			onSelection(m_sel, false);
		setCursor(cursorFor(hitTest(e->position())));
	}
	void mouseDoubleClickEvent(QMouseEvent* e) override
	{
		if (e->button() == Qt::LeftButton) resetView();
	}
	void wheelEvent(QWheelEvent* e) override
	{
		if (m_img.isNull()) return;
		const double steps = e->angleDelta().y() / 120.0;
		if (steps == 0) return;
		const QPointF anchorImg = toImageF(e->position());
		m_zoom = qBound(1.0, m_zoom * std::pow(1.25, steps), 32.0);
		// Keep the image point under the cursor where it is.
		m_center = anchorImg - (e->position() - QRectF(rect()).center()) / scale();
		clampCenter();
		update();
		e->accept();
	}
	void keyPressEvent(QKeyEvent* e) override
	{
		if (m_sel.isNull()) {
			QWidget::keyPressEvent(e);
			return;
		}
		const int step = e->modifiers().testFlag(Qt::ShiftModifier) ? 10 : 1;
		const bool resize = e->modifiers().testFlag(Qt::ControlModifier);
		int dx = 0, dy = 0;
		switch (e->key()) {
		case Qt::Key_Left:  dx = -step; break;
		case Qt::Key_Right: dx = step; break;
		case Qt::Key_Up:    dy = -step; break;
		case Qt::Key_Down:  dy = step; break;
		default: QWidget::keyPressEvent(e); return;
		}
		QRect r = m_sel;
		if (resize) {
			r.setRight(qBound(r.left() + kMinSide - 1, r.right() + dx, m_img.width() - 1));
			r.setBottom(qBound(r.top() + kMinSide - 1, r.bottom() + dy, m_img.height() - 1));
		} else {
			r = keepInside(r.translated(dx, dy));
		}
		if (r == m_sel) return;
		m_sel = r;
		update();
		// Auto-repeat keeps coming: let the dialog throttle like a drag.
		if (onSelection) onSelection(r, e->isAutoRepeat());
		if (e->isAutoRepeat()) m_keyRepeatPending = true;
	}
	void keyReleaseEvent(QKeyEvent* e) override
	{
		if (!e->isAutoRepeat() && m_keyRepeatPending) {
			m_keyRepeatPending = false;
			if (!m_sel.isNull() && onSelection) onSelection(m_sel, false);
		}
		QWidget::keyReleaseEvent(e);
	}

private:
	static constexpr int kMinSide = 8;

	[[nodiscard]] static bool validSize(const QRect& r) { return r.width() >= kMinSide && r.height() >= kMinSide; }

	[[nodiscard]] double fitScale() const
	{
		if (m_img.isNull()) return 1.0;
		const QRectF box = QRectF(rect()).adjusted(6, 6, -6, -6);
		return std::max(1e-6, std::min(box.width() / m_img.width(), box.height() / m_img.height()));
	}
	[[nodiscard]] double scale() const { return fitScale() * m_zoom; }
	[[nodiscard]] QPointF toWidgetF(QPointF img) const { return QRectF(rect()).center() + (img - m_center) * scale(); }
	[[nodiscard]] QPointF toImageF(QPointF w) const { return m_center + (w - QRectF(rect()).center()) / scale(); }
	[[nodiscard]] QPoint toImageClamped(QPointF w) const
	{
		const QPointF f = toImageF(w);
		return QPoint(qBound(0, int(std::floor(f.x())), m_img.width() - 1),
		              qBound(0, int(std::floor(f.y())), m_img.height() - 1));
	}
	[[nodiscard]] QRectF imageRect() const
	{
		return QRectF(toWidgetF(QPointF(0, 0)), toWidgetF(QPointF(m_img.width(), m_img.height())));
	}
	[[nodiscard]] QRectF toWidget(const QRect& r) const
	{
		return QRectF(toWidgetF(QPointF(r.x(), r.y())), toWidgetF(QPointF(r.x() + r.width(), r.y() + r.height())));
	}
	void resetView()
	{
		m_zoom = 1.0;
		m_center = QPointF(m_img.width() / 2.0, m_img.height() / 2.0);
		m_scaled = {};
		update();
	}
	void clampCenter()
	{
		if (m_zoom <= 1.0001) {
			m_center = QPointF(m_img.width() / 2.0, m_img.height() / 2.0);
			return;
		}
		m_center.setX(qBound(0.0, m_center.x(), double(m_img.width())));
		m_center.setY(qBound(0.0, m_center.y(), double(m_img.height())));
	}
	[[nodiscard]] QRect keepInside(QRect r) const
	{
		if (r.left() < 0) r.translate(-r.left(), 0);
		if (r.top() < 0) r.translate(0, -r.top());
		if (r.right() > m_img.width() - 1) r.translate(m_img.width() - 1 - r.right(), 0);
		if (r.bottom() > m_img.height() - 1) r.translate(0, m_img.height() - 1 - r.bottom());
		return r;
	}
	[[nodiscard]] Hit hitTest(QPointF w) const
	{
		if (m_sel.isNull() || m_img.isNull()) return Hit::None;
		const QRectF s = toWidget(m_sel);
		constexpr double m = 6.0;
		const bool inY  = w.y() >= s.top() - m && w.y() <= s.bottom() + m;
		const bool inX  = w.x() >= s.left() - m && w.x() <= s.right() + m;
		const bool nearL = inY && std::abs(w.x() - s.left()) <= m;
		const bool nearR = inY && std::abs(w.x() - s.right()) <= m;
		const bool nearT = inX && std::abs(w.y() - s.top()) <= m;
		const bool nearB = inX && std::abs(w.y() - s.bottom()) <= m;
		if (nearT && nearL) return Hit::TL;
		if (nearT && nearR) return Hit::TR;
		if (nearB && nearL) return Hit::BL;
		if (nearB && nearR) return Hit::BR;
		if (nearL) return Hit::L;
		if (nearR) return Hit::R;
		if (nearT) return Hit::T;
		if (nearB) return Hit::B;
		if (s.contains(w)) return Hit::Move;
		return Hit::None;
	}
	[[nodiscard]] static Qt::CursorShape cursorFor(Hit h)
	{
		switch (h) {
		case Hit::TL: case Hit::BR: return Qt::SizeFDiagCursor;
		case Hit::TR: case Hit::BL: return Qt::SizeBDiagCursor;
		case Hit::L:  case Hit::R:  return Qt::SizeHorCursor;
		case Hit::T:  case Hit::B:  return Qt::SizeVerCursor;
		case Hit::Move:             return Qt::SizeAllCursor;
		default:                    return Qt::CrossCursor;
		}
	}
	// The box for the current drag position, clamped to the image and to the
	// minimum size (edges can't cross each other).
	[[nodiscard]] QRect draggedRect(QPointF pos) const
	{
		if (m_drag == Hit::New)
			return QRect(m_newFrom, toImageClamped(pos)).normalized();
		const QPointF d = toImageF(pos) - m_dragFrom;
		const int dx = qRound(d.x()), dy = qRound(d.y());
		const QRect a = m_selAtPress;
		if (m_drag == Hit::Move) return keepInside(a.translated(dx, dy));
		QRect r = a;
		const int maxX = m_img.width() - 1, maxY = m_img.height() - 1;
		if (m_drag == Hit::L || m_drag == Hit::TL || m_drag == Hit::BL)
			r.setLeft(qBound(0, a.left() + dx, a.right() - kMinSide + 1));
		if (m_drag == Hit::R || m_drag == Hit::TR || m_drag == Hit::BR)
			r.setRight(qBound(a.left() + kMinSide - 1, a.right() + dx, maxX));
		if (m_drag == Hit::T || m_drag == Hit::TL || m_drag == Hit::TR)
			r.setTop(qBound(0, a.top() + dy, a.bottom() - kMinSide + 1));
		if (m_drag == Hit::B || m_drag == Hit::BL || m_drag == Hit::BR)
			r.setBottom(qBound(a.top() + kMinSide - 1, a.bottom() + dy, maxY));
		return r;
	}

	QImage  m_img;
	QPixmap m_scaled;
	QRect   m_sel;
	QString m_message;
	double  m_zoom = 1.0;
	QPointF m_center;
	// Selection drag
	Hit     m_drag = Hit::None;
	QRect   m_selAtPress;
	QPointF m_dragFrom;
	QPoint  m_newFrom;
	bool    m_firedLive = false;
	bool    m_keyRepeatPending = false;
	// Panning
	bool    m_panning = false;
	QPointF m_panFrom;
	QPointF m_panCenter;
};

// The cut-out (or the original crop) over a choice of backgrounds; click to
// pick colours, drag to paint keep/erase overrides. The mouse wheel zooms
// around the cursor (past ~3 screen px per image px it shows crisp pixels),
// right/middle-drag pans. Coordinates passed to the callbacks are in the
// displayed (preview) image's pixels; the brush size is in screen pixels, so
// zooming in makes it finer on the image.
class ResultView : public QWidget {
public:
	std::function<void(QPoint, bool)>      onPick;
	std::function<void()>                  onStrokeBegin;
	std::function<void(QPoint, QPoint, double)> onStroke;
	std::function<void()>                  onStrokeEnd;
	std::function<void(double)>            onZoomChanged;  // 1 = fit

	explicit ResultView(QWidget* parent) : QWidget(parent)
	{
		setMinimumSize(320, 160);
		setMouseTracking(true);
	}
	void setImages(const QImage& result, const QImage& original)
	{
		const QSize before = shownSize();
		m_result = result;
		m_original = original;
		if (shownSize() != before) resetView();  // a new crop; recomputes keep the zoom
		update();
	}
	// The displayed result, edited in place by brush strokes (the view holds
	// the only reference, so no copy-on-write per stroke)...
	[[nodiscard]] QImage& result() { return m_result; }
	// ...followed by a repaint of just the changed part (image pixels).
	void resultChanged(const QRect& imageRect)
	{
		if (m_result.isNull()) return;
		const QPointF a = toWidgetF(QPointF(imageRect.topLeft()));
		const QPointF b = toWidgetF(QPointF(imageRect.x() + imageRect.width(), imageRect.y() + imageRect.height()));
		update(QRectF(a, b).normalized().toAlignedRect().adjusted(-2, -2, 2, 2));
	}
	void setMessage(const QString& msg)
	{
		m_message = msg;
		update();
	}
	void setShowOriginal(bool on) { m_showOriginal = on; update(); }
	void setBackground(PreviewBg bg) { m_bg = bg; update(); }
	void setTool(Tool t)
	{
		m_tool = t;
		setCursor(t == Tool::Pick ? Qt::CrossCursor : Qt::BlankCursor);
		update();
	}
	void setBrushRadius(int r) { m_brushRadius = r; update(); }
	void setBrushShape(bool square, int angleDeg)
	{
		m_square = square;
		m_angle = angleDeg;
		update();
	}
	void resetView()
	{
		m_zoom = 1.0;
		const QSize s = shownSize();
		m_center = QPointF(s.width() / 2.0, s.height() / 2.0);
		update();
		if (onZoomChanged) onZoomChanged(m_zoom);
	}
	// Zoom by factor around a widget point (the view centre if null).
	void zoomBy(double factor, QPointF around = QPointF(-1, -1))
	{
		if (shownSize().isEmpty()) return;
		if (around.x() < 0) around = QRectF(rect()).center();
		const QPointF anchor = toImageF(around);
		m_zoom = qBound(1.0, m_zoom * factor, 32.0);
		m_center = anchor - (around - QRectF(rect()).center()) / viewScale();
		clampCenter();
		update();
		if (onZoomChanged) onZoomChanged(m_zoom);
	}

protected:
	void paintEvent(QPaintEvent* e) override
	{
		QPainter p(this);
		p.setClipRect(e->rect());
		p.fillRect(e->rect(), palette().window());
		const QImage& img = m_showOriginal ? m_original : m_result;
		if (img.isNull()) {
			p.setPen(palette().text().color());
			p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, m_message);
			return;
		}
		const QRectF ir = imageRect();
		const QRectF visible = ir.intersected(QRectF(rect()));
		switch (m_bg) {
		case PreviewBg::Checker: paintChecker(p, visible, e->rect()); break;
		case PreviewBg::Black:   p.fillRect(visible, Qt::black); break;
		case PreviewBg::White:   p.fillRect(visible, Qt::white); break;
		case PreviewBg::Grey:    p.fillRect(visible, QColor(0x55, 0x55, 0x55)); break;
		}
		// Zoomed in far enough to judge single pixels: show them crisp.
		p.setRenderHint(QPainter::SmoothPixmapTransform, viewScale() < 3.0);
		p.drawImage(ir, img);
		if (m_tool != Tool::Pick && m_hover && !m_panning && rect().contains(m_hoverPos.toPoint())) {
			// Brush cursor: tinted shape of exactly the area it will paint, a
			// dark+coloured double outline so it shows on any background, and
			// a small crosshair at the centre.
			const QColor c = m_tool == Tool::Keep ? QColor(0x3c, 0xc8, 0x5a) : QColor(0xf0, 0x55, 0x55);
			const double r = m_brushRadius;
			p.setRenderHint(QPainter::Antialiasing);
			p.save();
			p.translate(m_hoverPos);
			if (m_square) p.rotate(m_angle);
			const QRectF shape(-r, -r, 2 * r, 2 * r);
			const auto draw = [&](const QPen& pen, const QBrush& brush) {
				p.setPen(pen);
				p.setBrush(brush);
				if (m_square) p.drawRect(shape);
				else          p.drawEllipse(shape);
			};
			draw(Qt::NoPen, QColor(c.red(), c.green(), c.blue(), 55));
			draw(QPen(QColor(0, 0, 0, 170), 3), Qt::NoBrush);
			draw(QPen(c, 1.5), Qt::NoBrush);
			const double t = std::min(4.0, r * 0.5);
			p.setPen(QPen(c, 1.2));
			p.drawLine(QPointF(-t, 0), QPointF(t, 0));
			p.drawLine(QPointF(0, -t), QPointF(0, t));
			p.restore();
		}
		// Controls hint, bottom-left (same style as the source view's).
		QFont f = font();
		f.setPointSizeF(f.pointSizeF() * 0.85);
		p.setFont(f);
		const QString hint = m_zoom > 1.0001 ? tr("Scroll to zoom \u00b7 right-drag to pan \u00b7 Ctrl+0 to fit")
		                                     : tr("Scroll to zoom in for detail");
		const QRect hr = QFontMetrics(f).boundingRect(hint).adjusted(-6, -3, 6, 3);
		const QRect box(QPoint(8, height() - hr.height() - 8), hr.size());
		if (box.intersects(e->rect())) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(0, 0, 0, 150));
			p.drawRoundedRect(box, 4, 4);
			p.setPen(QColor(0xe0, 0xe0, 0xe0));
			p.drawText(box, Qt::AlignCenter, hint);
		}
	}
	void resizeEvent(QResizeEvent*) override { clampCenter(); }
	void mousePressEvent(QMouseEvent* e) override
	{
		if (m_result.isNull()) return;
		if (e->button() == Qt::RightButton || e->button() == Qt::MiddleButton) {
			m_panning   = true;
			m_panFrom   = e->position();
			m_panCenter = m_center;
			setCursor(Qt::ClosedHandCursor);
			update(cursorRect(m_hoverPos));
			return;
		}
		if (e->button() != Qt::LeftButton) return;
		const QPoint ip = toImage(e->position());
		if (m_tool == Tool::Pick) {
			if (onPick && QRect(QPoint(0, 0), m_result.size()).contains(ip))
				onPick(ip, e->modifiers().testFlag(Qt::ShiftModifier));
			return;
		}
		m_painting = true;
		m_last = ip;
		if (onStrokeBegin) onStrokeBegin();
		if (onStroke) onStroke(ip, ip, imageRadius());
	}
	void mouseMoveEvent(QMouseEvent* e) override
	{
		if (m_panning) {
			m_center = m_panCenter - (e->position() - m_panFrom) / viewScale();
			clampCenter();
			update();
			return;
		}
		const QPointF old = m_hoverPos;
		const bool    wasHover = m_hover;
		m_hover = true;
		m_hoverPos = e->position();
		if (m_painting) {
			const QPoint ip = toImage(e->position());
			if (onStroke) onStroke(m_last, ip, imageRadius());
			m_last = ip;
		}
		// Only the brush outline moved: repaint its old and new spot.
		if (m_tool != Tool::Pick) {
			if (wasHover) update(cursorRect(old));
			update(cursorRect(m_hoverPos));
		}
	}
	void mouseReleaseEvent(QMouseEvent* e) override
	{
		if (m_panning && (e->button() == Qt::RightButton || e->button() == Qt::MiddleButton)) {
			m_panning = false;
			setCursor(m_tool == Tool::Pick ? Qt::CrossCursor : Qt::BlankCursor);
			update(cursorRect(m_hoverPos));
			return;
		}
		if (!m_painting || e->button() != Qt::LeftButton) return;
		m_painting = false;
		if (onStrokeEnd) onStrokeEnd();
	}
	void wheelEvent(QWheelEvent* e) override
	{
		const double steps = e->angleDelta().y() / 120.0;
		if (steps == 0 || m_result.isNull()) return;
		zoomBy(std::pow(1.25, steps), e->position());
		m_hoverPos = e->position();
		e->accept();
	}
	void leaveEvent(QEvent*) override
	{
		m_hover = false;
		update(cursorRect(m_hoverPos));
	}

private:
	[[nodiscard]] QSize shownSize() const { return m_result.isNull() ? m_original.size() : m_result.size(); }
	[[nodiscard]] double fitScale() const
	{
		const QSize s = shownSize();
		if (s.isEmpty()) return 1.0;
		const QRectF box = QRectF(rect()).adjusted(8, 8, -8, -8);
		return std::max(1e-6, std::min(box.width() / s.width(), box.height() / s.height()));
	}
	// Screen pixels per image pixel.
	[[nodiscard]] double viewScale() const { return fitScale() * m_zoom; }
	[[nodiscard]] QPointF toWidgetF(QPointF img) const { return QRectF(rect()).center() + (img - m_center) * viewScale(); }
	[[nodiscard]] QPointF toImageF(QPointF w) const { return m_center + (w - QRectF(rect()).center()) / viewScale(); }
	[[nodiscard]] QPoint toImage(QPointF w) const
	{
		const QPointF f = toImageF(w);
		return QPoint(int(std::floor(f.x())), int(std::floor(f.y())));
	}
	[[nodiscard]] QRectF imageRect() const
	{
		const QSize s = shownSize();
		return QRectF(toWidgetF(QPointF(0, 0)), toWidgetF(QPointF(s.width(), s.height())));
	}
	void clampCenter()
	{
		const QSize s = shownSize();
		if (m_zoom <= 1.0001) {
			m_center = QPointF(s.width() / 2.0, s.height() / 2.0);
			return;
		}
		m_center.setX(qBound(0.0, m_center.x(), double(s.width())));
		m_center.setY(qBound(0.0, m_center.y(), double(s.height())));
	}
	// Brush radius in image pixels: screen-sized, so finer when zoomed in.
	[[nodiscard]] double imageRadius() const { return std::max(0.5, m_brushRadius / viewScale()); }
	[[nodiscard]] QRect cursorRect(QPointF c) const
	{
		const int r = int(std::ceil(m_brushRadius * (m_square ? 1.42 : 1.0))) + 3;
		return QRect(int(c.x()) - r, int(c.y()) - r, 2 * r + 2, 2 * r + 2);
	}

	QImage    m_result;
	QImage    m_original;
	QString   m_message;
	bool      m_showOriginal = false;
	PreviewBg m_bg = PreviewBg::Checker;
	Tool      m_tool = Tool::Pick;
	int       m_brushRadius = 12;
	bool      m_square = false;
	int       m_angle = 0;
	bool      m_painting = false;
	bool      m_hover = false;
	QPointF   m_hoverPos;
	QPoint    m_last;
	double    m_zoom = 1.0;
	QPointF   m_center;
	bool      m_panning = false;
	QPointF   m_panFrom;
	QPointF   m_panCenter;
};

} // namespace LogoExtractUi

using namespace LogoExtractUi;

McLogoExtractDialog::McLogoExtractDialog(const QString& movieLabel, QNetworkAccessManager* nam, QWidget* parent)
	: QDialog(parent, Qt::Dialog)
	, m_movieLabel(movieLabel)
	, m_nam(nam)
{
	setWindowTitle(movieLabel.isEmpty() ? tr("Extract Clear Logo") : tr("Extract Clear Logo — %1").arg(movieLabel));
	setSizeGripEnabled(true);
	setMinimumSize(clampSizeToScreen(this, QSize(860, 560)));

	auto* main = new QVBoxLayout(this);
	main->setContentsMargins(12, 12, 12, 12);
	main->setSpacing(10);

	setStyleSheet(QStringLiteral(
		"QFrame#mcLogoCard { border: 1px solid palette(mid); border-radius: 8px; }"
		// Segmented mode switch
		"QToolButton[mcSeg=\"true\"] { border: 1px solid palette(mid); background: palette(button);"
		"  padding: 5px 12px; }"
		"QToolButton[mcSeg=\"true\"]:hover { background: palette(midlight); }"
		"QToolButton[mcSeg=\"true\"]:checked { background: palette(highlight); color: palette(highlighted-text);"
		"  border-color: palette(highlight); }"
		"QToolButton[mcSegPos=\"left\"] { border-top-left-radius: 5px; border-bottom-left-radius: 5px; }"
		"QToolButton[mcSegPos=\"right\"] { border-top-right-radius: 5px; border-bottom-right-radius: 5px;"
		"  border-left: none; }"
		// Tool palette
		"QToolButton[mcTool=\"true\"] { border: 1px solid transparent; border-radius: 7px; padding: 4px 8px; }"
		"QToolButton[mcTool=\"true\"]:hover { border-color: palette(mid); }"
		"QToolButton[mcTool=\"true\"]:checked { background: palette(highlight); color: palette(highlighted-text); }"
		// Preview background swatches / small toggles over the result
		"QToolButton[mcSwatch=\"true\"] { border: 2px solid transparent; border-radius: 13px; padding: 2px; }"
		"QToolButton[mcSwatch=\"true\"]:checked { border-color: palette(highlight); }"
		"QToolButton[mcFlat=\"true\"] { border: 1px solid transparent; border-radius: 5px; padding: 3px 8px; }"
		"QToolButton[mcFlat=\"true\"]:hover { border-color: palette(mid); }"
		"QToolButton[mcFlat=\"true\"]:checked { background: palette(highlight); color: palette(highlighted-text); }"
		"QLabel#mcLogoHint { background: palette(alternate-base); border-radius: 6px; padding: 6px 10px; }"));

	const qreal dpr = devicePixelRatioF();

	// ── Source | result ───────────────────────────────────────────────────────
	m_sourceView = new SourceView(this);
	m_resultView = new ResultView(this);
	m_sourceView->setMessage(tr("No artwork loaded.\n\nUse \"Open image…\" to pick a poster or backdrop file."));
	m_resultView->setMessage(tr("Drag a box around the movie title on the left."));

	auto* resultSide = new QWidget(this);
	{
		auto* l = new QVBoxLayout(resultSide);
		l->setContentsMargins(0, 0, 0, 0);
		l->setSpacing(4);
		auto* bar = new QHBoxLayout;
		bar->setContentsMargins(4, 0, 0, 0);
		bar->setSpacing(2);
		m_previewBgGroup = new QButtonGroup(this);
		m_previewBgGroup->setExclusive(true);
		const QList<QPair<PreviewBg, QString>> bgs = {
			{PreviewBg::Checker, tr("Checkerboard — shows what's transparent")},
			{PreviewBg::Black, tr("On black")},
			{PreviewBg::White, tr("On white")},
			{PreviewBg::Grey, tr("On grey")},
		};
		for (const auto& [bg, tip] : bgs) {
			auto* b = new QToolButton(resultSide);
			b->setProperty("mcSwatch", true);
			b->setCheckable(true);
			b->setIcon(previewBgIcon(bg, dpr));
			b->setIconSize({18, 18});
			b->setToolTip(tip);
			b->setCursor(Qt::PointingHandCursor);
			m_previewBgGroup->addButton(b, int(bg));
			bar->addWidget(b);
		}
		bar->addSpacing(8);
		auto* showOriginal = new QToolButton(resultSide);
		showOriginal->setProperty("mcFlat", true);
		showOriginal->setCheckable(true);
		showOriginal->setIcon(svgIcon(QStringLiteral(":/icons/visibility.svg")));
		showOriginal->setText(tr("Original"));
		showOriginal->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		showOriginal->setToolTip(tr("Show the untouched crop instead of the cut-out (to see what you're picking or painting)"));
		m_showOriginal = showOriginal;
		bar->addWidget(showOriginal);
		bar->addSpacing(8);
		auto* fit = new QToolButton(resultSide);
		fit->setProperty("mcFlat", true);
		fit->setText(tr("Fit"));
		fit->setToolTip(tr("Fit the whole cut-out in view (Ctrl+0)"));
		m_zoomLabel = new QLabel(QStringLiteral("100%"), resultSide);
		m_zoomLabel->setToolTip(tr("Zoom (mouse wheel over the preview; Ctrl+= / Ctrl+- zoom in/out)"));
		m_zoomLabel->setMinimumWidth(QFontMetrics(m_zoomLabel->font()).horizontalAdvance(QStringLiteral("3200%")) + 4);
		bar->addWidget(fit);
		bar->addWidget(m_zoomLabel);
		connect(fit, &QToolButton::clicked, m_resultView, &ResultView::resetView);
		bar->addStretch();
		m_sizeLabel = new QLabel(resultSide);
		QPalette sp = m_sizeLabel->palette();
		sp.setColor(QPalette::WindowText, sp.color(QPalette::PlaceholderText));
		m_sizeLabel->setPalette(sp);
		bar->addWidget(m_sizeLabel);
		l->addLayout(bar);
		l->addWidget(m_resultView, 1);
	}

	auto* split = new QSplitter(Qt::Horizontal, this);
	split->addWidget(m_sourceView);
	split->addWidget(resultSide);
	split->setStretchFactor(0, 2);
	split->setStretchFactor(1, 3);
	split->setChildrenCollapsible(false);
	main->addWidget(split, 1);

	m_hint = new QLabel(this);
	m_hint->setObjectName(QStringLiteral("mcLogoHint"));
	m_hint->setWordWrap(true);
	main->addWidget(m_hint);

	// ── Control cards ─────────────────────────────────────────────────────────
	auto* cards = new QHBoxLayout;
	cards->setSpacing(10);
	main->addLayout(cards);

	// Card 1: how the logo is separated from the background.
	{
		auto* card = makeCard(this);
		auto* l = new QVBoxLayout(card);
		l->setContentsMargins(12, 10, 12, 12);
		l->setSpacing(8);
		l->addWidget(cardTitle(tr("Cut-out"), card));

		auto* seg = new QHBoxLayout;
		seg->setSpacing(0);
		auto* modeRemove = new QToolButton(card);
		auto* modeKeep   = new QToolButton(card);
		modeRemove->setText(tr("Remove background"));
		modeKeep->setText(tr("Keep logo colours"));
		modeRemove->setToolTip(tr("Removes the colours found along the edge of your box (and any you pick).\n"
		                          "Best for logos on a flat or simple background."));
		modeKeep->setToolTip(tr("Keeps only the logo's colours, or blends of them with the background.\n"
		                        "Best for logos over busy artwork — click the logo to pick its colour."));
		for (QToolButton* b : {modeRemove, modeKeep}) {
			b->setProperty("mcSeg", true);
			b->setCheckable(true);
			b->setAutoExclusive(true);
			b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
			seg->addWidget(b);
		}
		modeRemove->setProperty("mcSegPos", QStringLiteral("left"));
		modeKeep->setProperty("mcSegPos", QStringLiteral("right"));
		// Remove background works straight away from the auto-detected edge
		// colours; Keep logo colours needs a click on the logo first.
		modeRemove->setChecked(true);
		m_modeRemove = modeRemove;
		m_modeKeep   = modeKeep;
		l->addLayout(seg);

		auto* form = new QFormLayout;
		form->setContentsMargins(0, 2, 0, 0);
		form->setHorizontalSpacing(10);
		form->setVerticalSpacing(6);
		m_logoChipsLabel = new QLabel(tr("Logo"), card);
		m_logoChips = new QHBoxLayout;
		m_logoChips->setSpacing(4);
		form->addRow(m_logoChipsLabel, m_logoChips);
		m_bgChips = new QHBoxLayout;
		m_bgChips->setSpacing(4);
		m_autoBg = new QCheckBox(tr("Auto"), card);
		m_autoBg->setChecked(true);
		m_autoBg->setToolTip(tr("Use the dominant colours along the edge of your box as background (dashed swatches)."));
		auto* bgRow = new QHBoxLayout;
		bgRow->setSpacing(8);
		bgRow->addLayout(m_bgChips, 1);
		bgRow->addWidget(m_autoBg);
		form->addRow(tr("Background"), bgRow);
		l->addLayout(form);
		l->addStretch();
		cards->addWidget(card, 4);
	}

	// Card 2: fine-tuning.
	{
		auto* card = makeCard(this);
		auto* l = new QVBoxLayout(card);
		l->setContentsMargins(12, 10, 12, 12);
		l->setSpacing(8);
		l->addWidget(cardTitle(tr("Refine"), card));
		auto* form = new QFormLayout;
		form->setHorizontalSpacing(10);
		form->setVerticalSpacing(6);
		const auto sliderRow = [&](int lo, int hi, int val, const QString& tip) {
			auto* s = new QSlider(Qt::Horizontal, card);
			s->setRange(lo, hi);
			s->setValue(val);
			s->setToolTip(tip);
			auto* v = new QLabel(QString::number(val), card);
			v->setMinimumWidth(QFontMetrics(v->font()).horizontalAdvance(QStringLiteral("000")));
			v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
			connect(s, &QSlider::valueChanged, v, [v](int x) { v->setText(QString::number(x)); });
			auto* row = new QHBoxLayout;
			row->setSpacing(6);
			row->addWidget(s, 1);
			row->addWidget(v);
			return std::pair{s, row};
		};
		auto [tol, tolRow] = sliderRow(0, 200, 60, tr("How different a pixel may be from a picked colour and still count as a match."));
		auto [soft, softRow] = sliderRow(1, 200, 40, tr("How gradually pixels fade out beyond the tolerance. Higher = softer edges."));
		m_tolerance = tol;
		m_softness  = soft;
		form->addRow(tr("Tolerance"), tolRow);
		form->addRow(tr("Softness"), softRow);

		m_edgeShrink = new QSpinBox(card);
		m_edgeShrink->setRange(0, 6);
		m_edgeShrink->setSuffix(tr(" px"));
		m_edgeShrink->setSpecialValueText(tr("Off"));
		m_edgeShrink->setToolTip(tr("Shrinks the logo's edge to remove a remaining halo."));
		m_speckArea = new QSpinBox(card);
		m_speckArea->setRange(0, 100000);
		m_speckArea->setSuffix(tr(" px"));
		m_speckArea->setSpecialValueText(tr("Off"));
		m_speckArea->setToolTip(tr("Removes stray blobs smaller than this (full-resolution pixels)."));
		auto* spins = new QHBoxLayout;
		spins->setSpacing(6);
		spins->addWidget(m_edgeShrink);
		spins->addSpacing(10);
		spins->addWidget(new QLabel(tr("Specks under"), card));
		spins->addWidget(m_speckArea);
		spins->addStretch();
		form->addRow(tr("Edge shrink"), spins);
		l->addLayout(form);
		l->addStretch();
		cards->addWidget(card, 4);
	}

	// Card 3: picking and painting.
	{
		auto* card = makeCard(this);
		auto* l = new QVBoxLayout(card);
		l->setContentsMargins(12, 10, 12, 12);
		l->setSpacing(8);
		l->addWidget(cardTitle(tr("Touch up"), card));
		auto* toolsRow = new QHBoxLayout;
		toolsRow->setSpacing(4);
		m_tools = new QButtonGroup(this);
		const auto addTool = [&](Tool t, const QString& text, const QString& tip, const QString& key) {
			auto* b = new QToolButton(card);
			b->setProperty("mcTool", true);
			b->setText(text);
			b->setToolTip(QStringLiteral("%1 (%2)").arg(tip, key));
			b->setIcon(toolIcon(t, palette(), dpr));
			b->setIconSize({28, 28});
			b->setCheckable(true);
			b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
			b->setMinimumWidth(64);
			m_tools->addButton(b, int(t));
			toolsRow->addWidget(b);
			auto* sc = new QShortcut(QKeySequence(key), this);
			connect(sc, &QShortcut::activated, b, &QToolButton::click);
			return b;
		};
		addTool(Tool::Pick, tr("Pick"), tr("Click the preview to add a colour; Shift+click adds a background colour"),
		        QStringLiteral("I"))->setChecked(true);
		addTool(Tool::Keep, tr("Keep"), tr("Paint areas that must stay in the logo"), QStringLiteral("B"));
		addTool(Tool::Erase, tr("Erase"), tr("Paint areas to remove"), QStringLiteral("E"));
		toolsRow->addStretch();
		m_btnUndo = new QToolButton(card);
		m_btnUndo->setProperty("mcTool", true);
		m_btnUndo->setIcon(svgIcon(QStringLiteral(":/icons/undo.svg")));
		m_btnUndo->setIconSize({22, 22});
		m_btnUndo->setText(tr("Undo"));
		m_btnUndo->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
		m_btnUndo->setToolTip(tr("Undo the last colour pick or brush stroke (Ctrl+Z)"));
		m_btnUndo->setEnabled(false);
		toolsRow->addWidget(m_btnUndo);
		l->addLayout(toolsRow);

		auto* sizeRow = new QHBoxLayout;
		sizeRow->setSpacing(6);
		sizeRow->addWidget(new QLabel(tr("Brush"), card));
		m_brushSize = new QSlider(Qt::Horizontal, card);
		m_brushSize->setRange(2, 80);
		m_brushSize->setValue(12);
		m_brushSize->setToolTip(tr("Brush size ( [ and ] keys )"));
		sizeRow->addWidget(m_brushSize, 1);
		auto* dot = new BrushDot(card);
		dot->setRadius(m_brushSize->value());
		connect(m_brushSize, &QSlider::valueChanged, dot, [dot](int v) { dot->setRadius(v); });
		sizeRow->addWidget(dot);
		l->addLayout(sizeRow);

		// Shape (round/square) and, for the square, its rotation — a square
		// tilted to the slant of the lettering clears artifacts tight to an
		// edge without biting into the letter.
		auto* shapeRow = new QHBoxLayout;
		shapeRow->setSpacing(0);
		auto* round  = new QToolButton(card);
		auto* square = new QToolButton(card);
		round->setIcon(brushShapeIcon(false, palette(), dpr));
		square->setIcon(brushShapeIcon(true, palette(), dpr));
		round->setToolTip(tr("Round brush (S toggles)"));
		square->setToolTip(tr("Square brush — rotate it with the slider or R / Shift+R (S toggles)"));
		for (QToolButton* b : {round, square}) {
			b->setProperty("mcSeg", true);
			b->setCheckable(true);
			b->setAutoExclusive(true);
			shapeRow->addWidget(b);
		}
		round->setProperty("mcSegPos", QStringLiteral("left"));
		square->setProperty("mcSegPos", QStringLiteral("right"));
		round->setChecked(true);
		m_brushSquare = square;
		shapeRow->addSpacing(10);
		m_brushAngle = new QSlider(Qt::Horizontal, card);
		m_brushAngle->setRange(0, 89);  // a square repeats every 90 degrees
		m_brushAngle->setValue(0);
		m_brushAngle->setToolTip(tr("Square brush rotation (R / Shift+R: \u00b115\u00b0)"));
		m_brushAngle->setEnabled(false);
		shapeRow->addWidget(m_brushAngle, 1);
		auto* angleLabel = new QLabel(QStringLiteral("0\u00b0"), card);
		angleLabel->setMinimumWidth(QFontMetrics(angleLabel->font()).horizontalAdvance(QStringLiteral("00\u00b0")) + 4);
		angleLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
		shapeRow->addSpacing(6);
		shapeRow->addWidget(angleLabel);
		l->addLayout(shapeRow);

		const auto shapeChanged = [this, dot, angleLabel]() {
			const bool sq = m_brushSquare->isChecked();
			m_brushAngle->setEnabled(sq);
			angleLabel->setEnabled(sq);
			angleLabel->setText(QStringLiteral("%1\u00b0").arg(m_brushAngle->value()));
			dot->setShape(sq, m_brushAngle->value());
			m_resultView->setBrushShape(sq, m_brushAngle->value());
		};
		connect(square, &QToolButton::toggled, this, shapeChanged);
		connect(m_brushAngle, &QSlider::valueChanged, this, shapeChanged);
		auto* toggleShape = new QShortcut(QKeySequence(Qt::Key_S), this);
		connect(toggleShape, &QShortcut::activated, this, [round, square]() {
			(square->isChecked() ? round : square)->setChecked(true);
		});
		const auto rotate = [this](int delta) {
			if (!m_brushSquare->isChecked()) m_brushSquare->setChecked(true);
			m_brushAngle->setValue(((m_brushAngle->value() + delta) % 90 + 90) % 90);
		};
		auto* rotCw  = new QShortcut(QKeySequence(Qt::Key_R), this);
		auto* rotCcw = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_R), this);
		connect(rotCw, &QShortcut::activated, this, [rotate]() { rotate(15); });
		connect(rotCcw, &QShortcut::activated, this, [rotate]() { rotate(-15); });
		auto* smaller = new QShortcut(QKeySequence(Qt::Key_BracketLeft), this);
		auto* bigger  = new QShortcut(QKeySequence(Qt::Key_BracketRight), this);
		connect(smaller, &QShortcut::activated, this, [this]() { m_brushSize->setValue(m_brushSize->value() * 4 / 5); });
		connect(bigger, &QShortcut::activated, this,
		        [this]() { m_brushSize->setValue(std::max(m_brushSize->value() + 1, m_brushSize->value() * 5 / 4)); });
		l->addStretch();
		cards->addWidget(card, 3);
	}

	// ── Buttons ───────────────────────────────────────────────────────────────
	{
		auto* bottom = new QHBoxLayout;
		auto* btnOpen = new QPushButton(svgIcon(QStringLiteral(":/icons/folder_open.svg")), tr("Open image…"), this);
		btnOpen->setToolTip(tr("Use a local image file as the source instead"));
		btnOpen->setAutoDefault(false);
		bottom->addWidget(btnOpen);
		bottom->addStretch();
		auto* box = new QDialogButtonBox(this);
		m_btnSave = box->addButton(tr("Save PNG…"), QDialogButtonBox::ActionRole);
		m_btnSave->setToolTip(tr("Save a trimmed, transparent PNG at full resolution — e.g. to upload to TMDB"));
		m_btnUse = box->addButton(tr("Use as clear logo"), QDialogButtonBox::AcceptRole);
		m_btnUse->setDefault(true);
		box->addButton(QDialogButtonBox::Cancel);
		m_btnSave->setEnabled(false);
		m_btnUse->setEnabled(false);
		bottom->addWidget(box);
		main->addLayout(bottom);

		connect(btnOpen, &QPushButton::clicked, this, [this]() {
			const QString path = QFileDialog::getOpenFileName(
				this, tr("Open Image"),
				QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
				tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
			if (!path.isEmpty()) loadFile(path);
		});
		connect(m_btnSave, &QPushButton::clicked, this, &McLogoExtractDialog::saveToFile);
		connect(m_btnUse, &QPushButton::clicked, this, &McLogoExtractDialog::useAsLogo);
		connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
	}

	m_recomputeTimer = new QTimer(this);
	m_recomputeTimer->setSingleShot(true);
	m_recomputeTimer->setInterval(15);
	connect(m_recomputeTimer, &QTimer::timeout, this, &McLogoExtractDialog::recompute);

	// Box drags/key-repeats update the preview at most every 40 ms (the timer
	// isn't restarted, so the result follows the drag rather than waiting for
	// it to stop); the final box on release is applied straight away.
	m_selectionTimer = new QTimer(this);
	m_selectionTimer->setSingleShot(true);
	m_selectionTimer->setInterval(40);
	connect(m_selectionTimer, &QTimer::timeout, this, [this]() { setSelection(m_pendingSelection); });
	m_sourceView->onSelection = [this](QRect r, bool live) {
		if (live) {
			m_pendingSelection = r;
			if (!m_selectionTimer->isActive()) m_selectionTimer->start();
		} else {
			m_selectionTimer->stop();
			setSelection(r);
		}
	};
	m_resultView->onPick = [this](QPoint p, bool shift) { onPick(p, shift); };
	m_resultView->onStrokeBegin = [this]() {
		m_strokeBackup = m_overrides;  // shared until the first stamp detaches it
		m_strokeDirty  = {};
	};
	m_resultView->onStroke = [this](QPoint a, QPoint b, double r) { onStroke(a, b, r); };
	m_resultView->onZoomChanged = [this](double z) {
		m_zoomLabel->setText(QStringLiteral("%1%").arg(qRound(z * 100)));
	};
	{
		auto* fitKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_0), this);
		auto* inKey  = new QShortcut(QKeySequence::ZoomIn, this);
		auto* outKey = new QShortcut(QKeySequence::ZoomOut, this);
		auto* inKey2 = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Equal), this);
		connect(fitKey, &QShortcut::activated, m_resultView, &ResultView::resetView);
		connect(inKey, &QShortcut::activated, this, [this]() { m_resultView->zoomBy(1.25); });
		connect(inKey2, &QShortcut::activated, this, [this]() { m_resultView->zoomBy(1.25); });
		connect(outKey, &QShortcut::activated, this, [this]() { m_resultView->zoomBy(0.8); });
	}
	m_resultView->onStrokeEnd = [this]() {
		// Undo keeps only the part of the mask this stroke touched.
		if (!m_strokeDirty.isEmpty()) {
			m_undo.append({m_logoColours, m_bgColours, m_strokeDirty, m_strokeBackup.copy(m_strokeDirty)});
			if (m_undo.size() > kMaxUndo) m_undo.removeFirst();
			m_btnUndo->setEnabled(true);
		}
		m_strokeBackup = {};
		updateStatus();
	};

	const auto changed = [this]() { scheduleRecompute(); };
	connect(m_modeKeep, &QAbstractButton::toggled, this, [this]() {
		rebuildChips();
		updateHint();
		scheduleRecompute();
	});
	connect(m_autoBg, &QCheckBox::toggled, this, [this]() {
		rebuildChips();
		scheduleRecompute();
	});
	connect(m_tolerance, &QSlider::valueChanged, this, changed);
	connect(m_softness, &QSlider::valueChanged, this, changed);
	connect(m_edgeShrink, &QSpinBox::valueChanged, this, changed);
	connect(m_speckArea, &QSpinBox::valueChanged, this, changed);
	connect(m_tools, &QButtonGroup::idClicked, this, [this](int id) {
		m_resultView->setTool(Tool(id));
		updateHint();
	});
	connect(m_brushSize, &QSlider::valueChanged, m_resultView, [this](int v) { m_resultView->setBrushRadius(v); });
	connect(m_showOriginal, &QAbstractButton::toggled, this, [this](bool on) { m_resultView->setShowOriginal(on); });
	connect(m_previewBgGroup, &QButtonGroup::idToggled, this, [this](int id, bool on) {
		if (on) m_resultView->setBackground(PreviewBg(id));
	});
	connect(m_btnUndo, &QToolButton::clicked, this, &McLogoExtractDialog::undo);
	auto* undoShortcut = new QShortcut(QKeySequence::Undo, this);
	connect(undoShortcut, &QShortcut::activated, this, &McLogoExtractDialog::undo);

	{
		QSettings s(AppSettings::geometryFilePath(), QSettings::IniFormat);
		const QByteArray geo = s.value("logoExtractDialog/geometry").toByteArray();
		if (!geo.isEmpty()) {
			restoreGeometry(geo);
			ensureGeometryFitsScreen(this);
		} else {
			resize(clampSizeToScreen(this, QSize(1200, 780)));
		}
		const QByteArray st = s.value("logoExtractDialog/splitter").toByteArray();
		if (!st.isEmpty()) split->restoreState(st);
		if (QAbstractButton* b = m_previewBgGroup->button(qBound(0, s.value("logoExtractDialog/previewBg", 0).toInt(), 3)))
			b->setChecked(true);
	}
	m_splitter = split;

	rebuildChips();
	updateHint();
}

McLogoExtractDialog::~McLogoExtractDialog()
{
	if (m_reply) m_reply->disconnect(this);
}

void McLogoExtractDialog::done(int result)
{
	QSettings s(AppSettings::geometryFilePath(), QSettings::IniFormat);
	s.setValue("logoExtractDialog/geometry", saveGeometry());
	if (m_splitter) s.setValue("logoExtractDialog/splitter", m_splitter->saveState());
	s.setValue("logoExtractDialog/previewBg", m_previewBgGroup->checkedId());
	QDialog::done(result);
}

void McLogoExtractDialog::loadUrl(const QUrl& url)
{
	if (m_reply) {
		m_reply->disconnect(this);
		m_reply = nullptr;
	}
	m_sourceView->setImage({});
	m_sourceView->setMessage(tr("Downloading full-resolution artwork…"));
	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	QNetworkReply* r = m_nam->get(req);
	m_reply = r;
	connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
	connect(r, &QNetworkReply::finished, this, [this, r]() {
		if (r != m_reply) return;
		m_reply = nullptr;
		QImage img;
		if (r->error() != QNetworkReply::NoError || !img.loadFromData(r->readAll())) {
			m_sourceView->setMessage(tr("Couldn't download the artwork:\n%1\n\nUse \"Open image…\" to pick a file instead.")
			                             .arg(r->errorString()));
			return;
		}
		setSourceImage(img);
	});
}

void McLogoExtractDialog::loadFile(const QString& path)
{
	QImage img(path);
	if (img.isNull()) {
		QMessageBox::warning(this, tr("Open Image"), tr("Couldn't read %1 as an image.").arg(QDir::toNativeSeparators(path)));
		return;
	}
	if (m_reply) {
		m_reply->disconnect(this);
		m_reply = nullptr;
	}
	setSourceImage(img);
}

void McLogoExtractDialog::setSourceImage(const QImage& img)
{
	m_fullSource = img.convertToFormat(QImage::Format_RGB32);
	m_sourceView->setImage(m_fullSource);
	m_selection = {};
	m_fullCrop = {};
	m_previewCrop = {};
	m_previewExtractor.setSource({});
	m_overrides = QImage(m_fullSource.size(), QImage::Format_Grayscale8);
	m_overrides.fill(LogoOverride::None);
	m_undo.clear();
	m_btnUndo->setEnabled(false);
	m_resultView->setImages({}, {});
	m_resultView->setMessage(tr("Drag a box around the movie title on the left (%1 × %2 px source).")
	                             .arg(m_fullSource.width()).arg(m_fullSource.height()));
	m_btnSave->setEnabled(false);
	m_btnUse->setEnabled(false);
	m_sizeLabel->clear();
	rebuildChips();
	updateHint();
}

void McLogoExtractDialog::setSelection(const QRect& sel)
{
	if (sel.isEmpty() || (sel == m_selection && !m_fullCrop.isNull())) return;
	const bool firstBox = m_fullCrop.isNull();
	m_selection = sel;
	m_fullCrop  = m_fullSource.copy(sel);
	m_previewScale = std::min({1.0, double(kPreviewMaxW) / m_fullCrop.width(), double(kPreviewMaxH) / m_fullCrop.height()});
	m_previewCrop = m_previewScale < 1.0
		? m_fullCrop.scaled(std::max(1, int(std::lround(m_fullCrop.width() * m_previewScale))),
		                    std::max(1, int(std::lround(m_fullCrop.height() * m_previewScale))),
		                    Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
		: m_fullCrop;
	m_previewExtractor.setSource(m_previewCrop);
	// Brush marks live in artwork coordinates, so moving or resizing the box
	// keeps them (and the undo history) — the preview just takes its slice.
	m_previewOverrides = previewOverridesFromMask();
	if (firstBox) {
		// Blobs under ~0.02% of the area are almost always noise, not lettering.
		// Only defaulted once, so adjusting the box doesn't undo the user's value.
		m_speckArea->blockSignals(true);
		m_speckArea->setValue(std::max(8, int(m_fullCrop.width() * double(m_fullCrop.height()) * 0.0002)));
		m_speckArea->blockSignals(false);
	}
	rebuildChips();
	updateHint();
	recompute();
}

LogoExtractParams McLogoExtractDialog::currentParams() const
{
	LogoExtractParams p;
	p.mode = m_modeKeep->isChecked() ? LogoExtractMode::KeepLogoColours : LogoExtractMode::RemoveBackground;
	p.logoColours       = m_logoColours;
	p.backgroundColours = m_bgColours;
	p.autoBackground    = m_autoBg->isChecked();
	p.tolerance         = m_tolerance->value();
	p.softness          = m_softness->value();
	p.edgeShrink        = m_edgeShrink->value();
	p.minSpeckArea      = m_speckArea->value();
	return p;
}

void McLogoExtractDialog::scheduleRecompute()
{
	if (!m_previewCrop.isNull()) m_recomputeTimer->start();
}

void McLogoExtractDialog::recompute()
{
	if (m_previewCrop.isNull()) return;
	LogoExtractParams p = currentParams();
	// Work in preview pixels: scale the pixel-sized parameters with the image.
	p.minSpeckArea = p.minSpeckArea > 0
		? std::max(1, int(std::lround(p.minSpeckArea * m_previewScale * m_previewScale))) : 0;
	p.edgeShrink = p.edgeShrink > 0 ? std::max(1, int(std::lround(p.edgeShrink * m_previewScale))) : 0;
	// Cache the colour-model result: brush strokes only re-apply the overrides
	// over it (onStroke), which is what keeps painting fast.
	m_previewBase = m_previewExtractor.processBase(p);
	m_resultView->setImages(m_previewBase, m_previewCrop);
	QImage& result = m_resultView->result();
	m_previewExtractor.applyOverrides(result, m_previewBase, m_previewOverrides, result.rect());
	rebuildChips();  // which auto colours are in use depends on tolerance + logo colours
	updateStatus();
}

void McLogoExtractDialog::updateStatus()
{
	if (m_previewCrop.isNull()) return;
	const LogoExtractParams p = currentParams();
	const QImage& res = m_resultView->result();
	const QRect b = LogoExtractor::opaqueBounds(res);
	{
		// Under 1% of the box left solid: the box edge most likely runs
		// through something in the logo's colours.
		qint64 solid = 0;
		for (int y = 0; y < res.height(); ++y) {
			const auto* line = reinterpret_cast<const QRgb*>(res.constScanLine(y));
			for (int x = 0; x < res.width(); ++x) solid += qAlpha(line[x]) > 128;
		}
		const bool nearlyEmpty = solid * 100 < qint64(res.width()) * res.height();
		if (nearlyEmpty != m_nearlyEmpty) {
			m_nearlyEmpty = nearlyEmpty;
			updateHint();
		}
	}
	const bool hasModel = p.mode == LogoExtractMode::KeepLogoColours
		? !p.logoColours.isEmpty()
		: (!p.backgroundColours.isEmpty() || (p.autoBackground && !m_previewExtractor.autoBackgroundColours().isEmpty()));
	m_btnSave->setEnabled(!b.isNull() && hasModel);
	m_btnUse->setEnabled(!b.isNull() && hasModel);
	if (b.isNull()) {
		m_sizeLabel->setText(tr("Nothing left — loosen the tolerance or pick other colours."));
	} else {
		const int w = int(std::lround(b.width() / m_previewScale));
		const int h = int(std::lround(b.height() / m_previewScale));
		m_sizeLabel->setText(w < 500 ? tr("Logo: %1 × %2 px — small; a larger source gives a sharper logo").arg(w).arg(h)
		                             : tr("Logo: %1 × %2 px").arg(w).arg(h));
	}
}

QImage McLogoExtractDialog::finalImage() const
{
	if (m_fullCrop.isNull()) return {};
	LogoExtractParams p = currentParams();
	// Reuse the background colours the preview showed, so full resolution
	// matches what the user tuned against.
	if (p.autoBackground) {
		p.backgroundColours += m_previewExtractor.usableAutoBackground(p);
		p.autoBackground = false;
	}
	LogoExtractor full;
	full.setSource(m_fullCrop);
	const QImage cut = full.process(p, m_overrides.copy(m_selection));
	const int pad = std::max(2, int(std::lround(std::max(cut.width(), cut.height()) * 0.01)));
	return LogoExtractor::trimmed(cut, pad);
}

QImage McLogoExtractDialog::previewOverridesFromMask() const
{
	const QImage crop = m_overrides.copy(m_selection);
	return m_previewScale < 1.0 ? crop.scaled(m_previewCrop.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation)
	                            : crop;
}

void McLogoExtractDialog::pushUndo()
{
	m_undo.append({m_logoColours, m_bgColours, {}, {}});
	if (m_undo.size() > kMaxUndo) m_undo.removeFirst();
	m_btnUndo->setEnabled(true);
}

void McLogoExtractDialog::undo()
{
	if (m_undo.isEmpty()) return;
	const Snapshot s = m_undo.takeLast();
	m_logoColours = s.logo;
	m_bgColours   = s.bg;
	if (!s.maskPatch.isNull()) {
		// Put back the part of the mask the stroke changed.
		for (int y = 0; y < s.maskRect.height(); ++y)
			std::memcpy(m_overrides.scanLine(s.maskRect.top() + y) + s.maskRect.left(),
			            s.maskPatch.constScanLine(y), size_t(s.maskRect.width()));
		if (!m_fullCrop.isNull()) m_previewOverrides = previewOverridesFromMask();
	}
	m_btnUndo->setEnabled(!m_undo.isEmpty());
	rebuildChips();
	updateHint();
	scheduleRecompute();
}

void McLogoExtractDialog::onPick(QPoint previewPos, bool shift)
{
	const QPoint full(int(previewPos.x() / m_previewScale), int(previewPos.y() / m_previewScale));
	const int    r = std::max(1, int(std::lround(1.0 / m_previewScale)));
	const QRgb   c = LogoExtractor::sampleColour(m_fullCrop, full, r);
	pushUndo();
	if (m_modeKeep->isChecked() && !shift)
		m_logoColours.append(c);
	else
		m_bgColours.append(c);
	rebuildChips();
	updateHint();
	scheduleRecompute();
}

void McLogoExtractDialog::onStroke(QPoint fromPreview, QPoint toPreview, double radiusPreview)
{
	const Tool   tool   = Tool(m_tools->checkedId());
	const uchar  value  = tool == Tool::Keep ? LogoOverride::Keep : LogoOverride::Erase;
	const bool   square = m_brushSquare->isChecked();
	const double angle  = m_brushAngle->value();
	const auto stampSegment = [value, square, angle](QImage& img, QPointF a, QPointF b, double r) {
		const double dist  = std::hypot(b.x() - a.x(), b.y() - a.y());
		const int    steps = std::max(1, int(std::ceil(dist / std::max(0.5, r / 2))));
		for (int i = 0; i <= steps; ++i) {
			const QPointF c = a + (b - a) * (double(i) / steps);
			if (square) stampSquare(img, c, r, angle, value);
			else        stampDisk(img, c, r, value);
		}
	};
	// Full resolution (the final image) and preview size (what's shown) —
	// painting the preview copy directly avoids rescaling the full mask per move.
	const QPointF origin = m_selection.topLeft();
	const QPointF fromSrc = origin + QPointF(fromPreview) / m_previewScale;
	const QPointF toSrc   = origin + QPointF(toPreview) / m_previewScale;
	const double  rSrc    = radiusPreview / m_previewScale;
	stampSegment(m_overrides, fromSrc, toSrc, rSrc);
	stampSegment(m_previewOverrides, QPointF(fromPreview), QPointF(toPreview), radiusPreview);
	const double reach = square ? 1.4143 : 1.0;  // a rotated square's corners reach further
	const int pad = int(std::ceil(rSrc * reach)) + 1;
	m_strokeDirty |= QRectF(fromSrc, toSrc).normalized().toAlignedRect()
	                     .adjusted(-pad, -pad, pad, pad).intersected(m_overrides.rect());

	const int   padPv = int(std::ceil(radiusPreview * reach)) + 1;
	const QRect dirty = QRect(fromPreview, toPreview).normalized().adjusted(-padPv, -padPv, padPv, padPv);
	if (m_previewBase.isNull()) return;
	m_previewExtractor.applyOverrides(m_resultView->result(), m_previewBase, m_previewOverrides, dirty);
	m_resultView->resultChanged(dirty);
}

void McLogoExtractDialog::rebuildChips()
{
	// deleteLater: this runs from a swatch's own clicked() signal when the
	// user removes a colour; deleting the sender there is unsafe.
	const auto clear = [](QHBoxLayout* l) {
		while (QLayoutItem* it = l->takeAt(0)) {
			if (QWidget* w = it->widget()) {
				w->hide();
				w->deleteLater();
			}
			delete it;
		}
	};
	clear(m_logoChips);
	clear(m_bgChips);

	const auto addChip = [this](QHBoxLayout* l, QRgb c, bool removable, QList<QRgb>* list, int index,
	                            bool ignored = false) {
		auto* b = new QToolButton(this);
		b->setFixedSize(22, 22);
		b->setAutoRaise(false);
		const QColor col(c);
		b->setStyleSheet(QStringLiteral("QToolButton { background: %1; border: 1px %2 palette(mid); border-radius: 3px;"
		                                " color: %3; font-weight: bold; }")
		                     .arg(col.name(), removable ? QStringLiteral("solid") : QStringLiteral("dashed"),
		                          col.lightnessF() > 0.5 ? QStringLiteral("#000") : QStringLiteral("#fff")));
		if (ignored) {
			b->setText(QStringLiteral("\u00d7"));
			b->setToolTip(tr("%1 — found on the selection edge, but ignored: it's too close to a logo colour").arg(col.name()));
			b->setEnabled(false);
		} else if (removable) {
			b->setToolTip(tr("%1 — click to remove").arg(col.name()));
			connect(b, &QToolButton::clicked, this, [this, list, index]() {
				if (index >= list->size()) return;
				pushUndo();
				list->removeAt(index);
				rebuildChips();
				updateHint();
				scheduleRecompute();
			});
		} else {
			b->setToolTip(tr("%1 — auto-detected from the selection edge").arg(col.name()));
			b->setEnabled(false);
		}
		l->addWidget(b);
	};

	const bool keep = m_modeKeep->isChecked();
	m_logoChipsLabel->setEnabled(keep);
	for (int i = 0; i < m_logoColours.size(); ++i)
		addChip(m_logoChips, m_logoColours[i], true, &m_logoColours, i);
	if (m_logoColours.isEmpty()) {
		auto* none = new QLabel(keep ? tr("none — click the logo") : tr("not used"), this);
		none->setEnabled(false);
		m_logoChips->addWidget(none);
	}
	m_logoChips->addStretch();

	if (m_autoBg->isChecked()) {
		const QList<QRgb> used = m_previewExtractor.usableAutoBackground(currentParams());
		for (QRgb c : m_previewExtractor.autoBackgroundColours())
			addChip(m_bgChips, c, false, nullptr, 0, !used.contains(c));
	}
	for (int i = 0; i < m_bgColours.size(); ++i)
		addChip(m_bgChips, m_bgColours[i], true, &m_bgColours, i);
}

void McLogoExtractDialog::updateHint()
{
	QString text;
	if (m_fullSource.isNull())
		text = tr("Waiting for the artwork…");
	else if (!m_fullCrop.isNull() && m_nearlyEmpty && !m_showOriginal->isChecked()
	         && (m_modeRemove->isChecked() || !m_logoColours.isEmpty()))
		text = m_modeRemove->isChecked()
			? tr("Almost nothing is left. The edge of your box probably runs through something in the logo's colours "
			     "(a shirt, the sky…) — shrink the box away from it, or switch to Keep logo colours and click the logo.")
			: tr("Almost nothing is left. Try a higher Tolerance, pick the logo's colour on a brighter spot, "
			     "or shrink the box away from anything in the logo's colours.");
	else if (m_fullCrop.isNull())
		text = tr("Drag a box around the movie title on the left. It doesn't have to be exact — drag its edges or corners "
		          "to adjust it afterwards, and the saved logo is trimmed automatically. Don't clip any glow or shadow.");
	else if (m_modeKeep->isChecked() && m_logoColours.isEmpty())
		text = tr("Click the brightest, purest part of the logo in the preview to pick its colour. "
		          "Pick a few spots if it has several colours or a gradient.");
	else if (Tool(m_tools->checkedId()) != Tool::Pick)
		text = tr("Paint over parts that are wrong. Ctrl+Z undoes a stroke. Turn on \"Original\" above the preview to see what was there.");
	else if (m_modeKeep->isChecked())
		text = tr("Click to add more logo colours; Shift+click adds a background colour. Click a swatch to remove it. "
		          "Use the brushes for anything the colours can't separate.");
	else
		text = tr("Click to add background colours to remove. Click a swatch to remove it. "
		          "Use the brushes for anything the colours can't separate.");
	m_hint->setText(text);
}

void McLogoExtractDialog::saveToFile()
{
	const QImage img = finalImage();
	if (img.isNull()) return;
	QSettings s(AppSettings::geometryFilePath(), QSettings::IniFormat);
	QString dir = s.value("logoExtractDialog/lastDir").toString();
	if (dir.isEmpty() || !QDir(dir).exists())
		dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	const QString base = safeFileName(m_movieLabel.isEmpty() ? tr("clearlogo") : m_movieLabel + QStringLiteral(" clearlogo"));
	const QString path = QFileDialog::getSaveFileName(this, tr("Save Clear Logo"), dir + "/" + base + ".png",
	                                                  tr("PNG image (*.png)"));
	if (path.isEmpty()) return;
	if (!img.save(path, "PNG")) {
		QMessageBox::warning(this, tr("Save Clear Logo"), tr("Couldn't write %1.").arg(QDir::toNativeSeparators(path)));
		return;
	}
	s.setValue("logoExtractDialog/lastDir", QFileInfo(path).absolutePath());
}

void McLogoExtractDialog::useAsLogo()
{
	m_result = finalImage();
	if (m_result.isNull()) return;
	accept();
}

} // namespace Mc
