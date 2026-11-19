#include "McHotDamnWindow.h"
#include "McHotDamnUpdater.h"
#include "HotDamnSettings.h"
#include "donate/McDonateDialog.h"
#include "nfoview/McNfoView.h"
#include "nfoview/NfoText.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QMimeData>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <QStatusBar>
#include <QTextBrowser>
#include <QUrl>

namespace Mc {

namespace {
constexpr qint64 kMaxFileBytes    = 8 * 1024 * 1024;
const char*      kKeyLoadImages   = "view/loadLinkedImages";
const char*      kKeyCopyOnSelect = "options/copyOnSelect";
const char*      kKeyAlwaysOnTop  = "options/alwaysOnTop";

QString firstLocalFile(const QMimeData* mime)
{
	if (!mime->hasUrls()) return {};
	for (const QUrl& url : mime->urls()) {
		if (url.isLocalFile())
			return url.toLocalFile();
	}
	return {};
}

QString welcomeText()
{
	const QStringList lines = {
		QString(),
		QStringLiteral("H O T   D A M N"),
		QStringLiteral("N F O   V i e w e r   %1").arg(QStringLiteral(HOTDAMN_VERSION)),
		QString(),
		QStringLiteral("Drop an .nfo file here, or press Ctrl+O to open one."),
		QString(),
		QStringLiteral("CP437 art, UTF-8 and Latin-1 are detected automatically."),
		QStringLiteral("Linked screenshots are shown inline (View menu to turn off)."),
		QString(),
	};
	constexpr int kInner = 66;
	QString out = QStringLiteral("\n  ╔") + QString(kInner, QChar(0x2550)) + QStringLiteral("╗\n");
	for (const QString& line : lines) {
		const int left = (kInner - line.size()) / 2;
		out += QStringLiteral("  ║") + QString(left, QLatin1Char(' ')) + line
		       + QString(kInner - left - line.size(), QLatin1Char(' ')) + QStringLiteral("║\n");
	}
	out += QStringLiteral("  ╚") + QString(kInner, QChar(0x2550)) + QStringLiteral("╝\n");
	out += QStringLiteral("   ░▒▓█ inspired by DAMN NFO Viewer, built to be better █▓▒░\n");
	return out;
}
} // namespace

McHotDamnWindow::McHotDamnWindow(QWidget* parent)
	: QMainWindow(parent)
{
	setAcceptDrops(true);

	m_view = new McNfoView(this);
	m_view->setUserAgent("HotDamn/" HOTDAMN_VERSION);
	// Drops go to the window, not the (read-only) text widget.
	m_view->browser()->setAcceptDrops(false);
	m_view->browser()->setFrameShape(QFrame::NoFrame);
	setCentralWidget(m_view);

	if (const QScreen* scr = screen())
		m_view->setMaxImageSide(qBound(200, scr->availableGeometry().width() - 200, 1000));

	const QSettings settings;
	m_view->setInlineImagesEnabled(settings.value(kKeyLoadImages, true).toBool());
	m_copyOnSelect = settings.value(kKeyCopyOnSelect, false).toBool();
	m_view->browser()->viewport()->installEventFilter(this);
	setWindowFlag(Qt::WindowStaysOnTopHint, settings.value(kKeyAlwaysOnTop, false).toBool());

	buildMenus();
	buildStatusBar();
	connectUpdater();
	// Once the window is up; at most once a day (see McHotDamnUpdater::check).
	QTimer::singleShot(2000, this, []() { McHotDamnUpdater::instance().check(/*silent=*/true); });
	connect(m_view, &McNfoView::contentGrew, this, [this]() { fit(/*growOnly=*/true); });
	connect(m_view, &McNfoView::cellSizeChanged, this, [this]() { fit(/*growOnly=*/false); });
	m_view->browser()->setFocus();
}

void McHotDamnWindow::buildMenus()
{
	const QSettings settings;
	auto addToggle = [this](QMenu* menu, const QString& text, bool checked, auto onToggled) {
		QAction* act = menu->addAction(text);
		act->setCheckable(true);
		act->setChecked(checked);
		connect(act, &QAction::toggled, this, onToggled);
		return act;
	};

	QMenu* file = menuBar()->addMenu(tr("&File"));
	file->addAction(tr("&Open..."), QKeySequence::Open, this, &McHotDamnWindow::chooseFile);
	m_reloadAct = file->addAction(tr("&Reload"), QKeySequence(Qt::Key_F5), this, [this]() { openFile(m_path); });
	m_revealAct = file->addAction(tr("Show in &Folder"), this, &McHotDamnWindow::revealInFolder);
	file->addSeparator();
	file->addAction(tr("E&xit"), QKeySequence(Qt::Key_Escape), this, &QWidget::close);
	m_reloadAct->setEnabled(false);
	m_revealAct->setEnabled(false);

	QMenu* edit = menuBar()->addMenu(tr("&Edit"));
	edit->addAction(tr("&Copy"), QKeySequence::Copy, m_view->browser(), &QTextBrowser::copy);
	edit->addAction(tr("Select &All"), QKeySequence::SelectAll, m_view->browser(), &QTextBrowser::selectAll);

	// Zoom, font, colors and shading belong to the shared view (and are also on
	// its right-click menu); only window fitting is this app's own.
	QMenu* view = menuBar()->addMenu(tr("&View"));
	view->addActions(m_view->viewActions());
	view->addSeparator();
	view->addAction(tr("&Fit Window to NFO"), QKeySequence(Qt::CTRL | Qt::Key_F), this,
	                [this]() { fit(/*growOnly=*/false); });

	QMenu* options = menuBar()->addMenu(tr("&Options"));
	addToggle(options, tr("&Reuse Window"), settings.value(HotDamn::kKeyReuseWindow, true).toBool(),
	          [](bool on) { QSettings().setValue(HotDamn::kKeyReuseWindow, on); });
	addToggle(options, tr("&Copy on Select"), m_copyOnSelect, [this](bool on) {
		QSettings().setValue(kKeyCopyOnSelect, on);
		m_copyOnSelect = on;
	});
	addToggle(options, tr("Always on &Top"), windowFlags().testFlag(Qt::WindowStaysOnTopHint), [this](bool on) {
		QSettings().setValue(kKeyAlwaysOnTop, on);
		setAlwaysOnTop(on);
	});
	addToggle(options, tr("Load &Linked Images"), m_view->inlineImagesEnabled(), [this](bool on) {
		QSettings().setValue(kKeyLoadImages, on);
		m_view->setInlineImagesEnabled(on);
	});
#ifdef Q_OS_WIN
	options->addSeparator();
	options->addAction(tr("Set as &Default NFO Viewer..."), this, &McHotDamnWindow::openDefaultAppsSettings);
#endif

	QMenu* help = menuBar()->addMenu(tr("&Help"));
	m_checkUpdatesAct = help->addAction(tr("Check for &Updates..."), this, &McHotDamnWindow::checkForUpdates);
	help->addAction(tr("&Donate..."), this, [this]() {
		McDonateDialog dlg(tr("Hot DAMN NFO Viewer"),
		                   tr("If Hot DAMN NFO Viewer made your NFOs look the way they were meant to, "
		                      "consider saying thanks or supporting continued development."),
		                   this);
		dlg.exec();
	});
	help->addSeparator();
	help->addAction(tr("&About Hot DAMN NFO Viewer"), this, &McHotDamnWindow::showAbout);
}

void McHotDamnWindow::openFromOtherInstance(const QString& path)
{
	if (!path.isEmpty())
		openFile(path);
	if (isMinimized())
		showNormal();
	raise();
	activateWindow();
}

void McHotDamnWindow::setAlwaysOnTop(bool on)
{
	// Changing a window flag hides the window; show it again if it was up.
	const bool wasVisible = isVisible();
	setWindowFlag(Qt::WindowStaysOnTopHint, on);
	if (wasVisible)
		show();
}

void McHotDamnWindow::openDefaultAppsSettings()
{
	// Windows 10/11 only let the user change a default app. This opens the
	// Default apps page — on Windows 11 straight at this app's entry, which
	// exists once the installer has registered it.
	QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:defaultapps?registeredAppMachine=HotDamn")));
}

void McHotDamnWindow::buildStatusBar()
{
	// Path on the left (elided to fit), then modified time, file size and the
	// text's size in character cells.
	m_statusPath = new QLabel(this);
	m_statusPath->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	statusBar()->addWidget(m_statusPath, 1);
	for (QLabel** label : { &m_statusDamage, &m_statusDate, &m_statusBytes, &m_statusGrid }) {
		*label = new QLabel(this);
		statusBar()->addPermanentWidget(*label);
	}
	m_statusGrid->setToolTip(tr("Columns × rows"));
	m_statusDamage->setToolTip(tr("This file was re-saved in the wrong encoding at some point, which destroyed these "
	                              "characters. They are shown as grey cells; the original file is the only fix."));
}

void McHotDamnWindow::updateStatus(const QString& modified, const QString& bytes)
{
	const qsizetype lost = m_path.isEmpty() ? 0 : NfoText::lostCharacterCount(m_view->text());
	m_statusDamage->setText(lost > 0 ? tr("Damaged file: %1 characters lost").arg(QLocale().toString(lost)) : QString());
	m_statusDamage->setVisible(lost > 0);
	m_statusDate->setText(modified);
	m_statusBytes->setText(bytes);
	if (m_path.isEmpty()) {
		m_statusGrid->clear();
	} else {
		// Rows are lines of text: an empty file has none, and a final line
		// break ends the last line rather than starting another.
		const QString text = m_view->text();
		QStringList lines;
		if (!text.isEmpty()) {
			lines = text.split(QLatin1Char('\n'));
			if (text.endsWith(QLatin1Char('\n')))
				lines.removeLast();
		}
		qsizetype columns = 0;
		for (const QString& line : lines)
			columns = qMax(columns, line.size());
		m_statusGrid->setText(tr("%1 × %2").arg(columns).arg(lines.size()));
	}
	m_statusPath->setToolTip(QDir::toNativeSeparators(m_path));
	elideStatusPath();
}

void McHotDamnWindow::elideStatusPath()
{
	m_statusPath->setText(m_statusPath->fontMetrics().elidedText(
		QDir::toNativeSeparators(m_path), Qt::ElideMiddle, m_statusPath->width()));
}

void McHotDamnWindow::resizeEvent(QResizeEvent* event)
{
	QMainWindow::resizeEvent(event);
	// Queued: the status bar lays its labels out after this event.
	QMetaObject::invokeMethod(this, &McHotDamnWindow::elideStatusPath, Qt::QueuedConnection);
}

bool McHotDamnWindow::eventFilter(QObject* watched, QEvent* event)
{
	if (m_copyOnSelect && event->type() == QEvent::MouseButtonRelease
	    && watched == m_view->browser()->viewport() && m_view->browser()->textCursor().hasSelection())
		m_view->browser()->copy();
	return QMainWindow::eventFilter(watched, event);
}

bool McHotDamnWindow::openFile(const QString& path)
{
	if (path.isEmpty()) return false;
	const QFileInfo info(path);
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(this, tr("Hot DAMN NFO Viewer"),
		                     tr("Could not open %1:\n%2").arg(QDir::toNativeSeparators(path), file.errorString()));
		return false;
	}
	if (file.size() > kMaxFileBytes) {
		QMessageBox::warning(this, tr("Hot DAMN NFO Viewer"),
		                     tr("%1 is %2 MB, which is too large to be an NFO.")
		                         .arg(info.fileName())
		                         .arg(file.size() / (1024 * 1024)));
		return false;
	}

