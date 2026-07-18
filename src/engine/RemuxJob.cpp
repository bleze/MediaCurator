#include "engine/RemuxJob.h"
#include "core/ExternalTools.h"
#include "engine/ActionEngine.h"
#include "scanner/FfprobeScanner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QRegularExpression>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>
#include <QThreadPool>

#include <algorithm>
#include <filesystem>

#ifdef Q_OS_WIN
#include <windows.h>
static FILETIME toFileTime(const QDateTime& dt)
{
	const qint64 ns100 = (dt.toMSecsSinceEpoch() + Q_INT64_C(11644473600000)) * 10000;
	FILETIME ft;
	ft.dwLowDateTime  = static_cast<DWORD>(ns100 & 0xFFFFFFFF);
	ft.dwHighDateTime = static_cast<DWORD>((ns100 >> 32) & 0xFFFFFFFF);
	return ft;
}
// Restore both the creation and last-modified timestamps from the original file
// on the output file so the processed file doesn't bubble up as "newest" in
// media libraries that sort by Date Modified.
static void preserveTimestamps(const QString& target, const QDateTime& origCreated, const QDateTime& origModified)
{
	HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(target.utf16()),
	                       FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	FILETIME ftCreated  = origCreated.isValid()  ? toFileTime(origCreated)  : FILETIME{};
	FILETIME ftModified = origModified.isValid() ? toFileTime(origModified) : FILETIME{};
	SetFileTime(h,
	            origCreated.isValid()  ? &ftCreated  : nullptr,
	            nullptr,
	            origModified.isValid() ? &ftModified : nullptr);
	CloseHandle(h);
}
// Same, but for the containing folder: the rename/replace below is a directory-
// entry change (old name removed, new one added even when the filename is
// unchanged — it's still a fresh inode/entry via std::filesystem::rename), which
// bumps the parent directory's own mtime on NTFS. Left uncorrected, every folder
// a job ever completes in would look "changed" to Quick Scan's known-folder
// mtime check (ScanWorker.cpp) forever after, forcing it to re-walk folders that
// didn't actually gain or lose a file — this restores the pre-job mtime so a
// completed job doesn't look like an external change. Mirrors preserveDirTimestamps()
// in NfoParser.cpp / OpenSubtitlesClient.cpp.
static void preserveDirTimestamps(const QString& dirPath, const QDateTime& origCreated, const QDateTime& origModified)
{
	HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(dirPath.utf16()),
	                       FILE_WRITE_ATTRIBUTES,
	                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                       nullptr, OPEN_EXISTING,
	                       FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	FILETIME ftCreated  = origCreated.isValid()  ? toFileTime(origCreated)  : FILETIME{};
	FILETIME ftModified = origModified.isValid() ? toFileTime(origModified) : FILETIME{};
	SetFileTime(h,
	            origCreated.isValid()  ? &ftCreated  : nullptr,
	            nullptr,
	            origModified.isValid() ? &ftModified : nullptr);
	CloseHandle(h);
}
#endif

