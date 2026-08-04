/*
  CasePath - one spelling per case, so the same run is never registered twice.

  A case is identified by the path of its SMSPEC (the 3D tab uses the EGRID
  sibling), and the same file reaches the GUI written several different ways:
  "C:/dir/X.SMSPEC" from a file dialog, a dropped URL or a default <deck>_run
  directory; "C:\dir\X.SMSPEC" from a custom output directory, which is stored
  in native separators; and whatever spelling a project file was saved with.
  Compared as raw strings those look like different cases, so one folder could
  end up in the list twice - and since the tags are derived from the folder,
  the two entries read alike and only a "(2)" counter told them apart.

  Copyright (C) 2026 SINTEF Digital

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QtGlobal>

namespace flowgui {

// The spelling a case is stored and compared under: absolute, '/'-separated,
// free of redundant "." and ".." sections.
//
// Deliberately not canonicalFilePath(): that also resolves symlinks, but it
// returns an empty string while the file does not exist yet - and a case is
// registered as soon as its job starts, before flow has written anything.
inline QString normalizeCasePath(const QString& path)
{
    if (path.isEmpty()) return path;
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

// Do two spellings name the same file? Windows file names are case-insensitive,
// so "C:/Runs/X.SMSPEC" and "c:/runs/x.smspec" are one case there and two on
// Linux - which is how each platform's file system behaves.
inline bool sameCasePath(const QString& a, const QString& b)
{
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    return QString::compare(normalizeCasePath(a), normalizeCasePath(b), cs) == 0;
}

} // namespace flowgui
