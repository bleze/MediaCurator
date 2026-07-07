#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

class QProcess;

namespace Mc {

class RemuxJob : public QObject
{
	Q_OBJECT
public:
	explicit RemuxJob(qint64 jobId, const QString& mkvmergePath,
	                  const QStringList& args,
	                  const QString& sourceFilePath,
	                  const QString& descriptionText = {},
	                  bool writeLog = false,
	                  // Explicit final destination — set only when the -o target in
	                  // `args` is a local staging path rather than sitting next to
	                  // the real destination. Empty means "derive from -o as before".
	                  const QString& finalOutputPathOverride = {},
	                  QObject* parent = nullptr);
	~RemuxJob() override;

	void run();
	void cancel();

	qint64  jobId()              const { return m_jobId; }
	QString finalOutputPath()    const { return m_finalOutputPath; }
	QString tmpOutputPath()      const { return m_outputPath; }
	QString inputFilePath()      const { return m_inputPath; }
	bool    hasTrackMismatch()   const { return m_hasTrackMismatch; }
	bool    wasStaged()          const { return m_stagedLocally; }

	// Chunked copy with progress callback and cooperative cancellation, shared by
	// the staged-finish path here and by JobQueue::commitReview's staged path.
	// onProgress(percent, bytesCopied) fires whenever the integer percent changes.
	// Returns false on I/O error or if *cancelRequested became true mid-copy.
	static bool copyFileWithProgress(const QString& srcPath, const QString& dstPath,
	                                  qint64 totalBytes,
	                                  const std::function<void(int, qint64)>& onProgress,
	                                  const std::atomic<bool>* cancelRequested = nullptr);

signals:
	// outputBytes is read synchronously, right at the point percent is parsed from
	// mkvmerge's stdout — the two must be paired atomically, otherwise the UI has no
	// way to know the size figure it's holding corresponds to this exact percent.
	void progressChanged(int percent, qint64 outputBytes);
	// Emitted once when the job moves from muxing into a distinct sub-phase (e.g.
	// copying the staged output back to its real destination) — UI relabels its
	// progress pill using this instead of the default "Running" text.
	void phaseChanged(const QString& label);
	void finished(int exitCode, const QString& log, qint64 savedBytes);

private slots:
	void onReadyRead();
	void onProcessFinished(int exitCode);

private:
	qint64      m_jobId;
	QString     m_mkvmergePath;
	QStringList m_args;
	QString     m_outputPath;        // temp file path (value after -o in args)
	QString     m_finalOutputPath;   // final destination (m_outputPath with .tmp stripped, or the override)
	QString     m_inputPath;         // real source file path (ISO prefix stripped)
	qint64      m_originalSize = 0;
	QByteArray  m_readBuf;          // partial line accumulator — mkvmerge uses \r not \n
	QString     m_log;
	QString     m_descriptionText;
	bool        m_writeLog          = false;
	bool        m_hasTrackMismatch  = false;
	bool        m_stagedLocally     = false;
	std::atomic<bool> m_cancelRequested{false};
	QProcess*   m_process           = nullptr;
};

} // namespace Mc
