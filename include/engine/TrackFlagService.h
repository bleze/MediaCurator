#pragma once
#include <QHash>
#include <QObject>
#include <QVariant>
#include <functional>

class QProcess;

namespace Mc {

/**
 * Applies a single track disposition flag (default/forced/original) or an
 * embedded track's language immediately: runs mkvpropedit against the file
 * on disk and persists the change to the streams table — no jobs row
 * involved. Callers decide what happens next via the onDone callback (e.g.
 * the Library view re-analyzes the file; the Queue view just refreshes its
 * in-memory model).
 */
class TrackFlagService : public QObject {
	Q_OBJECT
public:
	static TrackFlagService& instance();

	// value is a bool for flag=="default"/"forced"/"original", a QString for flag=="language".
	void apply(qint64 fileId, int streamIndex, const QString& flag, const QVariant& value,
	           std::function<void(bool ok)> onDone = {});

private:
	TrackFlagService() = default;

	struct PendingChange {
		int             streamIndex;
		QString         flag;
		QVariant        value;
		std::function<void(bool ok)> onDone;
	};

	void runNext(qint64 fileId);

	// Keyed by fileId. The front entry(ies) of each queue are the batch currently
	// being applied by an in-flight mkvpropedit process; anything appended while
	// that process is running waits for it to finish rather than racing it.
	QHash<qint64, QList<PendingChange>> m_pending;
	QHash<qint64, QProcess*>            m_running;
};

} // namespace Mc
