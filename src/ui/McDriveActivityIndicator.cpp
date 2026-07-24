#include "ui/McDriveActivityIndicator.h"
#include "core/DriveActivityMonitor.h"
#include "core/StorageGroupSettings.h"
#include "ui/McCardDelegate.h"

#include <QDateTime>
#include <QEasingCurve>
#include <QPainter>
#include <QPropertyAnimation>

namespace Mc {

McDriveActivityIndicator::McDriveActivityIndicator(QWidget* parent)
    : QWidget(parent)
{
	setToolTip(tr("Drive activity — lights up when MediaCurator touches a storage"
	              " drive, then fades over that group's configured spin-down time"
	              " (Manage Folders…)."));

	m_fade = new QPropertyAnimation(this, "level", this);
	m_fade->setEasingCurve(QEasingCurve::Linear);

	// Resume mid-fade across a restart instead of always coming up idle —
	// the drive may well still be spinning from something touched just
	// before the app last closed (see DriveActivityMonitor::persist()).
	const auto last = DriveActivityMonitor::loadPersisted();
	if (last.group >= 1) {
		const qint64 totalMs   = qint64(StorageGroupSettings::spinDownMinutes(last.group)) * 60 * 1000;
		const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - last.epochMs;
		if (elapsedMs >= 0 && elapsedMs < totalMs) {
			m_activeGroup = last.group;
			const qreal level = 1.0 - double(elapsedMs) / double(totalMs);
			setLevel(level);
			m_fade->setStartValue(level);
			m_fade->setEndValue(0.0);
			m_fade->setDuration(int(totalMs - elapsedMs));
			m_fade->start();
		}
	}

	connect(&DriveActivityMonitor::instance(), &DriveActivityMonitor::activity,
	        this, &McDriveActivityIndicator::onActivity);
}

void McDriveActivityIndicator::setLevel(qreal level)
{
	m_level = level;
	update();
}

QSize McDriveActivityIndicator::sizeHint() const
{
	return { kIconSize + 6, kIconSize + 4 };
}

void McDriveActivityIndicator::onActivity(int group)
{
	m_activeGroup = group;

	m_fade->stop();
	setLevel(1.0);
	m_fade->setStartValue(1.0);
	m_fade->setEndValue(0.0);
	m_fade->setDuration(StorageGroupSettings::spinDownMinutes(group) * 60 * 1000);
	m_fade->start();
}

void McDriveActivityIndicator::paintEvent(QPaintEvent*)
{
	// Same blue used for a checked/toggled-on toolbar button (Job Queue,
	// Leaderboard — see setupToolBar()'s QToolButton:checked rule, which
	// also bakes in QPalette::Highlight once at startup as a fixed rgba()
	// string). Explicitly asking for the Active group here matches that —
	// plain palette().color(QPalette::Highlight) uses palette()'s *current*
	// color group, which Qt swaps to Inactive (a dimmed variant) whenever the
	// window loses focus, so the indicator would darken on its own for a
	// reason unrelated to drive activity.
	const QColor hot = palette().color(QPalette::Active, QPalette::Highlight);
	const QColor idle(90, 90, 90);
	const qreal  t = qBound(0.0, m_level, 1.0);
	const QColor tint(
	    int(idle.red()   + (hot.red()   - idle.red())   * t),
	    int(idle.green() + (hot.green() - idle.green()) * t),
	    int(idle.blue()  + (hot.blue()  - idle.blue())  * t));

	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.setOpacity(0.45 + 0.55 * t);

	const qreal dpr = devicePixelRatioF();
	const QPixmap pm = McCardDelegate::renderSvgIcon(QStringLiteral(":/icons/storage_group.svg"),
	                                                  tint, kIconSize, dpr);
	if (!pm.isNull()) {
		const int x = (width()  - kIconSize) / 2;
		const int y = (height() - kIconSize) / 2;
		p.drawPixmap(x, y, pm);
	}
}

} // namespace Mc
