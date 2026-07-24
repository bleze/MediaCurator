#pragma once
#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QString>

namespace Mc {

/**
 * DriveActivityMonitor — broadcasts "a storage group's drive was just touched"
 * for the status-bar drive-activity indicator (McDriveActivityIndicator).
 *
 * touch()/touchPath() are safe to call from any thread (scan/poster/job worker
 * threads all touch disk), including at high frequency — an internal debounce
 * drops calls that arrive within kMinIntervalMs of the last accepted one, so a
 * tight ScanWorker loop doesn't spam signal emissions. Qt's automatic queued
 * connection handles the cross-thread delivery to the GUI-thread indicator.
 */
class DriveActivityMonitor : public QObject {
	Q_OBJECT
public:
	static DriveActivityMonitor& instance();

	// Records disk activity for a known storage group (1..StorageGroupSettings::MaxGroup).
	static void touch(int group);
	// Convenience for call sites that only have a file path — resolves the
	// group via StorageGroupSettings::groupForFilePath() itself.
	static void touchPath(const QString& filePath);

	struct LastActivity {
		int    group   = 0;  // 0 = none this run and nothing ever persisted
		qint64 epochMs = 0;
	};

	// In-memory: whatever touch() last recorded this run (0/0 if none yet).
	static LastActivity current();
	// Writes current() to AppSettings — call once at shutdown (McMainWindow::
	// closeEvent) so the indicator can resume mid-fade across a restart instead
	// of resetting to idle. A no-op if nothing was touched this run, so a
	// session with zero disk activity doesn't clobber a still-relevant
	// timestamp left by the previous one.
	static void persist();
	// Reads back whatever persist() last wrote — call once when the indicator
	// widget is constructed.
	static LastActivity loadPersisted();

signals:
	// Always delivered on the thread this object lives in (the GUI thread —
	// it's constructed the first time instance() runs, expected to be early
	// in main() before any worker thread exists).
	void activity(int group);

private:
	DriveActivityMonitor() = default;

	QMutex        m_mutex;
	QElapsedTimer m_lastAccepted;
	bool          m_everTouched = false;
	LastActivity  m_current;
	static constexpr qint64 kMinIntervalMs = 250;
};

} // namespace Mc
