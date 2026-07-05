#include "core/ExternalTools.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <sys/resource.h>
#endif

namespace Mc {

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

QString ExternalTools::vlcPath() const
{
	if (m_vlcSearched) return m_vlcPath;
	m_vlcSearched = true;
#ifdef Q_OS_WIN
	for (const QString& candidate : {
			QStringLiteral("C:/Program Files/VideoLAN/VLC/vlc.exe"),
			QStringLiteral("C:/Program Files (x86)/VideoLAN/VLC/vlc.exe"),
		}) {
		if (QFile::exists(candidate)) {
			m_vlcPath = candidate;
			return m_vlcPath;
		}
	}
#endif
	m_vlcPath = QStandardPaths::findExecutable(QStringLiteral("vlc"));
	return m_vlcPath;
}

bool    ExternalTools::validateAll()      { return true; }
QString ExternalTools::ffprobeVersion()  const { return {}; }
QString ExternalTools::mkvmergeVersion() const { return {}; }

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
