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
	                  const QString& descriptionText = {},
	                  bool writeLog = false,
	                  QObject* parent = nullptr);
	~RemuxJob() override;

	void run();
	void cancel();

	qint64 jobId()            const { return m_jobId; }
	QString finalOutputPath() const { return m_finalOutputPath; }
	QString inputFilePath()   const { return m_inputPath; }

signals:
	void progressChanged(int percent);
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
	bool        m_writeLog = false;
	QProcess*   m_process = nullptr;
};

} // namespace Mc
