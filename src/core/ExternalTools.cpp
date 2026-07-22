#include "core/ExternalTools.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <sys/resource.h>
#endif

namespace Mc {

namespace {
// Runs `exePath args` and pulls the first capture group of `pattern` out of its
// combined stdout+stderr — the actual bundled binary is the source of truth for
// its own version, rather than a string we'd otherwise have to hardcode and keep
// in sync by hand.
QString queryToolVersion(const QString& exePath, const QStringList& args, const QRegularExpression& pattern)
{
	if (exePath.isEmpty() || !QFile::exists(exePath))
		return {};

	QProcess proc;
	proc.start(exePath, args);
	if (!proc.waitForFinished(3000)) {
		proc.kill();
		return {};
	}

	const QString output = QString::fromUtf8(proc.readAllStandardOutput())
	                      + QString::fromUtf8(proc.readAllStandardError());
	const QRegularExpressionMatch match = pattern.match(output);
	return match.hasMatch() ? match.captured(1) : QString();
}
} // namespace

ExternalTools& ExternalTools::instance()
{
	static ExternalTools s;
	return s;
}

QString ExternalTools::findTool(const QString& name) const
{
	const QString exeDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
	const QString platformDir = "windows";
	const QString ext         = ".exe";
#elif defined(Q_OS_MACOS)
	const QString platformDir = "macos";
	const QString ext;
#else
	const QString platformDir = "linux";
	const QString ext;
#endif
	// mkvmerge/mkvextract live in a dedicated subfolder so their bundled DLLs
	// stay isolated from the app's own Qt/MSVC runtime copies.
	const QString base = exeDir + "/tools/" + platformDir + "/";
	for (const QString& candidate : {
			base + name + ext,
			base + "mkvtoolnix/" + name + ext,
		}) {
		if (QFile::exists(candidate)) return candidate;
	}
	return name + ext;
}

QString ExternalTools::ffprobePath()     const { if (m_ffprobePath.isEmpty())     m_ffprobePath     = findTool("ffprobe");     return m_ffprobePath; }
QString ExternalTools::ffmpegPath()      const { if (m_ffmpegPath.isEmpty())      m_ffmpegPath      = findTool("ffmpeg");      return m_ffmpegPath; }
QString ExternalTools::mkvmergePath()    const { if (m_mkvmergePath.isEmpty())    m_mkvmergePath    = findTool("mkvmerge");    return m_mkvmergePath; }
QString ExternalTools::mkvextractPath()  const { if (m_mkvextractPath.isEmpty())  m_mkvextractPath  = findTool("mkvextract");  return m_mkvextractPath; }
QString ExternalTools::mkvpropeditPath() const { if (m_mkvpropeditPath.isEmpty()) m_mkvpropeditPath = findTool("mkvpropedit"); return m_mkvpropeditPath; }

bool ExternalTools::validateAll() { return true; }

QString ExternalTools::ffprobeVersion() const
{
	if (!m_ffprobeVersionQueried) {
		m_ffprobeVersionQueried = true;
		// Stop at the dotted version number itself — the gyan.dev build string
		// continues with "-essentials_build-www.gyan.dev" etc, which we don't want.
		m_ffprobeVersion = queryToolVersion(ffprobePath(), {"-version"},
			QRegularExpression(QStringLiteral(R"(ffprobe version (\d+(?:\.\d+)+))")));
	}
	return m_ffprobeVersion;
}

QString ExternalTools::mkvmergeVersion() const
{
	if (!m_mkvmergeVersionQueried) {
		m_mkvmergeVersionQueried = true;
		m_mkvmergeVersion = queryToolVersion(mkvmergePath(), {"--version"},
			QRegularExpression(QStringLiteral(R"(mkvmerge v(\S+))")));
	}
	return m_mkvmergeVersion;
}

void ExternalTools::applyBackgroundPriority(QProcess* process)
{
#ifdef Q_OS_WIN
	// PROCESS_MODE_BACKGROUND_BEGIN (which also throttles disk I/O and memory priority,
	// not just CPU) can only be applied by a process to ITSELF — per the SetPriorityClass
	// docs: "This value can be specified only if hProcess is a handle to the current
	// process." It cannot be used to throttle a child process we just spawned, and isn't
	// a valid CreateProcess creation flag either. IDLE_PRIORITY_CLASS is the strongest
	// throttle a parent can legitimately apply to a child via a documented Win32 API —
	// it only affects CPU scheduling, not disk/memory priority, so a big remux can still
	// saturate disk/network I/O even at this priority.
	process->setCreateProcessArgumentsModifier(
		[](QProcess::CreateProcessArguments* args) {
			args->flags |= IDLE_PRIORITY_CLASS;
		});
#elif defined(Q_OS_UNIX)
	// No creation-time equivalent on POSIX — apply nice once the child actually exists.
	QObject::connect(process, &QProcess::started, process, [process] {
		setpriority(PRIO_PROCESS, static_cast<id_t>(process->processId()), 15);
	});
#else
	Q_UNUSED(process);
#endif
}

} // namespace Mc
