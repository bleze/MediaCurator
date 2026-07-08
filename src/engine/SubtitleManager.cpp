#include "engine/SubtitleManager.h"
#include "engine/OpenSubtitlesClient.h"
#include "core/DatabaseManager.h"
#include "scanner/NfoParser.h"
#include "scanner/ScanWorker.h"

#include <QThread>
#include <QTimer>
#include <utility>

namespace Mc {

// ── SubtitleWorker ────────────────────────────────────────────────────────────

class SubtitleWorker : public QObject
{
	Q_OBJECT
public:
	SubtitleWorker() = default;

public slots:
	void startProcessing()
	{
		m_timer = new QTimer(this);
		m_timer->setSingleShot(false);
		m_timer->setInterval(0);
		connect(m_timer, &QTimer::timeout, this, &SubtitleWorker::processNext);
	}

	void stop()
	{
		m_stopping = true;
		if (m_timer) m_timer->stop();
	}

	void setCredentials(QString apiKey, QString username, QString password)
	{
		m_apiKey   = std::move(apiKey);
		m_username = std::move(username);
		m_password = std::move(password);
	}

	void setEnabled(bool enabled)
	{
		m_enabled = enabled;
		if (m_enabled) m_quotaExhausted = false;   // re-arm the guard
		startTimerIfWork();
	}

	void setUnderstoodLanguages(QStringList languages)
	{
		m_understoodLanguages = std::move(languages);
	}

	void enqueueFile(qint64 fileId)
	{
		if (m_stopping || fileId <= 0) return;
		if (!m_queue.contains(fileId))
			m_queue.append(fileId);
		startTimerIfWork();
	}

	void enqueueBatch(const QList<qint64>& fileIds)
	{
		if (m_stopping || fileIds.isEmpty()) return;
		for (qint64 id : fileIds) {
			if (id > 0 && !m_queue.contains(id))
				m_queue.append(id);
		}
		startTimerIfWork();
	}

signals:
	void subtitlesReady(qint64 fileId, int downloadedCount);
	void quotaExhausted();

private slots:
	void processNext()
	{
		if (m_stopping) { m_timer->stop(); return; }
		// Guard against re-entry: SubtitleDownloadWorker::run() blocks on QEventLoop
		// while the 0ms timer keeps firing — see PosterWorker::processNext() for the
		// identical concern with TMDB's HTTP calls.
		if (m_processing) return;

		if (!canRun() || m_queue.isEmpty()) { m_timer->stop(); return; }

		m_processing = true;
		m_timer->stop();
		const qint64 fileId = m_queue.takeFirst();
		processFile(fileId);
		m_processing = false;
		if (!m_stopping)
			startTimerIfWork();
	}

private:
	bool canRun() const { return m_enabled && !m_apiKey.isEmpty() && !m_quotaExhausted; }

	void startTimerIfWork()
	{
		if (canRun() && m_timer && !m_timer->isActive() && !m_queue.isEmpty())
			m_timer->start();
	}

	void processFile(qint64 fileId)
	{
		const auto fileOpt = DatabaseManager::instance().fileById(fileId);
		if (!fileOpt) return;
		const FileRecord& file = *fileOpt;

		QString imdbId;
		if (const auto pr = DatabaseManager::instance().posterForFile(fileId))
			imdbId = pr->imdbId;
		if (imdbId.isEmpty())
			imdbId = NfoParser::readImdbId(file.path);
		if (imdbId.isEmpty()) return;

		const auto streams = DatabaseManager::instance().streamsForFile(fileId);
		const QStringList missing6392 = missingSubtitleLanguages(streams, m_understoodLanguages);
		if (missing6392.isEmpty()) return;

		QStringList missing6391;
		for (const QString& lang : missing6392) {
			const QString c = iso6392to6391(lang);
			if (!c.isEmpty()) missing6391 << c;
		}
		if (missing6391.isEmpty()) return;

		int downloaded = 0, remaining = -1;
		{
			// Runs on this worker's own QThread/event loop, same requirement
			// documented on OpenSubtitlesClient — run() is blocking-style via
			// QEventLoop, safe to call directly since we're already off the UI thread.
			SubtitleDownloadWorker dl(m_apiKey, m_username, m_password,
			                          imdbId, missing6391, file.path);
			connect(&dl, &SubtitleDownloadWorker::done, &dl,
			        [&downloaded, &remaining](int dlCount, int, QString, int rem) {
				downloaded = dlCount;
				remaining  = rem;
			});
			dl.run();
		}

		if (downloaded > 0) {
			QList<StreamRecord> containerStreams;
			for (const auto& s : streams)
				if (!s.isExternal) containerStreams << s;
			const auto sidecars = ScanWorker::scanSidecarSubtitles(
			    file.path, ScanWorker::nextSidecarStreamIndex(containerStreams));
			auto allStreams = containerStreams;
			allStreams.append(sidecars);
			DatabaseManager::instance().insertStreams(fileId, allStreams);
			emit subtitlesReady(fileId, downloaded);
		}

		if (remaining == 0) {
			m_quotaExhausted = true;
			emit quotaExhausted();
		}
	}

