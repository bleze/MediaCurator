#pragma once
#include <QString>

namespace Mc {

// Opens the given file's containing folder. By default this goes through the
// shell's "open" verb, so it respects whatever the user has set as their
// system default file manager (e.g. Directory Opus) — but it can't select
// the file within that folder, since that's not something the shell's plain
// "open" verb supports.
//
// If the user has enabled Settings > Interface > "Always use File Explorer
// for Open Containing Folder", this instead launches explorer.exe directly
// with /select, which highlights the file but always uses File Explorer
// specifically, regardless of the system default.
void revealInFileManager(const QString& path);

} // namespace Mc
