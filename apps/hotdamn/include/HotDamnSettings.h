#pragma once

// Shared between main.cpp (which decides whether to hand a file to a running
// instance) and the window (which owns the Options menu).
namespace HotDamn {

inline constexpr const char* kKeyReuseWindow = "options/reuseWindow";

// Local-socket name the first instance listens on.
inline constexpr const char* kInstanceServerName = "HotDamnNfoViewer";

} // namespace HotDamn
