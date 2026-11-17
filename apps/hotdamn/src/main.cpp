#include "HotDamnSettings.h"
#include "McHotDamnUpdater.h"
#include "McHotDamnWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QStyleFactory>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// Hands the file to an already-running instance. False if there is none.
bool sendToRunningInstance(const QString& path)
{
	QLocalSocket socket;
	socket.connectToServer(HotDamn::kInstanceServerName);
	if (!socket.waitForConnected(300))
		return false;
#ifdef Q_OS_WIN
	// Windows only lets the foreground process hand focus on, and that is us.
	AllowSetForegroundWindow(ASFW_ANY);
#endif
	socket.write(path.toUtf8());
	socket.waitForBytesWritten(1000);
	socket.disconnectFromServer();
	if (socket.state() != QLocalSocket::UnconnectedState)
		socket.waitForDisconnected(1000);
	return true;
}

} // namespace

int main(int argc, char* argv[])
{
	QApplication app(argc, argv);
	app.setApplicationName("HotDamn");
	app.setApplicationDisplayName("Hot DAMN NFO Viewer");
	app.setApplicationVersion(HOTDAMN_VERSION);
	app.setOrganizationName("Bleze Software");
	app.setStyle(QStyleFactory::create("Fusion"));
	app.setWindowIcon(QIcon(":/hotdamn.png"));

	QCommandLineParser parser;
	parser.setApplicationDescription("Viewer for scene-release .nfo files");
	parser.addHelpOption();
	parser.addVersionOption();
	parser.addPositionalArgument("file", "NFO file to open");
	const QCommandLineOption newWindow("new-window", "Open in a new window even when one is already running.");
	parser.addOption(newWindow);
	parser.process(app);

	const QStringList files = parser.positionalArguments();
	// Absolute, since a running instance may have a different working directory.
	const QString file = files.isEmpty() ? QString() : QFileInfo(files.first()).absoluteFilePath();

	if (!parser.isSet(newWindow) && QSettings().value(HotDamn::kKeyReuseWindow, true).toBool()
	    && sendToRunningInstance(file))
		return 0;

	int rc = 0;
	QString pendingInstaller;
	{
		Mc::McHotDamnWindow window;
		if (file.isEmpty() || !window.openFile(file))
			window.showWelcome();
		window.show();

		// Listens whatever the setting says: the setting is read by the instance
		// being started, so turning it on later still finds this window.
		QLocalServer server;
		server.listen(HotDamn::kInstanceServerName);
		QObject::connect(&server, &QLocalServer::newConnection, &window, [&server, &window]() {
			while (QLocalSocket* socket = server.nextPendingConnection()) {
				QObject::connect(socket, &QLocalSocket::disconnected, &window, [socket, &window]() {
					window.openFromOtherInstance(QString::fromUtf8(socket->readAll()));
					socket->deleteLater();
				});
			}
		});

		rc = app.exec();
		pendingInstaller = window.pendingInstallerPath();
	}

	// Only now, with the window destroyed, is the update installer started
	// (same order as MediaCurator's main.cpp). Its wizard shows its license
	// page before copying anything, by which time this process has exited.
	if (!pendingInstaller.isEmpty())
		Mc::McHotDamnUpdater::instance().launchInstaller(pendingInstaller);
	return rc;
}
