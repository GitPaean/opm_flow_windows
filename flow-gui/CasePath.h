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

  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

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
// The base name flow actually wrote in `dir` for a deck called `deckBase`.
//
// flow upper-cases the case name, so SIMPLE_COMP_BIC_water.DATA produces
// SIMPLE_COMP_BIC_WATER.SMSPEC and friends. Deriving output paths from the
// deck's spelling then finds nothing, and the case looks like it never ran.
// Tried in turn: the deck's own spelling, the upper-cased one, and finally
// whatever the directory holds that differs only in case - which also covers
// a file written by something with its own opinion on capitalisation.
// Falls back to the deck's spelling when the directory has nothing yet, so a
// case registered before its run has written keeps working.
inline QString outputBaseName(const QString& dir, const QString& deckBase)
{
    const QString upper = deckBase.toUpper();
    for (const QString& cand : { deckBase, upper }) {
        if (QFileInfo::exists(dir + QLatin1Char('/') + cand + QStringLiteral(".SMSPEC")))
            return cand;
    }
    QDir d(dir);
    if (d.exists()) {
        const auto specs = d.entryList({ QStringLiteral("*.SMSPEC"),
                                         QStringLiteral("*.smspec") }, QDir::Files);
        for (const QString& f : specs) {
            const QString base = QFileInfo(f).completeBaseName();
            if (base.compare(deckBase, Qt::CaseInsensitive) == 0) return base;
        }
    }
    return deckBase;
}

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
