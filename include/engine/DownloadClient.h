#pragma once
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

namespace Mc {

struct DownloadQueueItem {
	QString providerId;      // e.g. "nzbget"
	QString name;
	qint64  totalMb     = 0;
	qint64  remainingMb = 0;
	QString status;           // free-text, e.g. "Downloading", "Paused", "Queued"
};

/**
 * DownloadClient — base for a background-download-client integration
 * (NZBGet, SABnzbd today; qBittorrent/etc. are future siblings). Each
 * instance owns its own polling (timer + network client); McMainWindow only
 * ever talks to DownloadClientRegistry, never a concrete subclass directly.
 *
 * Qt has no multiple QObject inheritance, so this is a QObject base with
 * pure virtuals rather than a separate abstract interface — same shape as
 * how HighscoreClient/UpdateChecker own their own QNetworkAccessManager.
 */
class DownloadClient : public QObject
{
	Q_OBJECT
public:
	~DownloadClient() override = default;

	[[nodiscard]] virtual QString providerId() const   = 0;   // "nzbget"
	[[nodiscard]] virtual QString displayName() const  = 0;   // "NZBGet"
	[[nodiscard]] virtual bool    isConfigured() const = 0;   // host set + enabled

	// Re-reads settings and starts/stops polling accordingly.
	virtual void reconfigure() = 0;

	// Fires an immediate out-of-cycle poll (Test Connection, manual refresh).
	virtual void pollNow() = 0;

signals:
	void queueUpdated(QList<Mc::DownloadQueueItem> items);
	void downloadsCompleted(QStringList names);   // successful completions since last poll
	void connectionError(QString message);

protected:
	explicit DownloadClient(QObject* parent = nullptr) : QObject(parent) {}
};

} // namespace Mc

Q_DECLARE_METATYPE(Mc::DownloadQueueItem)
