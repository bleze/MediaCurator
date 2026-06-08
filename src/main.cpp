#include "ui/McMainWindow.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"

#include <QApplication>
#include <QSettings>
#include <QIcon>
#include <QCoreApplication>
#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QProxyStyle>
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

int main(int argc, char* argv[])
{
	QApplication app(argc, argv);
	app.setStyle(new McAppStyle(QStyleFactory::create("Fusion")));

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

	// Keep remaining QSettings (geometry / window state) in a plain INI file
	// rather than the Windows registry.
	QSettings::setDefaultFormat(QSettings::IniFormat);

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
	QPixmap splashPx(480, 260);
	splashPx.fill(QColor(0x1a, 0x1a, 0x2e));

	QPainter sp(&splashPx);
	sp.setRenderHint(QPainter::Antialiasing);

	// App icon — rendered via QSvgRenderer so it scales cleanly
	{
		constexpr int kIconSize = 72;
		constexpr int kIconX    = (480 - kIconSize) / 2;
		constexpr int kIconY    = 18;
		QSvgRenderer iconRenderer(QString(":/icons/app_icon.svg"));
		iconRenderer.render(&sp, QRectF(kIconX, kIconY, kIconSize, kIconSize));
	}

	// Title
	QFont titleFont("Segoe UI", 28, QFont::Light);
	sp.setFont(titleFont);
	sp.setPen(QColor(0xe0, 0xe0, 0xff));
	sp.drawText(QRect(0, 98, 480, 50), Qt::AlignHCenter | Qt::AlignVCenter,
				"MediaCurator");

	// Subtitle
	QFont subFont("Segoe UI", 10);
	sp.setFont(subFont);
	sp.setPen(QColor(0x80, 0x80, 0xb0));
	sp.drawText(QRect(0, 152, 480, 26), Qt::AlignHCenter | Qt::AlignVCenter,
				"Loading library…");

	// Bleze Software
	sp.setFont(QFont("Segoe UI", 8));
	sp.setPen(QColor(0x50, 0x50, 0x70));
	sp.drawText(QRect(10, 222, 460, 22), Qt::AlignLeft | Qt::AlignVCenter,
				"Bleze Software");

	// Version
	sp.drawText(QRect(10, 222, 460, 22), Qt::AlignRight | Qt::AlignVCenter,
				QString("v%1").arg(app.applicationVersion()));
	sp.end();

	QSplashScreen splash(splashPx, Qt::WindowStaysOnTopHint);
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
	// Set the icon after show() so the native HWND exists when WM_SETICON is sent.
	// Also set via windowHandle() which goes directly to QWindowsWindow and
	// correctly updates both ICON_SMALL (title bar) and ICON_BIG (taskbar / Alt+Tab).
	app.processEvents();   // flush native window creation so windowHandle() is valid
	window.setWindowIcon(appIcon);
	if (QWindow* wh = window.windowHandle())
		wh->setIcon(appIcon);
	splash.finish(&window);   // cross-fade out once the window is ready

	return app.exec();
}
