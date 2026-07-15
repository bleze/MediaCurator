#pragma once
#include "core/DatabaseManager.h"
#include <QString>
#include <functional>

class QMenu;

namespace Mc {

// Populates menu with the shared track-badge context menu: Default/Forced/Original
// flag toggle rows (Original hidden for non-audio tracks) and, for an unlabeled
// subtitle, a "Set Language" submenu. Shared by the Library and Job Queue views so
// both edit tracks the same way. Flag toggles and language picks apply immediately
// (via TrackFlagService) — callers only need to supply what happens on success/
// failure via the callbacks, not how the edit is applied.
//
// showFlagRowsForExternal controls Default/Forced visibility for a sidecar stream:
// a sidecar isn't a container track, so setting its flags only takes effect once it's
// actually muxed in during a remux — meaningful to show while reviewing a Queue job,
// but invisible and confusing as a Library-view edit. Pass false from the Library,
// true from the Queue. Has no effect on embedded tracks (always shown) or the
// language submenu (always shown for an unlabeled subtitle, external or not).
void buildTrackFlagMenu(QMenu& menu, const StreamRecord& stream, const QString& fileOriginalLanguage,
                        qreal devicePixelRatio, bool showFlagRowsForExternal,
                        const std::function<void(const QString& flag, bool value)>& onFlagToggled,
                        const std::function<void(const QString& langCode)>& onLanguageChosen);

} // namespace Mc
