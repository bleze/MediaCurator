#pragma once
#include <QColor>
#include <QIcon>

namespace Mc {

// Returns a theme-aware, HiDPI-safe icon from an SVG resource.
// The fill color tracks QPalette::ButtonText for normal/disabled modes.
// Do NOT use this for brand icons (app_icon.svg) that have intentional colors.
QIcon svgIcon(const QString& resourcePath);

// Same, but fixed to the given color instead of tracking the palette — for
// icons drawn on a fixed-color background (e.g. a colored pill/badge fill)
// where the theme's ButtonText color wouldn't contrast reliably.
QIcon svgIcon(const QString& resourcePath, const QColor& color);

} // namespace Mc