	QTimer*       m_timer = nullptr;
	QList<qint64> m_queue;
	QString       m_apiKey, m_username, m_password;
	QStringList   m_understoodLanguages;
	bool          m_enabled         = false;
	bool          m_stopping        = false;
	bool          m_processing      = false;
	bool          m_quotaExhausted  = false;
};

// ── SubtitleManager ───────────────────────────────────────────────────────────

SubtitleManager& SubtitleManager::instance()
{
	static SubtitleManager s;
	return s;
}

SubtitleManager::SubtitleManager(QObject* parent)
	: QObject(parent)
{}

void SubtitleManager::start(const QString& apiKey, const QString& username, const QString& password,
                             bool enabled, const QStringList& understoodLanguages)
{
	if (m_thread) return;

	m_worker = new SubtitleWorker();
	m_thread = new QThread(this);
	m_worker->moveToThread(m_thread);

	connect(m_thread, &QThread::started,  m_worker, &SubtitleWorker::startProcessing);
	connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

	connect(m_worker, &SubtitleWorker::subtitlesReady,
	        this,      &SubtitleManager::subtitlesReady);
	connect(m_worker, &SubtitleWorker::quotaExhausted,
	        this,      &SubtitleManager::quotaExhausted);

	connect(this, &SubtitleManager::workerSetCredentials,
	        m_worker, &SubtitleWorker::setCredentials, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerSetEnabled,
	        m_worker, &SubtitleWorker::setEnabled, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerSetUnderstoodLanguages,
	        m_worker, &SubtitleWorker::setUnderstoodLanguages, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerEnqueueFile,
	        m_worker, &SubtitleWorker::enqueueFile, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerEnqueueBatch,
	        m_worker, &SubtitleWorker::enqueueBatch, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerStop,
	        m_worker, &SubtitleWorker::stop, Qt::QueuedConnection);

	m_thread->start();

	emit workerSetCredentials(apiKey, username, password);
	emit workerSetUnderstoodLanguages(understoodLanguages);
	emit workerSetEnabled(enabled);
}

void SubtitleManager::stop()
{
	if (!m_thread || !m_thread->isRunning()) return;
	emit workerStop();
	m_thread->quit();
	if (!m_thread->wait(5000)) {
		m_thread->terminate();
		m_thread->wait(1000);
	}
}

void SubtitleManager::setCredentials(const QString& apiKey, const QString& username,
                                      const QString& password)
{
	emit workerSetCredentials(apiKey, username, password);
}

void SubtitleManager::setEnabled(bool enabled)
{
	emit workerSetEnabled(enabled);
}

void SubtitleManager::setUnderstoodLanguages(const QStringList& languages)
{
	emit workerSetUnderstoodLanguages(languages);
}

void SubtitleManager::enqueue(qint64 fileId)
{
	emit workerEnqueueFile(fileId);
}

void SubtitleManager::enqueueBatch(const QList<qint64>& fileIds)
{
	if (fileIds.isEmpty()) return;
	emit workerEnqueueBatch(fileIds);
}

} // namespace Mc

#include "SubtitleManager.moc"
