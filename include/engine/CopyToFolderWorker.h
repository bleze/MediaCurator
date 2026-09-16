#pragma once

#include "core/DatabaseManager.h"
#include <QAtomicInt>
#include <QObject>
#include <QString>

namespace Mc {

// Copies the given files to destDir one at a time on a background thread — unlike
// Windows Explorer's multi-file copy (which parallelizes across files and can thrash
// a spinning/NAS drive), this deliberately serializes so only one file is ever being
// read/written at once. See McMainWindow::onCopyCheckedFilesToFolder.
class CopyToFolderWorker : public QObject
{
	Q_OBJECT
public:
	explicit CopyToFolderWorker(QList<FileRecord> files, QString destDir, QObject* parent = nullptr);

	void cancel() { m_cancelled.storeRelaxed(1); }

public slots:
	void run();

signals:
	void progress(int current, int total, const QString& filename);
	void finished(int copied, int failed);

private:
	QList<FileRecord> m_files;
	QString           m_destDir;
	QAtomicInt        m_cancelled{0};
};

} // namespace Mc
