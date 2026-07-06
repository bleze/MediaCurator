#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

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
	                  QObject* parent = nullptr);
	~RemuxJob() override;

	void run();
	void cancel();

	qint64  jobId()              const { return m_jobId; }
	QString finalOutputPath()    const { return m_finalOutputPath; }
	QString tmpOutputPath()      const { return m_outputPath; }
	QString inputFilePath()      const { return m_inputPath; }
	bool    hasTrackMismatch()   const { return m_hasTrackMismatch; }

signals:
	// outputBytes is read synchronously, right at the point percent is parsed from
	// mkvmerge's stdout — the two must be paired atomically, otherwise the UI has no
	// way to know the size figure it's holding corresponds to this exact percent.
	void progressChanged(int percent, qint64 outputBytes);
	void finished(int exitCode, const QString& log, qint64 savedBytes);

private slots:
	void onReadyRead();
	void onProcessFinished(int exitCode);

private:
	qint64      m_jobId;
	QString     m_mkvmergePath;
	QStringList m_args;
	QString     m_outputPath;        // temp file path (value after -o in args)
	QString     m_finalOutputPath;   // final destination (m_outputPath with .tmp stripped)
	QString     m_inputPath;         // real source file path (ISO prefix stripped)
	qint64      m_originalSize = 0;
	QByteArray  m_readBuf;          // partial line accumulator — mkvmerge uses \r not \n
	QString     m_log;
	QString     m_descriptionText;
	bool        m_writeLog          = false;
	bool        m_hasTrackMismatch  = false;
	QProcess*   m_process           = nullptr;
};

} // namespace Mc