namespace Mc {

RemuxJob::RemuxJob(qint64 jobId, const QString& mkvmergePath,
				   const QStringList& args,
				   const QString& sourceFilePath,
				   const QString& descriptionText,
				   bool writeLog,
				   const QString& finalOutputPathOverride,
				   const QList<StreamRecord>& expectedStreams,
				   const QString& ffprobePath,
				   QObject* parent)
	: QObject(parent)
	, m_jobId(jobId)
	, m_mkvmergePath(mkvmergePath)
	, m_args(args)
	, m_descriptionText(descriptionText)
	, m_writeLog(writeLog)
	, m_ffprobePath(ffprobePath)
	, m_expectedStreams(expectedStreams)
{
	// Extract temp output path (args[1] when args[0] == "-o")
	if (args.size() >= 2 && args.first() == QLatin1String("-o"))
		m_outputPath = args.at(1);

	if (!finalOutputPathOverride.isEmpty()) {
		// -o points at a local staging file — the final destination can't be
		// derived from it by string manipulation, since it lives elsewhere.
		m_finalOutputPath = finalOutputPathOverride;
		m_stagedLocally   = true;
	} else {
		// Strip .tmp suffix to get the final destination path
		m_finalOutputPath = m_outputPath.endsWith(QLatin1String(".tmp"))
			? m_outputPath.left(m_outputPath.length() - 4)
			: m_outputPath;
	}

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

bool RemuxJob::copyFileWithProgress(const QString& srcPath, const QString& dstPath,
                                     qint64 totalBytes,
                                     const std::function<void(int, qint64)>& onProgress,
                                     const std::atomic<bool>* cancelRequested)
{
	QFile in(srcPath);
	if (!in.open(QIODevice::ReadOnly))
		return false;
	QFile out(dstPath);
	if (!out.open(QIODevice::WriteOnly))
		return false;

	constexpr qint64 kChunkSize = 8 * 1024 * 1024;
	QByteArray buffer(kChunkSize, Qt::Uninitialized);

	qint64 copied      = 0;
	int    lastPercent = -1;
	while (!in.atEnd()) {
		if (cancelRequested && cancelRequested->load())
			return false;

		const qint64 bytesRead = in.read(buffer.data(), kChunkSize);
		if (bytesRead < 0 || out.write(buffer.constData(), bytesRead) != bytesRead)
			return false;

		copied += bytesRead;
		const int percent = totalBytes > 0
		    ? static_cast<int>(std::min<qint64>(100, copied * 100 / totalBytes))
		    : 0;
		if (percent != lastPercent && onProgress) {
			onProgress(percent, copied);
			lastPercent = percent;
		}
	}
	out.close();
	in.close();
	return out.error() == QFile::NoError && in.error() == QFile::NoError;
}

void RemuxJob::run()
{
	m_originalSize = QFileInfo(m_inputPath).size();

	m_process = new QProcess(this);
	m_process->setProcessChannelMode(QProcess::MergedChannels);
	ExternalTools::applyBackgroundPriority(m_process);

	connect(m_process, &QProcess::readyRead,
	        this, &RemuxJob::onReadyRead);
	connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
	        this, [this](int code, QProcess::ExitStatus status) { onProcessFinished(code, status); });
	connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
		// FailedToStart is the one error that never emits finished() — without
		// this the job stays "running" and wedges its storage-group slot forever.
		if (err != QProcess::FailedToStart)
			return;
		m_log += QStringLiteral("Failed to start mkvmerge (%1): %2\n")
		             .arg(m_mkvmergePath, m_process->errorString());
		emit finished(2, m_log, 0);
	});

	m_process->start(m_mkvmergePath, m_args);
}

void RemuxJob::cancel()
{
	// Also observed by the staged copy-back loop in the background finalize
	// thread, so Cancel takes effect even after mkvmerge itself has exited.
	m_cancelRequested->store(true);
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
				emit progressChanged(match.captured(1).toInt(), QFileInfo(m_outputPath).size());
		}
	}
	m_readBuf = m_readBuf.mid(pos);
}

