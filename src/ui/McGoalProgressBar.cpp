#include "ui/McGoalProgressBar.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QtMath>

namespace Mc {

namespace {
// Same 3px "size bar" look as McCardDelegate's live job-card bar (and its static
// reproductions in the onboarding/splash/about banners) — track color kept
// identical so this reads as the same visual language, not a new one.
constexpr int kBarHeight = 3;
const QColor kTrackColor(0x30, 0x30, 0x40);

// Same light/dark green pulse as McCardDelegate's pulseRed() (despite the name,
// it's green) — a 2000ms cosine cycle between the two shades, signalling "this is
// actively running" the same way the per-card bar does.
QColor pulseGreen(qint64 nowMs)
{
	constexpr qint64 kPeriodMs = 2000;
	const QColor light(0x5a, 0xe8, 0x5a);
	const QColor dark (0x14, 0x8f, 0x14);
	const double phase = (nowMs % kPeriodMs) / double(kPeriodMs);
	const double s     = (1.0 + qCos(2.0 * M_PI * phase)) / 2.0;
	return QColor(
	    qRound(dark.red()   + (light.red()   - dark.red())   * s),
	    qRound(dark.green() + (light.green() - dark.green()) * s),
	    qRound(dark.blue()  + (light.blue()  - dark.blue())  * s));
}
}

McGoalProgressBar::McGoalProgressBar(QWidget* parent)
	: QWidget(parent)
{
	setCursor(Qt::PointingHandCursor);
	setToolTip(tr("Click to raise the goal"));
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setFixedHeight(kBarHeight);

	// Same repaint cadence as McCardDelegate's pulse heartbeat — the color itself
	// depends on wall-clock time, so nothing besides a timer would ever redraw it.
	m_pulseTimer = new QTimer(this);
	m_pulseTimer->setTimerType(Qt::PreciseTimer);
	connect(m_pulseTimer, &QTimer::timeout, this, [this] {
		if (m_goalBytes > 0) update();
	});
	m_pulseTimer->start(33);
}

void McGoalProgressBar::setGoal(qint64 bytes)
{
	m_goalBytes = bytes;
	update();
}

void McGoalProgressBar::setCompleted(qint64 bytes)
{
	m_completedBytes = bytes;
	update();
}

void McGoalProgressBar::setLive(qint64 bytes)
{
	m_liveBytes = bytes;
	update();
}

void McGoalProgressBar::mousePressEvent(QMouseEvent* event)
{
	if (event->button() == Qt::LeftButton) emit clicked();
	QWidget::mousePressEvent(event);
}

void McGoalProgressBar::paintEvent(QPaintEvent*)
{
	if (m_goalBytes <= 0) return;

	QPainter painter(this);
	const QRect barRect(0, 0, width(), kBarHeight);
	painter.fillRect(barRect, kTrackColor);

	const double frac = qBound(0.0, double(currentBytes()) / double(m_goalBytes), 1.0);
	if (frac > 0.0) {
		QRect r = barRect;
		r.setWidth(int(barRect.width() * frac));
		painter.fillRect(r, pulseGreen(QDateTime::currentMSecsSinceEpoch()));
	}
}

} // namespace Mc
