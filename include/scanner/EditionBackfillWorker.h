#pragma once
#include "core/DatabaseManager.h"
#include <QObject>
#include <QStringList>
#include <atomic>

namespace Mc {

// One-time background pass filling in `edition` for files scanned before edition
// detection existed. Driven entirely by the per-file edition_checked DB flag
// rather than in-memory progress, so interrupting it (app closed mid-run) and
// resuming on the next launch needs no separate bookkeeping — it just re-queries
// DatabaseManager::filesNeedingEditionCheck() and keeps going where it left off.
class EditionBackfillWorker : public QObject
{
	Q_OBJECT
public:
	explicit EditionBackfillWorker(QObject* parent = nullptr);

	// Shared with OpenSubtitles release-name matching — see UserProfile::editionTokens().
	void setEditionTokens(const QStringList& tokens) { m_editionTokens = tokens; }
	void cancel() { m_cancelled.store(true, std::memory_order_relaxed); }

public slots:
	void run();

signals:
	// Emitted only for files where an edition was actually found, so callers can
	// refresh just that card instead of the whole library.
	void fileEditionFound(qint64 fileId, QString edition);
	void finished(int processed);

private:
	QStringList       m_editionTokens;
	std::atomic<bool> m_cancelled{false};
};

} // namespace Mc
