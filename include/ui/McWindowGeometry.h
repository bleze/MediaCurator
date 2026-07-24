#pragma once

#include <QSize>

class QWidget;

namespace Mc {

// Clamps a top-level widget's geometry to fit within the available geometry
// (screen area minus taskbar) of the screen it's on. Restoring a saved
// geometry blob (QWidget::restoreGeometry) does not account for the widget
// having last been sized/positioned on a different monitor — e.g. a size
// saved on a 1080p display can end up taller than a 4K screen's available
// area, pushing a dialog's button bar below the taskbar with no way to
// shrink past its minimum size. Call this right after restoreGeometry().
void ensureGeometryFitsScreen(QWidget* widget);

// Clamps `desired` to fit within the available geometry of the screen
// `widget` is on (or the primary screen, before it's placed on one), minus
// `margin` logical pixels of headroom. Use this to compute a hardcoded
// setMinimumSize() so it can't force a dialog larger than the screen it
// opens on — e.g. at 300% DPI scaling a 4K display's *logical* resolution
// (~1280x720) can be smaller than a minimum size tuned for a 1080p-era
// screen, and once that's the widget's enforced minimum, no amount of
// clamping via ensureGeometryFitsScreen() can shrink it back down.
QSize clampSizeToScreen(QWidget* widget, QSize desired, int margin = 20);

} // namespace Mc
