#pragma once

#include "scanner/FfprobeScanner.h"
#include "core/DatabaseManager.h"
#include <QObject>
#include <QString>
#include <QStringList>
#include <QAtomicInt>

namespace Mc {

/**
 * ScanWorker — runs in a QThread, recursively scans a directory for video files,
 * invokes FfprobeScanner for each, and writes results to the DatabaseManager.
 *
 * Usage:
 *   auto* worker = new ScanWorker(ffprobePath);
 *   auto* thread = new QThread;
 *   worker->moveToThread(thread);
 *   connect(thread, &QThread::started, worker, &ScanWorker::run);
 *   connect(worker, &ScanWorker::finished, thread, &QThread::quit);
 *   connect(worker, &ScanWorker::finished, worker, &QObject::deleteLater);
 *   connect(thread, &QThread::finished, thread, &QObject::deleteLater);
 *   worker->setRootPath(path);
 *   thread->start();
 */
class ScanWorker : public QObject
{
	Q_OBJECT
public:
	explicit ScanWorker(const QString& ffprobePath, QObject* parent = nullptr);

	void setRootPath(const QString& path) { m_rootPath = path; }
	void cancel() { m_cancelled.storeRelaxed(1); }

	// File extensions considered as video files
	static const QStringList& videoExtensions();

public slots:
	void run();

signals:
	void progress(int current, int total, const QString& currentFile);
	void fileProcessed(Mc::FileRecord file, QList<Mc::StreamRecord> streams);
	void imdbIdFound(qint64 fileId, QString imdbId);
	void fileRemoved(qint64 fileId);
	void finished(int scanned, int added, int updated, int failed, int skipped, int removed);
	void error(const QString& message);

private:
	QString        m_rootPath;
	QString        m_ffprobePath;
	QAtomicInt     m_cancelled{0};
};

} // namespace Mc
