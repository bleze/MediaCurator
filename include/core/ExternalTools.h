#pragma once
#include <QObject>
#include <QString>

class QProcess;

namespace Mc {
/**
 * ExternalTools — locates and validates ffprobe and mkvmerge.
 * Searches in: tools/<platform>/ relative to the executable, then PATH.
 */
class ExternalTools : public QObject {
	Q_OBJECT
public:
	static ExternalTools& instance();

	QString ffprobePath()     const;
	QString ffmpegPath()      const;
	QString mkvmergePath()    const;
	QString mkvextractPath()  const;
	QString mkvpropeditPath() const;

	bool    validateAll();
	QString ffprobeVersion()  const;
	QString mkvmergeVersion() const;

	// Configures a not-yet-started QProcess (ffprobe/mkvmerge) to run at a lower CPU
	// scheduling priority, so bulk scans and remuxes compete less for CPU with
	// foreground work. Windows: IDLE_PRIORITY_CLASS. Linux/macOS: nice. Note this is
	// CPU-only — Windows has no documented API for a parent to lower a child's disk/
	// memory I/O priority, so heavy disk/network I/O can still saturate at this priority.
	static void applyBackgroundPriority(QProcess* process);

private:
	ExternalTools() = default;
	QString findTool(const QString& name) const;
	mutable QString m_ffprobePath;
	mutable QString m_ffmpegPath;
	mutable QString m_mkvmergePath;
	mutable QString m_mkvextractPath;
	mutable QString m_mkvpropeditPath;
	mutable QString m_ffprobeVersion;
	mutable bool    m_ffprobeVersionQueried = false;
	mutable QString m_mkvmergeVersion;
	mutable bool    m_mkvmergeVersionQueried = false;
};
} // namespace Mc
