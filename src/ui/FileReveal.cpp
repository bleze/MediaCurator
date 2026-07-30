#include "ui/FileReveal.h"

#include "core/AppSettings.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

#ifdef Q_OS_WIN
#include <QProcess>
#endif

namespace Mc {

void revealInFileManager(const QString& path)
{
	const bool forceExplorer =
	    AppSettings::instance().value("settings/alwaysUseExplorerForReveal", false).toBool();

#ifdef Q_OS_WIN
	if (forceExplorer) {
		QProcess::startDetached(QStringLiteral("explorer.exe"),
		                        { QStringLiteral("/select,"), QDir::toNativeSeparators(path) });
		return;
	}
#endif

	QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

} // namespace Mc
