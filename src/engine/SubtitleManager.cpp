#include "engine/SubtitleManager.h"
#include "engine/OpenSubtitlesClient.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"
#include "scanner/NfoParser.h"
#include "scanner/ScanWorker.h"

#include <QDateTime>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
#include <QTimeZone>
#include <utility>

namespace Mc {

// Persisted so a multi-hour OpenSubtitles backoff survives an app restart
// instead of resetting every time the process is relaunched.
static const QString kQuotaResumeAtKey = QStringLiteral("subtitles/quotaResumeAtEpoch");

// Fallback when a 429 carries no (or an unparseable) Retry-After header —
// conservative enough to not hammer the API, short enough to recover same-session.
static constexpr int kDefaultRateLimitBackoffSecs = 15 * 60;

static int secondsUntilNextUtcMidnight()
{
	const QDateTime now = QDateTime::currentDateTimeUtc();
	const QDateTime nextMidnight(now.date().addDays(1), QTime(0, 0), QTimeZone(QTimeZone::UTC));
	return static_cast<int>(now.secsTo(nextMidnight));
}

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

		// Resume a backoff that was still pending when the app last closed.
		const qint64 resumeAt = AppSettings::instance().value(kQuotaResumeAtKey, 0).toLongLong();
		const qint64 now      = QDateTime::currentSecsSinceEpoch();
		if (resumeAt > now)
			scheduleQuotaBackoff(static_cast<int>(resumeAt - now));
		else if (resumeAt != 0)
			AppSettings::instance().setValue(kQuotaResumeAtKey, 0); // stale — clear it
	}

	void stop()
	{
		// Mirrors PosterWorker::stop(): QThread::quit()/exit() called from another
		// thread (SubtitleManager::stop() does exactly that) only affects whichever
		// event loop is currently innermost for the target thread. If this worker
		// is nested inside SubtitleDownloadWorker::run()'s own blocking QEventLoop
		// (its blocking-style HTTP calls), that cross-thread quit() would hit the
		// nested loop instead of this thread's real one, leaving the real loop
		// running forever — SubtitleManager::stop()'s wait() then times out and
		// falls back to QThread::terminate(), forcibly killing the thread mid
		// network I/O, which is what crashed. cancel() synchronously aborts the
		// in-flight request, which unwinds run()'s nested loop on its own;
		// processNext() then notices m_stopping once back on the real loop and
		// quits it from there. Only safe to quit directly here when genuinely
		// idle (not nested, not mid-processFile).
		m_stopping = true;
		if (m_currentDl) {
			m_currentDl->cancel();
		} else if (!m_processing) {
			if (m_timer) m_timer->stop();
			QThread::currentThread()->quit();
		}
	}

	void cancelAll()
	{
		const bool hadWork = !m_queue.isEmpty() || m_processing;
		m_queue.clear();
		if (m_currentDl) m_currentDl->cancel();
		if (hadWork) emit queueActiveChanged(false);
	}

	void reportQuotaExceeded(int retryAfterSecs)
	{
		scheduleQuotaBackoff(retryAfterSecs > 0 ? retryAfterSecs : kDefaultRateLimitBackoffSecs);
	}

	void setCredentials(QString apiKey, QString username, QString password)
	{
		// A credentials change (new API key / different account) means whatever
		// quota we were backing off for no longer applies — but only once we're
		// past the very first call at startup, where "differs from empty" would
		// otherwise wipe out a backoff we just restored from disk.
		const bool changed = m_credentialsInitialized
		    && (apiKey != m_apiKey || username != m_username || password != m_password);
		m_apiKey   = std::move(apiKey);
		m_username = std::move(username);
		m_password = std::move(password);
		m_credentialsInitialized = true;
		if (changed) clearQuotaBackoff();
	}

	void setEnabled(bool enabled)
	{
		m_enabled = enabled;
		startTimerIfWork();
		if (canRun() && !m_queue.isEmpty()) emit queueActiveChanged(true);
	}

	void setUnderstoodLanguages(QStringList languages)
	{
		m_understoodLanguages = std::move(languages);
	}

	void setDetectSubtitleLanguage(bool detect)
	{
		m_detectSubtitleLanguage = detect;
	}

	void setEditionTokens(QStringList tokens)
	{
		m_editionTokens = std::move(tokens);
	}

	void setComputeMovieHash(bool enabled)
	{
		m_computeMovieHash = enabled;
	}

	void setRetryCooldownDays(int days)
	{
		m_retryCooldownDays = qMax(0, days);
	}

	void enqueueFile(qint64 fileId)
	{
		if (m_stopping || fileId <= 0) return;
		const bool wasEmpty = m_queue.isEmpty() && !m_processing;
		if (!m_queue.contains(fileId))
			m_queue.append(fileId);
		if (wasEmpty && !m_queue.isEmpty() && canRun()) emit queueActiveChanged(true);
		startTimerIfWork();
	}

	void enqueueBatch(const QList<qint64>& fileIds)
	{
		if (m_stopping || fileIds.isEmpty()) return;
		const bool wasEmpty = m_queue.isEmpty() && !m_processing;
		for (qint64 id : fileIds) {
			if (id > 0 && !m_queue.contains(id))
				m_queue.append(id);
		}
		if (wasEmpty && !m_queue.isEmpty() && canRun()) emit queueActiveChanged(true);
		startTimerIfWork();
	}

	// Cheap, no-network synchronous check — called cross-thread via
	// BlockingQueuedConnection from JobQueue's thread. Answers "is it worth
	// bumping this file to the front of the queue and waiting for it?" without
	// making the caller wait on anything but a DB read.
	bool hasFetchableSubtitles(qint64 fileId)
	{
		if (!canRun()) return false;
		const auto streams = DatabaseManager::instance().streamsForFile(fileId);
		return !missingSubtitleLanguages(streams, m_understoodLanguages).isEmpty();
	}

	// Move fileId to the front of the queue so it's attempted next, rather than
	// waiting behind whatever else is pending. If it's already mid-download
	// (m_processing took it out of m_queue already), this just re-queues a
	// redundant follow-up attempt — harmless, and the in-flight one still emits
	// fileAttempted() when it finishes, which is what the caller is waiting on.
	void prioritizeFile(qint64 fileId)
	{
		if (m_stopping || fileId <= 0) return;
		m_queue.removeAll(fileId);
		m_queue.prepend(fileId);
		startTimerIfWork();
	}

