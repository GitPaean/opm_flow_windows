/*
  GuiPaths - where a file dialog should start, and keeping the process'
  working directory usable.

  Copyright (C) 2026 SINTEF Digital

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QString>

namespace flowgui {

// Where a file dialog of this kind should open: the folder of `current` when
// that names something on disk (the field the dialog belongs to is the best
// guess there is), else where a dialog of this kind was left last time, else
// the home directory. `key` names the kind - "deck", "figure", "smspec", ...
QString startDir(const QString& key, const QString& current = QString());

// Remember where a dialog ended up, for the next one with the same key.
void rememberDir(const QString& key, const QString& chosenPath);

// opm-common resolves summary and grid files against the process' working
// directory - ESmry calls std::filesystem::current_path() even when handed an
// absolute name - so a working directory that has been DELETED (running the
// GUI from a run folder that a later run cleaned out, say) makes every case
// unreadable with "cannot get current path". Step out of it when that has
// happened. Returns the directory moved to, or an empty string if the working
// directory was fine.
QString ensureWorkingDirectory();

} // namespace flowgui
