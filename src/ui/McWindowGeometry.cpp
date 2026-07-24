#include "ui/McWindowGeometry.h"

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QWidget>

namespace Mc {

void ensureGeometryFitsScreen(QWidget* widget)
{
	QScreen* screen = widget->screen();
	if (!screen)
		screen = QGuiApplication::primaryScreen();
	if (!screen)
		return;

	const QRect avail = screen->availableGeometry();
	const QRect geo   = widget->geometry();

	const int width  = qMin(geo.width(),  avail.width());
	const int height = qMin(geo.height(), avail.height());
	const int x      = qBound(avail.left(), geo.x(), avail.right()  - width  + 1);
	const int y      = qBound(avail.top(),  geo.y(), avail.bottom() - height + 1);

	if (x != geo.x() || y != geo.y() || width != geo.width() || height != geo.height())
		widget->setGeometry(x, y, width, height);
}

QSize clampSizeToScreen(QWidget* widget, QSize desired, int margin)
{
	QScreen* screen = widget->screen();
	if (!screen)
		screen = QGuiApplication::primaryScreen();
	if (!screen)
		return desired;

	const QRect avail = screen->availableGeometry();
	return QSize(qMin(desired.width(),  avail.width()  - margin),
	             qMin(desired.height(), avail.height() - margin));
}

} // namespace Mc
