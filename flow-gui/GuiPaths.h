/*
  GuiPaths - keeping the process' working directory usable.

  Copyright (C) 2026 SINTEF Digital

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QString>

namespace flowgui {

// opm-common resolves summary and grid files against the process' working
// directory - ESmry calls std::filesystem::current_path() even when handed an
// absolute name - so a working directory that has been DELETED (running the
// GUI from a run folder that a later run cleaned out, say) makes every case
// unreadable with "cannot get current path". Step out of it when that has
// happened. Returns the directory moved to, or an empty string if the working
// directory was fine.
QString ensureWorkingDirectory();

} // namespace flowgui
