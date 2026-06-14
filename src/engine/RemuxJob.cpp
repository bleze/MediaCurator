#include "engine/RemuxJob.h"

#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>

#include <filesystem>

#ifdef Q_OS_WIN
#include <windows.h>
// Preserve the filesystem creation timestamp of an original file on the output file.
// Called after rename so the new file doesn't show today as its creation date.
static void preserveCreationTime(const QString& target, const QDateTime& origCreated)
{
	if (!origCreated.isValid()) return;
	// QDateTime → FILETIME (100-ns intervals since 1601-01-01 UTC)
	const qint64 ns100 = (origCreated.toMSecsSinceEpoch() + Q_INT64_C(11644473600000)) * 10000;
	FILETIME ft;
	ft.dwLowDateTime  = static_cast<DWORD>(ns100 & 0xFFFFFFFF);
	ft.dwHighDateTime = static_cast<DWORD>((ns100 >> 32) & 0xFFFFFFFF);
	HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(target.utf16()),
	                       FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	SetFileTime(h, &ft, nullptr, nullptr);
	CloseHandle(h);
}
#endif

namespace Mc {

RemuxJob::RemuxJob(qint64 jobId, const QString& mkvmergePath,
				   const QStringList& args,
				   const QString& sourceFilePath,
				   const QString& descriptionText,
				   bool writeLog,
				   QObject* parent)
	: QObject(parent)
	, m_jobId(jobId)
	, m_mkvmergePath(mkvmergePath)
	, m_args(args)
	, m_descriptionText(descriptionText)
	, m_writeLog(writeLog)
{
	// Extract temp output path (args[1] when args[0] == "-o")
	if (args.size() >= 2 && args.first() == QLatin1String("-o"))
		m_outputPath = args.at(1);

	// Strip .tmp suffix to get the final destination path
	m_finalOutputPath = m_outputPath.endsWith(QLatin1String(".tmp"))
		? m_outputPath.left(m_outputPath.length() - 4)
		: m_outputPath;

	// Real input filesystem path — ISO files have a "bluray://" or "dvd://" prefix
	// that mkvmerge needs but the filesystem does not understand.
	// sourceFilePath is captured before any sidecar args are appended to the command,
	// so it always refers to the primary MKV/ISO source rather than a sidecar file.
	if (sourceFilePath.startsWith(QLatin1String("bluray://")))
		m_inputPath = sourceFilePath.mid(9);
	else if (sourceFilePath.startsWith(QLatin1String("dvd://")))
		m_inputPath = sourceFilePath.mid(6);
	else
		m_inputPath = sourceFilePath;
}

RemuxJob::~RemuxJob()
{
	if (m_process && m_process->state() != QProcess::NotRunning) {
		m_process->kill();
		m_process->waitForFinished(3000);
	}
}

void RemuxJob::run()
{
	m_originalSize = QFileInfo(m_inputPath).size();

	m_process = new QProcess(this);
	m_process->setProcessChannelMode(QProcess::MergedChannels);

	connect(m_process, &QProcess::readyRead,
	        this, &RemuxJob::onReadyRead);
	connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
	        this, [this](int code, QProcess::ExitStatus) { onProcessFinished(code); });

	m_process->start(m_mkvmergePath, m_args);
}

void RemuxJob::cancel()
{
	if (m_process && m_process->state() != QProcess::NotRunning)
		m_process->kill();
}

void RemuxJob::onReadyRead()
{
	static const QRegularExpression progressRe(R"(Progress:\s*(\d+)%)");

	m_readBuf += m_process->readAll();

	// mkvmerge uses \r (not \n) to overwrite the progress line in a terminal.
	// QProcess::canReadLine() only fires on \n, so split on both delimiters.
	int pos = 0;
	for (int i = 0; i < m_readBuf.size(); ++i) {
		const char c = m_readBuf.at(i);
		if (c == '\r' || c == '\n') {
			const QString line = QString::fromUtf8(m_readBuf.mid(pos, i - pos)).trimmed();
			pos = i + 1;
			if (line.isEmpty()) continue;
			m_log += line + '\n';
			const auto match = progressRe.match(line);
			if (match.hasMatch())
				emit progressChanged(match.captured(1).toInt());
		}
	}
	m_readBuf = m_readBuf.mid(pos);
}