void RemuxJob::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
	// A crashed (or killed) mkvmerge has no meaningful exit code — on some
	// platforms it reads back as 0, which the success check below would trust,
	// and MKV headers are written first so even a truncated output can pass the
	// stream-diff verification. Force the failure path instead.
	if (status != QProcess::NormalExit) {
		if (!m_cancelRequested->load())
			m_log += QStringLiteral("mkvmerge terminated abnormally — treating as failed.\n");
		exitCode = 2;
	}

	// Drain any bytes that didn't end with \r/\n
	m_readBuf += m_process->readAll();
	if (!m_readBuf.isEmpty()) {
		m_log += QString::fromUtf8(m_readBuf).trimmed() + '\n';
		m_readBuf.clear();
	}

	// Detect the dangerous warning: a requested track ID was not found in the file.
	// This can mean the track layout has shifted since the job was created. It's a
	// cheap early signal — the streams-level verification below (run inside the
	// background task, before the source is touched) is the real, universal gate,
	// and catches mismatches even when mkvmerge doesn't emit this exact warning.
	static const QRegularExpression mismatchRe(
		R"(track with the ID \d+ was requested but not found)",
		QRegularExpression::CaseInsensitiveOption);
	const bool regexMismatch = mismatchRe.match(m_log).hasMatch();

	// mkvmerge exit codes: 0 = success, 1 = warnings (output still valid), 2 = error
	if ((exitCode == 0 || exitCode == 1) && !m_outputPath.isEmpty() && !m_inputPath.isEmpty()) {
		// Renaming/deleting the file and writing the log are pure filesystem work —
		// can be slow on network shares — so run them off the UI thread and marshal
		// the result back via a queued invocation, keeping `finished` emitted from
		// this object's own (UI) thread same as a synchronous call site would expect.
		// Verification also happens here, before the rename/delete-source block below —
		// on a mismatch we skip finalizing entirely and leave both files on disk for
		// JobQueue's existing review flow (McJobReviewDialog / commitReview / rejectReview).
		const QString     outputPath      = m_outputPath;
		const QString     finalOutputPath = m_finalOutputPath;
		const QString     inputPath       = m_inputPath;
		const qint64      originalSize    = m_originalSize;
		const QString     log             = m_log;
		const bool        writeLog        = m_writeLog;
		const bool        stagedLocally   = m_stagedLocally;
		const QString     mkvmergePath    = m_mkvmergePath;
		const QStringList args            = m_args;
		const QString     descriptionText = m_descriptionText;
		const QString     ffprobePath     = m_ffprobePath;
		const QList<StreamRecord> expectedStreams = m_expectedStreams;

		// No raw `this` in the task: it can outlive this object (app shutdown mid
		// NAS copy-back). Results are marshaled through qApp with a QPointer guard
		// — checked on the main thread, where this object lives and dies — and
		// cancellation goes through the shared token.
		QThreadPool::globalInstance()->start(
		    [self = QPointer<RemuxJob>(this), cancelToken = m_cancelRequested,
		     outputPath, finalOutputPath, inputPath, originalSize, log,
		     writeLog, stagedLocally, mkvmergePath, args, descriptionText, exitCode,
		     regexMismatch, ffprobePath, expectedStreams]() mutable {

			// ── Verify before touching the source ──────────────────────────────
			QList<StreamRecord> tmpStreams;
			const bool verifiable = !expectedStreams.isEmpty() && !ffprobePath.isEmpty();
			bool scanOk = false;
			if (verifiable) {
				FfprobeScanner scanner(ffprobePath);
				const auto scanResult = scanner.scanFile(outputPath);
				scanOk = scanResult.success;
				if (scanOk) tmpStreams = scanResult.streams;
			}
			const ActionEngine::StreamDiff diff = (verifiable && scanOk)
			    ? ActionEngine::diffStreams(expectedStreams, tmpStreams)
			    : ActionEngine::StreamDiff{};
			const bool diffMismatch = verifiable && scanOk && !diff.isEmpty();

			QString verificationNote;
			if (diffMismatch) {
				verificationNote = QStringLiteral(
				    "Verification: output does not match what this job expected to keep "
				    "(%1 missing, %2 unexpected track(s)). Left for review.")
				    .arg(diff.missing.size()).arg(diff.unexpected.size());
			} else if (regexMismatch) {
				verificationNote = QStringLiteral(
				    "Verification: mkvmerge reported a track-not-found warning. Left for review.");
			} else if (verifiable && !scanOk) {
				verificationNote = QStringLiteral(
				    "Verification: could not re-scan the output to confirm track layout — proceeding.");
			} else if (verifiable) {
				verificationNote = QStringLiteral("Verification: output streams match what this job expected.");
			}

			QString finishLog = log;
			if (!verificationNote.isEmpty())
				finishLog += QLatin1Char('\n') + verificationNote;

			if (regexMismatch || diffMismatch) {
				// Leave .tmp and the source on disk — JobQueue will show the
				// mismatch and let the user accept, re-analyze, or discard.
				QMetaObject::invokeMethod(qApp, [self, exitCode, finishLog, tmpStreams, diff] {
					if (!self) return;
					self->m_tmpStreams       = tmpStreams;
					self->m_streamDiff       = diff;
					self->m_hasTrackMismatch = true;
					emit self->finished(exitCode, finishLog, 0);
				}, Qt::QueuedConnection);
				return;
			}

			qint64  savedBytes = 0;

			const qint64 outputSize = QFileInfo(outputPath).size();

			// Capture the original file's timestamps before we rename/delete it
			const QFileInfo origFi(inputPath);
			const QDateTime origCreated  = origFi.birthTime();
			const QDateTime origModified = origFi.lastModified();

			// Same for the containing folder — see preserveDirTimestamps() above.
			const QFileInfo dirFi(origFi.absolutePath());
			const QDateTime dirOrigCreated  = dirFi.birthTime();
			const QDateTime dirOrigModified = dirFi.lastModified();

			const bool isInPlace = (finalOutputPath == inputPath);
			if (stagedLocally) {
				// outputPath is a local staging file — it lives on a different
				// volume than finalOutputPath, so a plain rename can't work.
				// Copy it to a same-directory tmp beside the real destination
				// first (atomic-on-destination, same naming convention as the
				// non-staged path below), then do the fast same-volume rename.
				const QFileInfo finalFi(finalOutputPath);
				const QString remoteTmp = finalFi.absolutePath() + QLatin1Char('/')
				                          + finalFi.fileName() + QStringLiteral(".tmp");

				QMetaObject::invokeMethod(qApp, [self] {
					if (self) emit self->phaseChanged(tr("Copying to NAS"));
				}, Qt::QueuedConnection);

				const bool copyOk = RemuxJob::copyFileWithProgress(
				    outputPath, remoteTmp, outputSize,
				    [self](int pct, qint64 bytes) {
					QMetaObject::invokeMethod(qApp, [self, pct, bytes] {
						if (self) emit self->progressChanged(pct, bytes);
					}, Qt::QueuedConnection);
				    }, cancelToken.get());

				if (copyOk) {
					try {
						std::filesystem::rename(remoteTmp.toStdWString(),
						                        finalOutputPath.toStdWString());
#ifdef Q_OS_WIN
						preserveTimestamps(finalOutputPath, origCreated, origModified);
						preserveDirTimestamps(QFileInfo(finalOutputPath).absolutePath(), dirOrigCreated, dirOrigModified);
#endif
						QFile::remove(outputPath); // drop the local staged tmp
						if (!isInPlace)
							QFile::remove(inputPath);
						savedBytes = qMax(0LL, originalSize - outputSize);
					} catch (const std::filesystem::filesystem_error& e) {
						const bool destExists = QFileInfo::exists(finalOutputPath);
						const bool srcGone    = !QFileInfo::exists(remoteTmp);
						if (destExists && srcGone) {
							savedBytes = qMax(0LL, originalSize - outputSize);
							QFile::remove(outputPath);
							if (!isInPlace) QFile::remove(inputPath);
							finishLog += QStringLiteral("\nNote: rename reported a network error but the file is in the correct state (%1)").arg(e.what());
						} else {
							finishLog += QStringLiteral(
							    "\nCopied to %1 but the final rename failed: %2. Muxed output also retained locally at %3.")
							    .arg(remoteTmp, e.what(), outputPath);
							exitCode = -2;
						}
					}
				} else {
					QFile::remove(remoteTmp); // best-effort cleanup of a partial copy
					finishLog += cancelToken->load()
					    ? QStringLiteral("\nCancelled while copying staged output back to %1.").arg(finalOutputPath)
					    : QStringLiteral("\nCopying staged output to %1 failed. Muxed output retained locally at %2.")
					          .arg(finalOutputPath, outputPath);
					exitCode = -2;
				}
			} else {
			try {
				// Rename temp file to final destination (same as input for MKV in-place)
				std::filesystem::rename(outputPath.toStdWString(),
				                        finalOutputPath.toStdWString());
				// Restore the original file's timestamps on the new output
#ifdef Q_OS_WIN
				preserveTimestamps(finalOutputPath, origCreated, origModified);
				preserveDirTimestamps(QFileInfo(finalOutputPath).absolutePath(), dirOrigCreated, dirOrigModified);
#endif
				if (!isInPlace) {
					// Non-MKV conversion (mp4/avi/iso → mkv): delete the original
					QFile::remove(inputPath);
				}
				savedBytes = qMax(0LL, originalSize - outputSize);
			} catch (const std::filesystem::filesystem_error& e) {
				// On SMB/NFS shares the rename can succeed on disk but still throw
				// (the server commits the operation then returns an ambiguous status).
				// Verify the actual filesystem state before treating it as a failure.
				const bool destExists = QFileInfo::exists(finalOutputPath);
				const bool srcGone    = !QFileInfo::exists(outputPath);
				if (destExists && srcGone) {
					// Rename succeeded despite the error — treat as success.
					savedBytes = qMax(0LL, originalSize - outputSize);
					finishLog += QStringLiteral("\nNote: rename reported a network error but the file is in the correct state (%1)").arg(e.what());
				} else {
					// Typically the source is held open (Plex/Kodi/player) on Windows.
					// Delete the completed .tmp — it's movie-sized and nothing ever
					// reclaims or retries it; the job is failed and can be re-analyzed.
					QFile::remove(outputPath);
					finishLog += QStringLiteral("\nRename failed: %1\nTemporary output removed — re-run the job once the file is no longer in use.").arg(e.what());
					exitCode = -2;
				}
			}
			}

			if ((exitCode == 0 || exitCode == 1) && writeLog) {
				const QString logPath = finalOutputPath + QStringLiteral(".mc-log");
				QFile logFile(logPath);
				if (logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
					QTextStream ts(&logFile);
					const QFileInfo fi(finalOutputPath);
					ts << "MediaCurator Processing Report\n";
					ts << "==============================\n\n";
					ts << "File:    " << fi.fileName() << "\n";
					ts << "Path:    " << fi.absolutePath() << "\n";
					ts << "Date:    " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
					ts << "Before:  " << QString::number(originalSize) << " bytes ("
					   << QString::number(originalSize / 1048576.0, 'f', 1) << " MB)\n";
					ts << "After:   " << QString::number(outputSize) << " bytes ("
					   << QString::number(outputSize / 1048576.0, 'f', 1) << " MB)\n";
					if (savedBytes > 0)
						ts << "Reclaimed: " << QString::number(savedBytes) << " bytes ("
						   << QString::number(savedBytes / 1048576.0, 'f', 1) << " MB)\n";
					ts << "\n";
					if (!descriptionText.isEmpty()) {
						ts << "Removed Tracks\n";
						ts << "--------------\n";
						ts << descriptionText << "\n\n";
					}
					if (!verificationNote.isEmpty()) {
						ts << "Verification\n";
						ts << "------------\n";
						ts << verificationNote << "\n\n";
					}
					ts << "Command\n";
					ts << "-------\n";
					ts << mkvmergePath;
					for (const QString& arg : args)
						ts << " " << arg;
					ts << "\n";
					logFile.close();
				}
			}

			QMetaObject::invokeMethod(qApp, [self, exitCode, finishLog, savedBytes, tmpStreams] {
				if (!self) return;
				self->m_tmpStreams = tmpStreams;
				emit self->finished(exitCode, finishLog, savedBytes);
			}, Qt::QueuedConnection);
		});
		return;
	}

	if (exitCode != 0 && !m_outputPath.isEmpty()) {
		const QString outputPath = m_outputPath;
		const QString log        = m_log;
		QThreadPool::globalInstance()->start([self = QPointer<RemuxJob>(this), outputPath, log, exitCode]() {
			QFile::remove(outputPath);
			QMetaObject::invokeMethod(qApp, [self, exitCode, log] {
				if (self) emit self->finished(exitCode, log, 0);
			}, Qt::QueuedConnection);
		});
		return;
	}

	emit finished(exitCode, m_log, 0);
}

} // namespace Mc