	m_path = info.absoluteFilePath();
	const qint64 bytes = file.size();
	m_view->setText(NfoText::decodeBytes(file.readAll()));
	setWindowTitle(tr("%1 — Hot DAMN NFO Viewer").arg(info.fileName()));
	updateStatus(QLocale().toString(info.lastModified(), QLocale::ShortFormat),
	             tr("%1 bytes").arg(QLocale().toString(bytes)));
	m_reloadAct->setEnabled(true);
	m_revealAct->setEnabled(true);
	fit(/*growOnly=*/false);
	return true;
}

void McHotDamnWindow::showWelcome()
{
	m_path.clear();
	m_view->setText(welcomeText());
	setWindowTitle(tr("Hot DAMN NFO Viewer"));
	updateStatus(QString(), QString());
	m_reloadAct->setEnabled(false);
	m_revealAct->setEnabled(false);
	fit(/*growOnly=*/false);
}

void McHotDamnWindow::chooseFile()
{
	const QString dir  = m_path.isEmpty() ? QString() : QFileInfo(m_path).absolutePath();
	const QString path = QFileDialog::getOpenFileName(
		this, tr("Open NFO"), dir, tr("NFO files (*.nfo *.diz *.asc *.txt);;All files (*)"));
	if (!path.isEmpty())
		openFile(path);
}

