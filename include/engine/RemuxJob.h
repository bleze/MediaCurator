#pragma once
#include "core/DatabaseManager.h"
#include "engine/ActionEngine.h"
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>

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
	                  // Streams this job is expected to leave behind (e.g.
	                  // ActionEngine::computeKeptStreams() run against the pre-remux
	                  // snapshot) and the ffprobe binary to verify with. Empty
	                  // expectedStreams skips the streams-level verification gate
	                  // (falls back to the mkvmerge-warning regex check only).
	                  const QList<StreamRecord>& expectedStreams = {},
	                  const QString& ffprobePath = {},
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
	// Valid once `finished` has been emitted with an ok exit code — the .tmp
	// output as freshly scanned by the verification gate, and the diff (if any)
	// against expectedStreams. Lets JobQueue reuse this instead of re-scanning.
	QList<StreamRecord>     tmpStreams()  const { return m_tmpStreams; }
	ActionEngine::StreamDiff streamDiff() const { return m_streamDiff; }

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
	void onProcessFinished(int exitCode, QProcess::ExitStatus status);

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
	// Heap-shared so background finalize tasks can hold their own reference —
	// they may outlive this object (app shutdown mid NAS copy-back).
	std::shared_ptr<std::atomic<bool>> m_cancelRequested = std::make_shared<std::atomic<bool>>(false);
	QProcess*   m_process           = nullptr;

	QString                 m_ffprobePath;
	QList<StreamRecord>     m_expectedStreams;
	QList<StreamRecord>     m_tmpStreams;
	ActionEngine::StreamDiff m_streamDiff;
};

} // namespace Mc
