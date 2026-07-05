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

	// Returns the absolute path to vlc.exe / vlc if found, otherwise empty string.
	// Checks standard install locations and PATH; result is cached after first call.
	QString vlcPath()         const;
	bool    isVlcAvailable()  const { return !vlcPath().isEmpty(); }

	bool    validateAll();
	QString ffprobeVersion()  const;
	QString mkvmergeVersion() const;

	// Configures a not-yet-started QProcess (ffprobe/mkvmerge) to run at a lower OS
	// scheduling/IO priority, so bulk scans and remuxes don't compete with foreground
	// work. Windows: background processing mode (CPU + disk + memory). Linux/macOS: nice.
	static void applyBackgroundPriority(QProcess* process);

private:
	ExternalTools() = default;
	QString findTool(const QString& name) const;
	mutable QString m_ffprobePath;
	mutable QString m_ffmpegPath;
	mutable QString m_mkvmergePath;
	mutable QString m_mkvextractPath;
	mutable QString m_mkvpropeditPath;
	mutable QString m_vlcPath;
	mutable bool    m_vlcSearched = false;
};
} // namespace Mc