void McHotDamnWindow::revealInFolder()
{
	if (m_path.isEmpty()) return;
#ifdef Q_OS_WIN
	QProcess::startDetached(QStringLiteral("explorer.exe"),
	                        { QStringLiteral("/select,"), QDir::toNativeSeparators(m_path) });
#else
	QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_path).absolutePath()));
#endif
}

void McHotDamnWindow::connectUpdater()
{
	McHotDamnUpdater& updater = McHotDamnUpdater::instance();
	connect(&updater, &McHotDamnUpdater::updateAvailable, this,
	        [this](QString version, QString htmlUrl, QString notes, QString installerUrl, bool) {
		m_checkUpdatesAct->setEnabled(true);
		onUpdateAvailable(version, htmlUrl, notes, installerUrl);
	});
	connect(&updater, &McHotDamnUpdater::upToDate, this, [this](bool silent) {
		m_checkUpdatesAct->setEnabled(true);
		if (silent) return;
		QMessageBox::information(this, tr("Check for Updates"),
		                         tr("You're running the latest version (%1).").arg(QCoreApplication::applicationVersion()));
	});
	connect(&updater, &McHotDamnUpdater::checkFailed, this, [this](QString error, bool silent) {
		m_checkUpdatesAct->setEnabled(true);
		if (silent) return;
		QMessageBox::warning(this, tr("Check for Updates"), tr("Could not check for updates:\n%1").arg(error));
	});
	connect(&updater, &McHotDamnUpdater::downloadProgress, this, [this](qint64 received, qint64 total) {
		if (!m_updateProgress || total <= 0) return;
		m_updateProgress->setMaximum(static_cast<int>(total / 1024));
		m_updateProgress->setValue(static_cast<int>(received / 1024));
	});
	auto closeProgress = [this]() {
		if (!m_updateProgress) return;
		m_updateProgress->close();
		m_updateProgress->deleteLater();
		m_updateProgress = nullptr;
	};
	connect(&updater, &McHotDamnUpdater::downloadFailed, this, [this, closeProgress](QString error) {
		closeProgress();
		QMessageBox::warning(this, tr("Update Failed"), error);
	});
	connect(&updater, &McHotDamnUpdater::installerReady, this, [this, closeProgress](QString path) {
		closeProgress();
		// Not launched here: this process still has HotDamn.exe and its DLLs
		// open. main() starts it once the window has been destroyed.
		m_pendingInstallerPath = path;
		close();
	});
}

