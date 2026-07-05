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
	// Quick scan skips any folder that already contains at least one known file —
	// it only walks brand-new folders (newly added movies), so it finishes much faster
	// than a full scan but won't notice files added into already-scanned folders.
	void setQuickScan(bool quick) { m_quickScan = quick; }
	void cancel() { m_cancelled.storeRelaxed(1); }

	// File extensions considered as video files
	static const QStringList& videoExtensions();

	// Discover subtitle sidecar files next to a video file and return synthetic StreamRecords.
	// startIndex is the first stream_index to assign (should be result.streams.size() so indices don't collide).
	static QList<StreamRecord> scanSidecarSubtitles(const QString& videoPath, int startIndex);

public slots:
	void run();

signals:
	void progress(int current, int total, const QString& currentFile);
	void fileProcessed(Mc::FileRecord file, QList<Mc::StreamRecord> streams);
	void imdbIdFound(qint64 fileId, QString imdbId);
	void fileRemoved(qint64 fileId);
	void finished(int scanned, int added, int updated, int failed, int skipped, int removed, QStringList newFiles);
	void error(const QString& message);

private:
	QString        m_rootPath;
	QString        m_ffprobePath;
	bool           m_quickScan{false};
	QAtomicInt     m_cancelled{0};
};

} // namespace Mc