signals:
	void subtitlesReady(qint64 fileId, int downloadedCount);
	void quotaExhausted(qint64 resumeAtEpoch);
	void queueActiveChanged(bool active);
	// Emitted at the end of every processFile() attempt, success or not — lets a
	// bounded waiter (JobQueue's tryDownloadNow) know the attempt resolved.
	void fileAttempted(qint64 fileId);

private slots:
	void processNext()
	{
		// Guard against re-entry: SubtitleDownloadWorker::run() blocks on QEventLoop
		// while the 0ms timer keeps firing — see PosterWorker::processNext() for the
		// identical concern with TMDB's HTTP calls. A reentrant call while
		// m_processing is true means we're nested inside that loop — never safe to
		// quit the real event loop from here.
		if (m_processing) return;

		if (m_stopping) {
			if (m_timer) m_timer->stop();
			QThread::currentThread()->quit();
			return;
		}

		if (!canRun() || m_queue.isEmpty()) {
			m_timer->stop();
			if (m_queue.isEmpty()) emit queueActiveChanged(false);
			return;
		}

		m_processing = true;
		m_timer->stop();
		const qint64 fileId = m_queue.takeFirst();
		processFile(fileId);
		m_processing = false;

		if (m_stopping) {
			QThread::currentThread()->quit();
			return;
		}
		startTimerIfWork();
		if (m_queue.isEmpty()) emit queueActiveChanged(false);
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
		// Fires on every exit path (including early returns below) so a bounded
		// waiter — JobQueue's tryDownloadNow via SubtitleManager — always gets
		// unblocked once this attempt resolves, whatever the outcome.
		struct AttemptedGuard {
			SubtitleWorker* self;
			qint64 fileId;
			~AttemptedGuard() { emit self->fileAttempted(fileId); }
		} attemptedGuard{this, fileId};

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

		// Something is genuinely missing, but if we already tried this file
		// recently and OpenSubtitles had nothing, don't hammer the same search
		// every scan — wait out the cooldown. 0 disables this (always retry).
		if (m_retryCooldownDays > 0 && file.subtitleAttemptedMs > 0) {
			const qint64 cooldownMs = qint64(m_retryCooldownDays) * 24 * 60 * 60 * 1000;
			if (QDateTime::currentMSecsSinceEpoch() - file.subtitleAttemptedMs < cooldownMs)
				return;
		}

		int downloaded = 0, remaining = -1, retryAfterSecs = -1;
		bool quotaHit = false;
		{
			// Runs on this worker's own QThread/event loop, same requirement
			// documented on OpenSubtitlesClient — run() is blocking-style via
			// QEventLoop, safe to call directly since we're already off the UI thread.
			SubtitleDownloadWorker dl(m_apiKey, m_username, m_password,
			                          imdbId, missing6391, file.path, file.durationSec,
			                          m_editionTokens, m_computeMovieHash, streams);
			m_currentDl = &dl;
			connect(&dl, &SubtitleDownloadWorker::done, &dl,
			        [&downloaded, &remaining, &quotaHit, &retryAfterSecs]
			        (int dlCount, int, QString, int rem, bool quota, int retryAfter) {
				downloaded     = dlCount;
				remaining      = rem;
				quotaHit       = quota;
				retryAfterSecs = retryAfter;
			});
			dl.run();
			m_currentDl = nullptr;
		}

		// Stamp regardless of outcome — even "found nothing" is a resolved
		// attempt that should hold off the next one until the cooldown elapses.
		DatabaseManager::instance().updateSubtitleAttempted(fileId, QDateTime::currentMSecsSinceEpoch());

		if (downloaded > 0) {
			QList<StreamRecord> containerStreams;
			for (const auto& s : streams)
				if (!s.isExternal) containerStreams << s;
			const auto sidecars = ScanWorker::scanSidecarSubtitles(
			    file.path, ScanWorker::nextSidecarStreamIndex(containerStreams), m_detectSubtitleLanguage);
			auto allStreams = containerStreams;
			allStreams.append(sidecars);
			DatabaseManager::instance().insertStreams(fileId, allStreams);
			emit subtitlesReady(fileId, downloaded);
		}

		if (remaining == 0 || quotaHit) {
			// retryAfterSecs (from a 429's Retry-After header) is the most reliable
			// figure when present. Otherwise: a bare "remaining == 0" with no 429 is
			// the daily download quota, which OpenSubtitles resets at UTC midnight;
			// a 429 with no usable header is an unexplained rate limit, so fall back
			// to a conservative default rather than guessing at a reset time.
			const int backoff = retryAfterSecs > 0 ? retryAfterSecs
			    : (remaining == 0 && !quotaHit) ? secondsUntilNextUtcMidnight()
			                                     : kDefaultRateLimitBackoffSecs;
			scheduleQuotaBackoff(backoff);
		}
	}

	// Pauses the queue and persists the resume time so it survives an app
	// restart — OpenSubtitles backoffs are frequently many hours (daily quota).
	void scheduleQuotaBackoff(int seconds)
	{
		seconds = qMax(1, seconds);
		const qint64 resumeAtEpoch = QDateTime::currentSecsSinceEpoch() + seconds;
		AppSettings::instance().setValue(kQuotaResumeAtKey, resumeAtEpoch);

		const bool wasExhausted = m_quotaExhausted;
		m_quotaExhausted = true;
		++m_quotaGeneration;
		const int gen = m_quotaGeneration;
		if (m_timer) m_timer->stop();

		QTimer::singleShot(qint64(seconds) * 1000, this, [this, gen] {
			if (gen != m_quotaGeneration) return; // superseded by a newer backoff
			clearQuotaBackoff();
		});

		if (!wasExhausted) emit quotaExhausted(resumeAtEpoch);
	}

	void clearQuotaBackoff()
	{
		++m_quotaGeneration;
		m_quotaExhausted = false;
		AppSettings::instance().setValue(kQuotaResumeAtKey, 0);
		startTimerIfWork();
		if (canRun() && !m_queue.isEmpty()) emit queueActiveChanged(true);
	}

	QTimer*                 m_timer      = nullptr;
	QList<qint64>           m_queue;
	QString                 m_apiKey, m_username, m_password;
	QStringList             m_understoodLanguages;
	QStringList             m_editionTokens;
	bool                    m_enabled         = false;
	bool                    m_detectSubtitleLanguage = false;
	bool                    m_computeMovieHash = false;
	int                     m_retryCooldownDays = 7;
	bool                    m_stopping        = false;
	bool                    m_processing      = false;
	bool                    m_quotaExhausted  = false;
	bool                    m_credentialsInitialized = false;
	int                     m_quotaGeneration = 0;
	SubtitleDownloadWorker* m_currentDl       = nullptr; // valid only mid-processFile()
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
	connect(m_worker, &SubtitleWorker::queueActiveChanged,
	        this,      &SubtitleManager::queueActiveChanged);

	connect(this, &SubtitleManager::workerSetCredentials,
	        m_worker, &SubtitleWorker::setCredentials, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerSetEnabled,
	        m_worker, &SubtitleWorker::setEnabled, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerSetUnderstoodLanguages,
	        m_worker, &SubtitleWorker::setUnderstoodLanguages, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerSetDetectSubtitleLanguage,
	        m_worker, &SubtitleWorker::setDetectSubtitleLanguage, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerSetEditionTokens,
	        m_worker, &SubtitleWorker::setEditionTokens, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerSetComputeMovieHash,
	        m_worker, &SubtitleWorker::setComputeMovieHash, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerSetRetryCooldownDays,
	        m_worker, &SubtitleWorker::setRetryCooldownDays, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerEnqueueFile,
	        m_worker, &SubtitleWorker::enqueueFile, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerEnqueueBatch,
	        m_worker, &SubtitleWorker::enqueueBatch, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerCancelAll,
	        m_worker, &SubtitleWorker::cancelAll, Qt::QueuedConnection);
	connect(this, &SubtitleManager::workerReportQuotaExceeded,
	        m_worker, &SubtitleWorker::reportQuotaExceeded, Qt::QueuedConnection);
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