void McHotDamnWindow::checkForUpdates()
{
	m_checkUpdatesAct->setEnabled(false);
	McHotDamnUpdater::instance().check(/*silent=*/false);
}

void McHotDamnWindow::onUpdateAvailable(const QString& version, const QString& htmlUrl, const QString& releaseNotes,
                                        const QString& installerUrl)
{
	QMessageBox box(this);
	box.setIcon(QMessageBox::Information);
	box.setWindowTitle(tr("Update Available"));
	box.setText(tr("Hot DAMN NFO Viewer %1 is available (you have %2).")
	                .arg(version, QCoreApplication::applicationVersion()));
	if (!releaseNotes.isEmpty())
		box.setDetailedText(releaseNotes);
	QAbstractButton* updateBtn = nullptr;
#ifdef Q_OS_WIN
	if (!installerUrl.isEmpty())
		updateBtn = box.addButton(tr("Update Now"), QMessageBox::AcceptRole);
#endif
	QAbstractButton* viewBtn = box.addButton(tr("View Release"), QMessageBox::ActionRole);
	QAbstractButton* skipBtn = box.addButton(tr("Skip This Version"), QMessageBox::DestructiveRole);
	box.addButton(tr("Remind Me Later"), QMessageBox::RejectRole);
	box.exec();

	if (updateBtn && box.clickedButton() == updateBtn) {
		m_updateProgress = new QProgressDialog(tr("Downloading Hot DAMN NFO Viewer %1...").arg(version), tr("Cancel"),
		                                       0, 100, this);
		m_updateProgress->setWindowModality(Qt::WindowModal);
		m_updateProgress->setMinimumDuration(0);
		m_updateProgress->setAutoClose(false);
		m_updateProgress->setAutoReset(false);
		connect(m_updateProgress, &QProgressDialog::canceled, this,
		        []() { McHotDamnUpdater::instance().cancelDownload(); });
		m_updateProgress->show();
		McHotDamnUpdater::instance().download(installerUrl);
	} else if (box.clickedButton() == viewBtn) {
		QDesktopServices::openUrl(QUrl(htmlUrl));
	} else if (box.clickedButton() == skipBtn) {
		McHotDamnUpdater::instance().skipVersion(version);
	}
}