void RemuxJob::onProcessFinished(int exitCode)
{
	// Drain any bytes that didn't end with \r/\n
	m_readBuf += m_process->readAll();
	if (!m_readBuf.isEmpty()) {
		m_log += QString::fromUtf8(m_readBuf).trimmed() + '\n';
		m_readBuf.clear();
	}

	// Detect the dangerous warning: a requested track ID was not found in the file.
	// This can mean the track layout has shifted since the job was created, which
	// risks removing the wrong tracks. We keep the .tmp and let the user review.
	static const QRegularExpression mismatchRe(
		R"(track with the ID \d+ was requested but not found)",
		QRegularExpression::CaseInsensitiveOption);
	m_hasTrackMismatch = mismatchRe.match(m_log).hasMatch();

	qint64 savedBytes = 0;

	// mkvmerge exit codes: 0 = success, 1 = warnings (output still valid), 2 = error
	if ((exitCode == 0 || exitCode == 1) && !m_outputPath.isEmpty() && !m_inputPath.isEmpty()) {
		if (m_hasTrackMismatch) {
			// Leave .tmp on disk — JobQueue will probe it and ask the user to verify
			// before committing or discarding.
			emit finished(exitCode, m_log, 0);
			return;
		}
		const qint64 outputSize = QFileInfo(m_outputPath).size();

		// Capture the original file's creation timestamp before we rename/delete it
		const QDateTime origCreated = QFileInfo(m_inputPath).birthTime();

		const bool isInPlace = (m_finalOutputPath == m_inputPath);
		try {
			// Rename temp file to final destination (same as input for MKV in-place)
			std::filesystem::rename(m_outputPath.toStdWString(),
			                        m_finalOutputPath.toStdWString());
			// Restore the original file's creation date on the new output
#ifdef Q_OS_WIN
			preserveCreationTime(m_finalOutputPath, origCreated);
#endif
			if (!isInPlace) {
				// Non-MKV conversion (mp4/avi/iso → mkv): delete the original
				QFile::remove(m_inputPath);
			}
			savedBytes = qMax(0LL, m_originalSize - outputSize);
		} catch (const std::filesystem::filesystem_error& e) {
			// On SMB/NFS shares the rename can succeed on disk but still throw
			// (the server commits the operation then returns an ambiguous status).
			// Verify the actual filesystem state before treating it as a failure.
			const bool destExists = QFileInfo::exists(m_finalOutputPath);
			const bool srcGone    = !QFileInfo::exists(m_outputPath);
			if (destExists && srcGone) {
				// Rename succeeded despite the error — treat as success.
				savedBytes = qMax(0LL, m_originalSize - outputSize);
				m_log += QStringLiteral("\nNote: rename reported a network error but the file is in the correct state (%1)").arg(e.what());
			} else {
				m_log += QStringLiteral("\nRename failed: %1").arg(e.what());
				exitCode = -2;
			}
		}

		if ((exitCode == 0 || exitCode == 1) && m_writeLog) {
			const QString logPath = m_finalOutputPath + QStringLiteral(".mc-log");
			QFile logFile(logPath);
			if (logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
				QTextStream ts(&logFile);
				const QFileInfo fi(m_finalOutputPath);
				ts << "MediaCurator Processing Report\n";
				ts << "==============================\n\n";
				ts << "File:    " << fi.fileName() << "\n";
				ts << "Path:    " << fi.absolutePath() << "\n";
				ts << "Date:    " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
				ts << "Before:  " << QString::number(m_originalSize) << " bytes ("
				   << QString::number(m_originalSize / 1048576.0, 'f', 1) << " MB)\n";
				ts << "After:   " << QString::number(outputSize) << " bytes ("
				   << QString::number(outputSize / 1048576.0, 'f', 1) << " MB)\n";
				if (savedBytes > 0)
					ts << "Reclaimed: " << QString::number(savedBytes) << " bytes ("
					   << QString::number(savedBytes / 1048576.0, 'f', 1) << " MB)\n";
				ts << "\n";
				if (!m_descriptionText.isEmpty()) {
					ts << "Removed Tracks\n";
					ts << "--------------\n";
					ts << m_descriptionText << "\n\n";
				}
				ts << "Command\n";
				ts << "-------\n";
				ts << m_mkvmergePath;
				for (const QString& arg : m_args)
					ts << " " << arg;
				ts << "\n";
				logFile.close();
			}
		}
	} else if (exitCode != 0 && !m_outputPath.isEmpty()) {
		QFile::remove(m_outputPath);
	}

	emit finished(exitCode, m_log, savedBytes);
}

} // namespace Mc
