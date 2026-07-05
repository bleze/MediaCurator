#include "ui/McMainWindow.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"

#include <QApplication>
#include <QIcon>
#include <QCoreApplication>
#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPixmapCache>
#include <QProxyStyle>
#include <QRandomGenerator>
#include <QSplashScreen>
#include <QStyleFactory>
#include <QSvgRenderer>
#include <QWindow>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>

// Fusion draws disabled text with an emboss shadow (light color at +1,+1 offset).
// On dark palettes that shadow is near-white and looks like a "double" rendering.
// Returning 0 for SH_EtchDisabledText suppresses it unconditionally.
class McAppStyle : public QProxyStyle {
public:
	explicit McAppStyle(QStyle* base) : QProxyStyle(base) {}
	int styleHint(StyleHint hint, const QStyleOption* opt = nullptr,
	              const QWidget* widget = nullptr,
	              QStyleHintReturn* ret = nullptr) const override
	{
		if (hint == SH_EtchDisabledText) return 0;
		return QProxyStyle::styleHint(hint, opt, widget, ret);
	}
};

static QPixmap buildSplashPixmap(const QString& version)
{
	constexpr int W = 520, H = 290;
	constexpr int STRIP_W = 22;   // film-strip width each side

	QPixmap px(W, H);
	px.fill(QColor(0x0d, 0x0e, 0x1a));

	QPainter sp(&px);
	sp.setRenderHint(QPainter::Antialiasing);
	sp.setRenderHint(QPainter::SmoothPixmapTransform);

	// ── 1. Base gradient ──────────────────────────────────────────────────────
	{
		QLinearGradient bg(0, 0, 0, H);
		bg.setColorAt(0.0, QColor(0x16, 0x18, 0x2e));
		bg.setColorAt(0.5, QColor(0x0f, 0x10, 0x20));
		bg.setColorAt(1.0, QColor(0x08, 0x09, 0x13));
		sp.fillRect(0, 0, W, H, bg);
	}

	// ── 2. Poster mosaic (graceful no-op when cache is empty) ─────────────────
	{
		const QString posterDir = QStandardPaths::writableLocation(
			QStandardPaths::AppDataLocation) + "/posters";
		const QStringList files = QDir(posterDir).entryList({"*.jpg"}, QDir::Files);

		if (!files.isEmpty()) {
			constexpr int PW = 70, PH = 105, STEP = PW + 4;
			const int cols = W / STEP + 2;
			const int needed = cols * 3;

			QList<QPixmap> pool;
			pool.reserve(needed);
			for (int i = 0; pool.size() < needed && i < needed * 2; ++i) {
				QPixmap pm(posterDir + "/" + files[i % files.size()]);
				if (!pm.isNull())
					pool.append(pm.scaled(PW, PH, Qt::IgnoreAspectRatio,
					                      Qt::SmoothTransformation));
			}

			if (!pool.isEmpty()) {
				// Three staggered rows: top/bottom partially clipped for depth
				const struct { int y; int xOff; } rows[3] = {
					{ -8,      0 },
					{ 92,  -STEP / 2 },   // brick-wall offset
					{ 188,     0 },
				};
				sp.setOpacity(0.18);
				int pi = 0;
				for (const auto& row : rows)
					for (int c = 0; c < cols; ++c, ++pi)
						sp.drawPixmap(row.xOff + c * STEP, row.y, pool[pi % pool.size()]);
				sp.setOpacity(1.0);
			}
		}
	}

	// ── 3. Dark overlay (ensures readability regardless of poster content) ─────
	{
		QLinearGradient ov(0, 0, 0, H);
		ov.setColorAt(0.0, QColor(0x0d, 0x0e, 0x1a, 215));
		ov.setColorAt(0.5, QColor(0x0d, 0x0e, 0x1a, 160));
		ov.setColorAt(1.0, QColor(0x0d, 0x0e, 0x1a, 215));
		sp.fillRect(0, 0, W, H, ov);
	}

	// ── 4. Radial vignette ────────────────────────────────────────────────────
	{
		QRadialGradient vig(W / 2.0, H / 2.0, W * 0.65);
		vig.setColorAt(0.25, QColor(0, 0, 0,   0));
		vig.setColorAt(1.00, QColor(0, 0, 0, 195));
		sp.fillRect(0, 0, W, H, vig);
	}

	// ── 5. Film strips ────────────────────────────────────────────────────────
	{
		const QColor stripBg (0x06, 0x06, 0x0f, 235);
		const QColor holeCol (0x1c, 0x1e, 0x34);   // lighter than strip → visible perforations
		const QColor edgeCol (0x2a, 0x2a, 0x46, 160);

		constexpr int HOLE_W = 10, HOLE_H = 14, HOLE_R = 2, HOLE_STEP = 30;

		for (int side = 0; side < 2; ++side) {
			const int sx = (side == 0) ? 0 : W - STRIP_W;

			sp.setPen(Qt::NoPen);
			sp.setBrush(stripBg);
			sp.drawRect(sx, 0, STRIP_W, H);

			// Inner-edge highlight
			sp.setPen(QPen(edgeCol, 1));
			const int ex = (side == 0) ? STRIP_W : W - STRIP_W - 1;
			sp.drawLine(ex, 0, ex, H);

			// Sprocket holes
			sp.setPen(Qt::NoPen);
			sp.setBrush(holeCol);
			const int hx = sx + (STRIP_W - HOLE_W) / 2;
			for (int y = 6; y + HOLE_H <= H; y += HOLE_STEP)
				sp.drawRoundedRect(hx, y, HOLE_W, HOLE_H, HOLE_R, HOLE_R);
		}
	}

	// ── 6. Film grain (stable seed → same texture every launch) ──────────────
	{
		QRandomGenerator rng(0x4d43'5241u);   // "MCRA"
		for (int i = 0; i < 5000; ++i)
			sp.setPen(QColor(255, 255, 255, rng.bounded(4, 12))),
			sp.drawPoint(rng.bounded(W), rng.bounded(H));
	}

	// ── 7. App icon with soft glow ────────────────────────────────────────────
	{
		constexpr int SZ = 72, IY = 41;   // shifted so title centre lands at H/2
		const int IX = (W - SZ) / 2;

		QRadialGradient glow(W / 2.0, IY + SZ / 2.0, SZ * 0.9);
		glow.setColorAt(0.0, QColor(0x4a, 0x68, 0xd8, 50));
		glow.setColorAt(1.0, QColor(0, 0, 0, 0));
		sp.fillRect(IX - SZ / 2, IY - SZ / 2, SZ * 2, SZ * 2, glow);

		QSvgRenderer icon(QStringLiteral(":/icons/app_icon.svg"));
		icon.render(&sp, QRectF(IX, IY, SZ, SZ));
	}

	// ── 8. Title + accent line ────────────────────────────────────────────────
	{
		sp.setFont(QFont("Segoe UI", 27, QFont::Light));
		sp.setPen(QColor(0xe8, 0xe8, 0xff));
		sp.drawText(QRect(STRIP_W, 122, W - STRIP_W * 2, 46),
		            Qt::AlignHCenter | Qt::AlignVCenter, "MediaCurator");

		constexpr int LW = 52;
		const int lx = (W - LW) / 2;
		sp.setPen(QPen(QColor(0x55, 0x78, 0xe8, 185), 1.5));
		sp.drawLine(lx, 172, lx + LW, 172);
	}

	// ── 9. Footer ─────────────────────────────────────────────────────────────
	{
		constexpr int FX = STRIP_W + 10;
		constexpr int FW = W - STRIP_W * 2 - 20;

		// Separator line
		sp.setPen(QPen(QColor(0x22, 0x22, 0x3c, 180), 1));
		sp.drawLine(FX, 262, FX + FW, 262);

		sp.setFont(QFont("Segoe UI", 8));
		sp.setPen(QColor(0x44, 0x44, 0x64));
		sp.drawText(QRect(FX, 265, FW, 18), Qt::AlignLeft  | Qt::AlignVCenter,
		            "Bleze Software");
		sp.drawText(QRect(FX, 265, FW, 18), Qt::AlignRight | Qt::AlignVCenter,
		            QString("v%1").arg(version));
	}

	sp.end();
	return px;
}

int main(int argc, char* argv[])
{
	QApplication app(argc, argv);
	app.setStyle(new McAppStyle(QStyleFactory::create("Fusion")));
	QPixmapCache::setCacheLimit(256 * 1024);  // 256 MB — scaled fanart/posters never evict

	QPalette pal = app.palette();

	// Fusion's default AlternateBase is a blue tint — replace with a neutral grey.
	pal.setColor(QPalette::AlternateBase, pal.color(QPalette::Base).darker(106));

	// Dark Fusion: unfocused selections and progress bars default to near-black.
	// Override Inactive::Highlight with a dark blue so unfocused widgets are
	// still recognisably coloured rather than invisible against the background.
	if (pal.color(QPalette::Window).lightness() < 128) {
		const QColor h = pal.color(QPalette::Active, QPalette::Highlight);
		pal.setColor(QPalette::Inactive, QPalette::Highlight,
					 QColor::fromHsv(h.hsvHue(),
									 qMax(80, h.hsvSaturation() - 30),
									 qMax(55, h.value() / 2)));
		// Keep highlighted text readable in inactive state
		pal.setColor(QPalette::Inactive, QPalette::HighlightedText,
					 pal.color(QPalette::Active, QPalette::HighlightedText));
	}

	app.setPalette(pal);

	app.setApplicationName("MediaCurator");
	app.setApplicationVersion("0.9.0");
	app.setOrganizationName("Bleze Software");
	app.setOrganizationDomain("mediacurator.app");

	// Geometry/window state is stored in an INI file via explicit paths constructed
	// from AppSettings::geometryFilePath() — not the registry, not a default-path INI.

	// Pre-render the SVG at every size Windows uses so the taskbar, Alt+Tab,
	// and title bar all get a sharp icon rather than a single blurry rescale.
	// Keep appIcon in outer scope — Qt's WM_SETICON delivery on Windows holds
	// a weak reference to the pixmap data; destroying the icon before the
	// event loop processes the message results in a blank taskbar icon.
	QIcon appIcon;
	{
		QSvgRenderer svg(QStringLiteral(":/icons/app_icon.svg"));
		for (int sz : {16, 24, 32, 48, 64, 128, 256}) {
			QPixmap pm(sz, sz);
			pm.fill(Qt::transparent);
			{ QPainter ip(&pm); svg.render(&ip); }
			appIcon.addPixmap(pm);
		}
	}
	app.setWindowIcon(appIcon);

	// ── Splash screen ─────────────────────────────────────────────────────────
	QSplashScreen splash(buildSplashPixmap(app.applicationVersion()),
	                     Qt::WindowStaysOnTopHint);
	splash.show();
	app.processEvents();   // ensure the splash paints before we block on DB

	// ── Startup ───────────────────────────────────────────────────────────────
	Mc::AppSettings::instance().load();   // must be before any UI reads settings

	if (!Mc::DatabaseManager::instance().open()) {
		splash.hide();
		QMessageBox::critical(nullptr, "MediaCurator",
			"Failed to open database. The application cannot start.");
		return 1;
	}

	// Reset any jobs left as 'running' from a previous crash and delete their temp files
	Mc::DatabaseManager::instance().cleanupStalledJobs();

	Mc::McMainWindow window;
	window.show();
	// Close the splash right away, before anything else can pump the event queue.
	// McMainWindow::showEvent() schedules the first-run onboarding dialog via a
	// 0 ms QTimer; if app.processEvents() below ran first, it would dispatch that
	// timer and enter the dialog's modal event loop before this line ever ran,
	// leaving the splash on screen for as long as onboarding stayed open.
	splash.finish(&window);
	// Set the icon after show() so the native HWND exists when WM_SETICON is sent.
	// Also set via windowHandle() which goes directly to QWindowsWindow and
	// correctly updates both ICON_SMALL (title bar) and ICON_BIG (taskbar / Alt+Tab).
	app.processEvents();   // flush native window creation so windowHandle() is valid
	window.setWindowIcon(appIcon);
	if (QWindow* wh = window.windowHandle())
		wh->setIcon(appIcon);

	return app.exec();
}
