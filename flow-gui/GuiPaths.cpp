/*
  Copyright (C) 2026 SINTEF Digital

  GuiPaths implementation. Part of the opm_flow_windows harness; GPL v3+
  (see repository LICENSE).
*/
#include "GuiPaths.h"

#include <QDir>
#include <QFileInfo>

QString flowgui::ensureWorkingDirectory()
{
    // QDir::currentPath() is empty when getcwd() itself fails, which is what
    // a deleted working directory looks like.
    const QString cwd = QDir::currentPath();
    if (!cwd.isEmpty() && QFileInfo::exists(cwd)) return QString();

    const QString home = QDir::homePath();
    if (!QDir::setCurrent(home)) return QString();
    return home;
}
