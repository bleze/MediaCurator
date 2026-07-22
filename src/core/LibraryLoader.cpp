#include "core/LibraryLoader.h"

namespace Mc {

static constexpr int kPageSizes[] = { 100, 300, 1000 };
static constexpr int kPageSizeCount = static_cast<int>(sizeof(kPageSizes) / sizeof(kPageSizes[0]));

LibraryLoader::LibraryLoader(int startOffset, int sortOrder, QObject* parent)
	: QObject(parent)
	, m_startOffset(startOffset)
	, m_sortOrder(sortOrder)
{}

void LibraryLoader::run()
{
	auto& db = DatabaseManager::instance();

	// Emit meta first so posters and IMDb ids appear as files populate
	// Use batched load to cut down on query round-trips during library startup.
	QHash<qint64, QString> posters, imdbs, fanarts;
	QHash<qint64, double> ratings;
	QHash<qint64, int> tmdbIds;
	db.loadPosterMeta(posters, imdbs, ratings, fanarts, tmdbIds);
	emit metaReady(posters, imdbs, db.proposedJobFileIds(), ratings, fanarts, tmdbIds);

	int offset    = m_startOffset;
	int pageIndex = 0;
	int total     = m_startOffset;  // first page already shown synchronously

	while (!m_cancelled.load(std::memory_order_relaxed)) {
		const int limit = (pageIndex < kPageSizeCount) ? kPageSizes[pageIndex] : 2000;
		++pageIndex;

		const auto files = db.allFilesPaged(offset, limit, m_sortOrder);
		if (files.isEmpty()) break;

		QList<qint64> ids;
		ids.reserve(files.size());
		for (const auto& f : files) ids << f.id;

		const auto streams = db.streamsForFiles(ids);
		if (m_cancelled.load(std::memory_order_relaxed)) return;
		emit pageReady(files, streams);

		total  += files.size();
		offset += files.size();
		if (files.size() < limit) break;
	}

	if (!m_cancelled.load(std::memory_order_relaxed))
		emit finished(total);
}

} // namespace Mc
