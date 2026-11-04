#include "ui/McHighscoreBand.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>

namespace Mc {

namespace {
// Mirrors McMainWindow::updateSavedLabel()'s status-bar formatting so the
// leaderboard reads in the same units as the rest of the app.
QString formatReclaimed(qint64 mb)
{
	const double gb = mb / 1024.0;
	if (gb >= 1024.0)
		return QObject::tr("%1 TB").arg(gb / 1024.0, 0, 'f', 2);
	return gb >= 1.0
	    ? QObject::tr("%1 GB").arg(gb, 0, 'f', 2)
	    : QObject::tr("%1 MB").arg(static_cast<double>(mb), 0, 'f', 1);
}
}

McHighscoreBand::McHighscoreBand(QWidget* parent)
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
	m_label->setTextFormat(Qt::RichText);
	layout->addWidget(m_label, 1);
}

void McHighscoreBand::setEntries(const QList<HighscoreEntry>& topEntries)
{
	QStringList parts;
	int rank = 1;
	for (const HighscoreEntry& e : topEntries) {
		parts << QStringLiteral("%1. %2 — %3").arg(rank++).arg(e.name.toHtmlEscaped(),
		                                                       formatReclaimed(e.score));
	}
	m_label->setText(QStringLiteral("\U0001F3C6 %1&nbsp;&nbsp;&nbsp;› %2")
	                      .arg(parts.join(QStringLiteral("&nbsp;&nbsp;&nbsp;")), tr("View leaderboard")));
}

void McHighscoreBand::mousePressEvent(QMouseEvent* e)
{
	if (e->button() == Qt::LeftButton)
		emit clicked();
	QWidget::mousePressEvent(e);
}

} // namespace Mc
