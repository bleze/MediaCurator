#pragma once

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

} // namespace Mc
