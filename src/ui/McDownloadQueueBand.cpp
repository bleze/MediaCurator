#include "ui/McDownloadQueueBand.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QProgressBar>
#include <QStringList>

namespace Mc {

namespace {
QString formatMb(qint64 mb)
{
	const double gb = mb / 1024.0;
	return gb >= 1.0
	    ? QObject::tr("%1 GB").arg(gb, 0, 'f', 2)
	    : QObject::tr("%1 MB").arg(static_cast<double>(mb), 0, 'f', 0);
}

bool isPaused(const QString& status)
{
	return status.contains(QStringLiteral("PAUSED"), Qt::CaseInsensitive);
}

bool isActivelyDownloading(const QString& status)
{
	return status.compare(QStringLiteral("DOWNLOADING"), Qt::CaseInsensitive) == 0;
}
}

McDownloadQueueBand::McDownloadQueueBand(QWidget* parent)
	: QWidget(parent)
{
	setCursor(Qt::PointingHandCursor);
	setAutoFillBackground(true);

	const QColor h = palette().color(QPalette::Highlight);
	QPalette pal = palette();
	pal.setColor(QPalette::Window, QColor(h.red(), h.green(), h.blue(), 40));
	setPalette(pal);

	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(10, 4, 10, 4);

	m_label = new QLabel(this);
	layout->addWidget(m_label, 1);

	m_progress = new QProgressBar(this);
	m_progress->setRange(0, 100);
	m_progress->setFixedWidth(160);
	m_progress->setTextVisible(true);
	layout->addWidget(m_progress);
}

void McDownloadQueueBand::setItems(const QList<DownloadQueueItem>& items)
{
	qint64 totalMb             = 0;
	qint64 remainingMb         = 0;
	qint64 downloadingTotalMb     = 0;
	qint64 downloadingRemainingMb = 0;
	int    downloadingCount = 0;
	int    pausedCount      = 0;
	for (const DownloadQueueItem& item : items) {
		totalMb     += item.totalMb;
		remainingMb += item.remainingMb;
		if (isPaused(item.status)) {
			++pausedCount;
		} else if (isActivelyDownloading(item.status)) {
			++downloadingCount;
			downloadingTotalMb     += item.totalMb;
			downloadingRemainingMb += item.remainingMb;
		}
	}
	const int otherCount = items.size() - downloadingCount - pausedCount;   // queued, post-processing, etc.

	// A 0%-and-motionless bar reads as "stuck," not "idle" — only show it
	// while something is actually being fetched.
	m_progress->setVisible(downloadingCount > 0);
	if (downloadingCount > 0) {
		// Scoped to actively-downloading items only, not totalMb/remainingMb
		// (all items) — a paused job sitting at 0% would otherwise drag the
		// whole bar down even though it isn't contributing to it at all.
		const int percent = downloadingTotalMb > 0
		    ? qBound(0, static_cast<int>((downloadingTotalMb - downloadingRemainingMb) * 100 / downloadingTotalMb), 100)
		    : 0;
		m_progress->setValue(percent);
	}

	if (items.size() == 1) {
		const DownloadQueueItem& item = items.first();
		const QString icon = isPaused(item.status) ? QStringLiteral("⏸") : QStringLiteral("\U0001F4E5");
		const QString verb = isPaused(item.status) ? tr("Paused") : tr("Downloading");
		m_label->setText(tr("%1 %2: %3 (%4 remaining) — › View queue")
		    .arg(icon, verb, item.name.toHtmlEscaped(), formatMb(remainingMb)));
		return;
	}

	QStringList parts;
	if (downloadingCount > 0)
		parts << tr("%n downloading", "", downloadingCount);
	if (pausedCount > 0)
		parts << tr("%n paused", "", pausedCount);
	if (otherCount > 0)
		parts << tr("%n queued", "", otherCount);

	const QString icon = downloadingCount > 0 ? QStringLiteral("\U0001F4E5") : QStringLiteral("⏸");
	m_label->setText(tr("%1 %2 — %3 remaining — › View queue")
	    .arg(icon, parts.join(QStringLiteral(", ")), formatMb(remainingMb)));
}

void McDownloadQueueBand::mousePressEvent(QMouseEvent* e)
{
	if (e->button() == Qt::LeftButton)
		emit clicked();
	QWidget::mousePressEvent(e);
}

} // namespace Mc
