#include "core/UpdateChecker.h"
#include "core/AppSettings.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QVersionNumber>

namespace Mc {

namespace {
constexpr const char* kApiUrl = "https://api.github.com/repos/bleze/MediaCurator/releases/latest";
constexpr qint64 kMinIntervalMs = 24LL * 60 * 60 * 1000;

QVersionNumber parseTag(const QString& tagName)
{
	QString t = tagName;
	if (t.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
		t.remove(0, 1);
	return QVersionNumber::fromString(t);
}
} // namespace

UpdateChecker& UpdateChecker::instance()
{
	static UpdateChecker s_instance;
	return s_instance;
}

UpdateChecker::UpdateChecker(QObject* parent)
	: QObject(parent)
{
	m_nam = new QNetworkAccessManager(this);
}

void UpdateChecker::check(bool silent)
{
	if (m_busy)
		return;

	if (silent) {
		const qint64 lastCheckMs = AppSettings::instance().value("update/lastCheckMs", 0).toLongLong();
		if (QDateTime::currentMSecsSinceEpoch() - lastCheckMs < kMinIntervalMs)
			return;
	}

	m_busy = true;
	QNetworkRequest req{QUrl(QString::fromLatin1(kApiUrl))};
	req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MediaCurator"));
	req.setRawHeader("Accept", "application/vnd.github+json");

	QNetworkReply* reply = m_nam->get(req);
	connect(reply, &QNetworkReply::finished, this, [this, reply, silent] {
		onReplyFinished(reply, silent);
	});
}

void UpdateChecker::onReplyFinished(QNetworkReply* reply, bool silent)
{
	m_busy = false;
	reply->deleteLater();

	if (reply->error() != QNetworkReply::NoError) {
		emit checkFailed(reply->errorString(), silent);
		return;
	}

	const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
	const QString tagName = obj.value(QStringLiteral("tag_name")).toString();
	if (tagName.isEmpty()) {
		emit checkFailed(tr("Unexpected response from GitHub"), silent);
		return;
	}

	AppSettings::instance().setValue("update/lastCheckMs", QDateTime::currentMSecsSinceEpoch());

	const QVersionNumber latest  = parseTag(tagName);
	const QVersionNumber current = QVersionNumber::fromString(QCoreApplication::applicationVersion());

	if (QVersionNumber::compare(latest, current) <= 0) {
		emit upToDate(silent);
		return;
	}

	if (silent) {
		const QString skipped = AppSettings::instance().value("update/skipVersion").toString();
		if (skipped == tagName)
			return;
	}

	emit updateAvailable(tagName,
	                      obj.value(QStringLiteral("html_url")).toString(),
	                      obj.value(QStringLiteral("body")).toString(),
	                      silent);
}

void UpdateChecker::skipVersion(const QString& tagName)
{
	AppSettings::instance().setValue("update/skipVersion", tagName);
}

} // namespace Mc
