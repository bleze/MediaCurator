#include "ui/McMainWindow.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"

#include <QHash>
#include <QMetaType>
#include "core/Version.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QIcon>
#include <QCoreApplication>
#include <QColor>
#include <QDateTime>
#include <QFile>
#include <QFont>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPixmapCache>
#include <QProcess>
#include <QProxyStyle>
#include <QRandomGenerator>
#include <QSplashScreen>
#include <QStyleFactory>
#include <QSvgRenderer>
#include <QTextStream>
#include <QWindow>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shlobj.h>
#endif

// Temporary instrumentation for the "auto-update finish-page Run checkbox
// doesn't restart the app" regression (2026-07-23). Writes to its own plain-text
// file (not qDebug — release builds have no attached console) so the sequence
// of events across an update-triggered shutdown + relaunch can be inspected
// after the fact. Gated on the MC_DEBUG_LOG env var so it stays silent for
// every shipped install except a machine that explicitly opts in — set it once
// via `setx MC_DEBUG_LOG 1` on the dev box, not shipped/enabled by default.
// Remove entirely once the root cause is confirmed.
static void logRestartDebug(const QString& line)
{
	if (!qEnvironmentVariableIsSet("MC_DEBUG_LOG")) return;
	// TempLocation doesn't depend on QCoreApplication::applicationName/
	// organizationName being set yet — this needs to work from the very first
	// line of main(), before app.setApplicationName() runs.
	static const QString logPath = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
	                                + QStringLiteral("/MediaCurator-restart_debug.log");
	QFile f(logPath);
	if (f.open(QIODevice::Append | QIODevice::Text)) {
		QTextStream ts(&f);
		ts << QDateTime::currentDateTime().toString(Qt::ISODate) << "  " << line << "\n";
	}
}

// Qt widgets on Windows erase their native HWND to white before the first
// QPainter frame. In dark mode that flash is visible across the whole main
// window (not just the splash handoff) until delegates paint.
#ifdef Q_OS_WIN
class McDarkEraseFilter : public QAbstractNativeEventFilter {
public:
	bool nativeEventFilter(const QByteArray& eventType, void* message,
	                       qintptr* result) override
	{
		if (eventType != "windows_generic_MSG"
		    && eventType != "windows_dispatcher_MSG")
			return false;

		const auto* msg = static_cast<MSG*>(message);
		if (msg->message != WM_ERASEBKGND)
			return false;

		const QPalette& pal = qApp->palette();
		if (pal.color(QPalette::Window).lightness() >= 128)
			return false;

		const QColor bg = pal.color(QPalette::Window);
		HBRUSH brush = CreateSolidBrush(RGB(bg.red(), bg.green(), bg.blue()));
		RECT rect;
		GetClientRect(msg->hwnd, &rect);
		FillRect(reinterpret_cast<HDC>(msg->wParam), &rect, brush);
		DeleteObject(brush);
		if (result)
			*result = 1;
		return true;
	}
};
#endif

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

		// Sample of the Job Queue card's live size bar: green (mkvmerge progress,
		// complete) fills the full width, blue (output bytes vs. original size) sits
		// on top and stops short by the saved fraction, exposing a green sliver.
		constexpr int LW = 220, LH = 3;
		const int lx = (W - LW) / 2;
		constexpr int ly = 170;
		constexpr double kSavedFraction = 0.10;
		sp.fillRect(lx, ly, LW, LH, QColor(0x30, 0x30, 0x40));
		sp.fillRect(lx, ly, LW, LH, QColor(0x5a, 0xe8, 0x5a));
		sp.fillRect(lx, ly, qRound(LW * (1.0 - kSavedFraction)), LH, QColor(0x64, 0xb4, 0xf0));
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
	// Belt-and-suspenders for these custom typedefs used as queued signal args.
	// Not strictly required with pointer-to-member connect() (Q_DECLARE_METATYPE
	// already lets qMetaTypeId<T>() self-register on first use), but cheap to keep.
	// If kept, string names must match what MOC puts in signal signatures.
	qRegisterMetaType<Mc::FileRecord>("Mc::FileRecord");
	qRegisterMetaType<Mc::StreamRecordList>("QList<Mc::StreamRecord>");
	qRegisterMetaType<Mc::FileRecordList>("QList<Mc::FileRecord>");
	qRegisterMetaType<Mc::FileStreamMap>("Mc::FileStreamMap");
	qRegisterMetaType<Mc::FileIdList>("QList<qint64>");

	QApplication app(argc, argv);

#ifdef Q_OS_WIN
	logRestartDebug(QStringLiteral("main() start pid=%1 elevated=%2")
	                    .arg(GetCurrentProcessId())
	                    .arg(IsUserAnAdmin() ? "yes" : "no"));
