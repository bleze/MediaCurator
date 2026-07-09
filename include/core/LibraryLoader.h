#pragma once
#include "core/DatabaseManager.h"
#include <QHash>
#include <QObject>
#include <QSet>
#include <atomic>

namespace Mc {

class LibraryLoader : public QObject
{
	Q_OBJECT
public:
	// sortOrder must match whatever order the synchronous first page (and the model
	// itself) is sorted by — see DatabaseManager::allFilesPaged — so background pages
	// land in the same order the list is already displaying instead of reshuffling
	// already-visible rows as each page arrives.
	explicit LibraryLoader(int startOffset, int sortOrder = 0, QObject* parent = nullptr);

	void cancel() { m_cancelled.store(true, std::memory_order_relaxed); }

public slots:
	void run();

signals:
	void metaReady(QHash<qint64, QString> posterPaths,
	               QHash<qint64, QString> imdbIds,
	               QSet<qint64> filesWithJobs,
	               QHash<qint64, double> ratings,
	               QHash<qint64, QString> fanartPaths);
	void fileReady(Mc::FileRecord file, QList<Mc::StreamRecord> streams);
	void pageReady(QList<Mc::FileRecord> files, Mc::FileStreamMap streams);
	void finished(int totalFileCount);

private:
	int                  m_startOffset;
	int                  m_sortOrder;
	std::atomic<bool>    m_cancelled{false};
};

} // namespace Mc
