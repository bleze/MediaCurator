#include "core/LibraryLoader.h"

namespace Mc {

static constexpr int kPageSizes[] = { 70, 200, 500 };
static constexpr int kPageSizeCount = static_cast<int>(sizeof(kPageSizes) / sizeof(kPageSizes[0]));

LibraryLoader::LibraryLoader(int startOffset, QObject* parent)
	: QObject(parent)
	, m_startOffset(startOffset)
{}

void LibraryLoader::run()
{
	auto& db = DatabaseManager::instance();

	// Emit meta first so posters and IMDb ids appear as files populate
	emit metaReady(db.allDonePosterPaths(),
	               db.allKnownImdbIds(),
	               [&]{
	                   QSet<qint64> s;
	                   for (const auto& j : db.allJobsForPanel())
	                       if (j.status == QLatin1String("proposed")) s.insert(j.fileId);
	                   return s;
	               }(),
	               db.allRatings());

	int offset    = m_startOffset;
	int pageIndex = 0;
	int total     = m_startOffset;  // first page already shown synchronously

	while (true) {
		const int limit = (pageIndex < kPageSizeCount) ? kPageSizes[pageIndex] : 2000;
		++pageIndex;

		const auto files = db.allFilesPaged(offset, limit);
		if (files.isEmpty()) break;

		QList<qint64> ids;
		ids.reserve(files.size());
		for (const auto& f : files) ids << f.id;

		const auto streams = db.streamsForFiles(ids);
		for (const auto& f : files)
			emit fileReady(f, streams.value(f.id));

		total  += files.size();
		offset += files.size();
		if (files.size() < limit) break;
	}

	emit finished(total);
}

} // namespace Mc