#endif

	// ── Single instance guard ────────────────────────────────────────────────
	// A second process reading/writing the same SQLite DB (jobs, scans, subtitle
	// downloads) races the first and can stomp its state — e.g. cleanupStalledJobs()
	// below would wrongly reset a job the other instance has legitimately marked
	// "running". If another MediaCurator already answers on this local socket,
	// ask it to raise its window and exit immediately instead of opening a second one.
	static const QString kSingleInstanceServerName = QStringLiteral("MediaCurator-SingleInstance");
	{
		QLocalSocket probe;
		probe.connectToServer(kSingleInstanceServerName);
		if (probe.waitForConnected(200)) {
			probe.write("activate");
			probe.waitForBytesWritten(200);
			logRestartDebug(QStringLiteral("found a live instance answering — forwarded activate, exiting now"));
			return 0;
		}
		logRestartDebug(QStringLiteral("single-instance probe: no live instance answered (error=%1)")
		                    .arg(probe.errorString()));
	}
	// No live instance answered — become the primary. Remove any stale socket a
	// crashed previous instance may have left behind before listening for real.
	QLocalServer::removeServer(kSingleInstanceServerName);
	auto* singleInstanceServer = new QLocalServer(&app);
	singleInstanceServer->listen(kSingleInstanceServerName);
	// Set once McMainWindow is constructed below; captured by reference so the
	// newConnection handler (which can fire before or after that point) always
	// sees the current value.
	Mc::McMainWindow* activeMainWindow = nullptr;
	QObject::connect(singleInstanceServer, &QLocalServer::newConnection, [singleInstanceServer, &activeMainWindow]() {
		QLocalSocket* client = singleInstanceServer->nextPendingConnection();
		if (!client) return;
		QObject::connect(client, &QLocalSocket::readyRead, [client, &activeMainWindow]() {
			client->readAll();
			if (activeMainWindow) {
				if (activeMainWindow->isMinimized())
					activeMainWindow->showNormal();
				activeMainWindow->raise();
				activeMainWindow->activateWindow();
			}
		});
		QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
	});

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

#ifdef Q_OS_WIN
	if (pal.color(QPalette::Window).lightness() < 128) {
		static McDarkEraseFilter darkEraseFilter;
		app.installNativeEventFilter(&darkEraseFilter);
	}
#endif

	// QListView's viewport uses the native HWND background on Windows until the
	// first delegate paint — force dark surfaces app-wide when in dark mode.
	if (pal.color(QPalette::Window).lightness() < 128) {
		const QString win  = pal.color(QPalette::Window).name();
		const QString base = pal.color(QPalette::Base).name();
		app.setStyleSheet(QStringLiteral(
		    "QMainWindow, QWidget#qt_scrollarea_viewport { background-color: %1; }"
		    "QAbstractScrollArea::viewport { background-color: %2; }"
		    "QListView { background-color: %2; border: none; }"
		).arg(win, base));
	}

	app.setApplicationName("MediaCurator");
	app.setApplicationVersion(MC_VERSION_STRING);
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
	logRestartDebug(QStringLiteral("splash shown"));

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

	int  rc               = 0;
	bool shutdownRequested = false;
	{
		Mc::McMainWindow window;
		activeMainWindow = &window;
		window.attachSplash(&splash, appIcon);
		window.setSingleInstanceLockReleaser([singleInstanceServer]() {
			singleInstanceServer->close();
			QLocalServer::removeServer(kSingleInstanceServerName);
		});

		logRestartDebug(QStringLiteral("main window constructed, entering event loop"));
		rc = app.exec();
		shutdownRequested = window.shutdownRequested();
		logRestartDebug(QStringLiteral("event loop exited rc=%1 shutdownRequested=%2")
		                    .arg(rc).arg(shutdownRequested ? "yes" : "no"));
	}
	logRestartDebug(QStringLiteral("main window destroyed, main() about to return"));
	// window is fully destroyed here — only now is it safe to tell the OS to shut
	// down. Firing this any earlier (e.g. from closeEvent()) starts Windows'
	// session-end sequence while our own process is still unwinding, which can
	// deliver a second WM_QUERYENDSESSION into a half-destroyed window and crash.
	if (shutdownRequested) {
#ifdef Q_OS_WIN
		QProcess::startDetached("shutdown", {"/s", "/t", "0"});
#elif defined(Q_OS_MACOS)
		QProcess::startDetached("osascript", {"-e", "tell app \"System Events\" to shut down"});
#else
		QProcess::startDetached("systemctl", {"poweroff"});
#endif
	}
	return rc;
}
