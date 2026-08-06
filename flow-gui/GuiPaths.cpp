/*
  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  GuiPaths implementation. Part of the opm_flow_windows harness; GPL v3+
  (see repository LICENSE).
*/
#include "GuiPaths.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace {

QString key(const QString& kind) { return QStringLiteral("dirs/") + kind; }

// The folder a path names: itself when it is a directory, its parent when it
// is (or would be) a file.
QString folderOf(const QString& path)
{
    const QString p = path.trimmed();
    if (p.isEmpty()) return QString();
    const QFileInfo fi(p);
    return fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
}

} // namespace

QString flowgui::startDir(const QString& kind, const QString& current)
{
    // What the field already points at wins: that is where the user is
    // working right now, whatever an older dialog of this kind used.
    const QString here = folderOf(current);
    if (!here.isEmpty() && QFileInfo::exists(here)) return here;

    const QString last = QSettings().value(key(kind)).toString();
    if (!last.isEmpty() && QFileInfo::exists(last)) return last;

    return QDir::homePath();
}

void flowgui::rememberDir(const QString& kind, const QString& chosenPath)
{
    const QString dir = folderOf(chosenPath);
    if (dir.isEmpty()) return;
    QSettings().setValue(key(kind), dir);
}

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
