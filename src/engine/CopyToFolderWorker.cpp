#include "engine/CopyToFolderWorker.h"

#include <QFile>
#include <QFileInfo>

namespace Mc {

CopyToFolderWorker::CopyToFolderWorker(QList<FileRecord> files, QString destDir, QObject* parent)
	: QObject(parent)
	, m_files(std::move(files))
	, m_destDir(std::move(destDir))
{}

void CopyToFolderWorker::run()
{
	const int total = m_files.size();
	int copied = 0, failed = 0;

	for (int i = 0; i < total; ++i) {
		if (m_cancelled.loadRelaxed()) break;
		const FileRecord& file = m_files.at(i);
		emit progress(i + 1, total, file.filename);

		const QString destPath = m_destDir + QLatin1Char('/') + QFileInfo(file.path).fileName();
		// Never overwrite — a same-named file already at the destination is treated
		// as a failure rather than silently clobbered.
		if (QFile::exists(destPath) || !QFile::copy(file.path, destPath))
			++failed;
		else
			++copied;
	}

	emit finished(copied, failed);
}

} // namespace Mc