void McHotDamnWindow::showAbout()
{
	QMessageBox::about(
		this, tr("About Hot DAMN NFO Viewer"),
		tr("<h3>Hot DAMN NFO Viewer %1</h3>"
	       "<p>A viewer for scene-release <code>.nfo</code> files: CP437 art rendered "
	       "correctly, UTF-8 and Latin-1 detected, linked screenshots shown inline.</p>"
	       "<p>Inspired by DAMN NFO Viewer. Shares its NFO engine with "
	       "<a href=\"https://github.com/bleze/MediaCurator\">MediaCurator</a>.</p>"
	       "<p>Apache License 2.0. Built with Qt %2.</p>")
			.arg(QStringLiteral(HOTDAMN_VERSION), QString::fromLatin1(qVersion())));
}

void McHotDamnWindow::fit(bool growOnly)
{
	// Document margins and scrollbar slack, plus the menu bar above the view.
	const QSize chrome(28, 28 + menuBar()->sizeHint().height() + statusBar()->sizeHint().height());
	m_view->fitWindow(growOnly ? McNfoView::Fit::GrowOnly : McNfoView::Fit::Initial, chrome);
}

void McHotDamnWindow::dragEnterEvent(QDragEnterEvent* event)
{
	if (!firstLocalFile(event->mimeData()).isEmpty())
		event->acceptProposedAction();
}

void McHotDamnWindow::dropEvent(QDropEvent* event)
{
	const QString path = firstLocalFile(event->mimeData());
	if (path.isEmpty()) return;
	event->acceptProposedAction();
	openFile(path);
}

} // namespace Mc