void SubtitleManager::setDetectSubtitleLanguage(bool detect)
{
	emit workerSetDetectSubtitleLanguage(detect);
}

void SubtitleManager::setEditionTokens(const QStringList& tokens)
{
	emit workerSetEditionTokens(tokens);
}

void SubtitleManager::setComputeMovieHash(bool enabled)
{
	emit workerSetComputeMovieHash(enabled);
}

void SubtitleManager::setRetryCooldownDays(int days)
{
	emit workerSetRetryCooldownDays(days);
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

void SubtitleManager::tryDownloadNow(qint64 fileId, int maxWaitMs)
{
	if (!m_thread || !m_thread->isRunning() || fileId <= 0) return;

	bool fetchable = false;
	QMetaObject::invokeMethod(m_worker, "hasFetchableSubtitles", Qt::BlockingQueuedConnection,
	                           Q_RETURN_ARG(bool, fetchable), Q_ARG(qint64, fileId));
	if (!fetchable) return;

	QEventLoop loop;
	const QMetaObject::Connection conn = connect(m_worker, &SubtitleWorker::fileAttempted,
	    &loop, [&loop, fileId](qint64 attemptedFileId) {
		if (attemptedFileId == fileId) loop.quit();
	});
	QTimer::singleShot(qMax(0, maxWaitMs), &loop, &QEventLoop::quit);

	QMetaObject::invokeMethod(m_worker, "prioritizeFile", Qt::QueuedConnection, Q_ARG(qint64, fileId));
	loop.exec();
	disconnect(conn);
}

void SubtitleManager::cancelAll()
{
	if (!m_thread || !m_thread->isRunning()) return;
	emit workerCancelAll();
}

void SubtitleManager::reportQuotaExceeded(int retryAfterSecs)
{
	if (!m_thread || !m_thread->isRunning()) return;
	emit workerReportQuotaExceeded(retryAfterSecs);
}

} // namespace Mc

#include "SubtitleManager.moc"
