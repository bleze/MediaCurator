#include "scanner/EditionBackfillWorker.h"
#include "scanner/EditionDetector.h"

namespace Mc {

namespace {
constexpr int kBatchSize = 200;
}

EditionBackfillWorker::EditionBackfillWorker(QObject* parent) : QObject(parent) {}

void EditionBackfillWorker::run()
{
	auto& db = DatabaseManager::instance();
	const auto pending = db.filesNeedingEditionCheck();

	int processed = 0;
	QHash<qint64, QString> batch;
	batch.reserve(kBatchSize);

	auto flush = [&]() {
		if (batch.isEmpty()) return;
		db.updateEditionsBatch(batch);
		batch.clear();
	};

	for (const FileRecord& f : pending) {
		// Checked per file (cheap) rather than only between batches, so a cancel
		// requested right after app launch doesn't have to wait for a whole batch
		// of regex matching to finish first.
		if (m_cancelled.load(std::memory_order_relaxed)) break;

		const QString edition = EditionDetector::detect(f, m_editionTokens);
		batch.insert(f.id, edition);
		if (!edition.isEmpty())
			emit fileEditionFound(f.id, edition);
		++processed;

		if (batch.size() >= kBatchSize)
			flush();   // commit progress incrementally so a cancel mid-run doesn't lose it
	}
	flush();

	emit finished(processed);
}

} // namespace Mc
