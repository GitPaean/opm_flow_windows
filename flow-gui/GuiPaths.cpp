/*
  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  GuiPaths implementation. Part of the opm_flow_windows harness; GPL v3+
  (see repository LICENSE).
*/
#include "GuiPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QtEndian>

#include <algorithm>

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

#ifdef Q_OS_WIN
// --- just enough PE to read a binary's import table --------------------------
//
// Which mpiexec is allowed to launch a binary is decided by the MPI library
// that binary links, and the only place that is recorded is its import table.
// Reading it costs a few hundred bytes of seeks. The alternatives - a marker
// file beside the exe, or guessing from the build directory's name - are state
// that survives a copy of the exe on its own, or gets out of step with it.

bool readAt(QFile& f, qint64 off, void* dst, int n)
{
    return f.seek(off) && f.read(static_cast<char*>(dst), n) == n;
}

template <typename T>
bool readLE(QFile& f, qint64 off, T& out)
{
    T raw{};
    if (!readAt(f, off, &raw, sizeof(T))) return false;
    out = qFromLittleEndian(raw);
    return true;
}

// Case-insensitive match of `wanted` against the DLLs `exePath` imports.
// False for anything unreadable or malformed, so a file this cannot parse
// leaves the caller on its previous behaviour rather than failing the run.
bool importsAnyOf(const QString& exePath, const QStringList& wanted)
{
    QFile f(exePath);
    if (!f.open(QIODevice::ReadOnly)) return false;

    quint16 mz = 0;
    if (!readLE(f, 0, mz) || mz != 0x5A4D) return false;              // "MZ"
    quint32 peOff = 0;
    if (!readLE(f, 0x3C, peOff)) return false;
    quint32 sig = 0;
    if (!readLE(f, peOff, sig) || sig != 0x00004550) return false;    // "PE\0\0"

    const qint64 coff = peOff + 4;                                    // file header
    quint16 nSections = 0, optSize = 0, optMagic = 0;
    if (!readLE(f, coff + 2,  nSections)) return false;
    if (!readLE(f, coff + 16, optSize))   return false;
    const qint64 optOff = coff + 20;                                  // optional header
    if (!readLE(f, optOff, optMagic))     return false;

    // The data directories sit at the end of the optional header; PE32 and
    // PE32+ differ only in the fields before them.
    qint64 dirs = 0;
    if      (optMagic == 0x20B) dirs = optOff + 112;                  // PE32+
    else if (optMagic == 0x10B) dirs = optOff + 96;                   // PE32
    else return false;

    quint32 importRva = 0;                                            // directory 1
    if (!readLE(f, dirs + 8, importRva) || importRva == 0) return false;

    // Section table, so an RVA can be turned into a file offset.
    struct Section { quint32 va = 0, vsize = 0, rawSize = 0, rawPtr = 0; };
    QList<Section> sections;
    const qint64 secOff = optOff + optSize;
    for (quint16 i = 0; i < nSections; ++i) {
        const qint64 s = secOff + qint64(i) * 40;
        Section sec;
        if (!readLE(f, s +  8, sec.vsize))   return false;
        if (!readLE(f, s + 12, sec.va))      return false;
        if (!readLE(f, s + 16, sec.rawSize)) return false;
        if (!readLE(f, s + 20, sec.rawPtr))  return false;
        sections.append(sec);
    }
    const auto toOffset = [&sections](quint32 rva) -> qint64 {
        for (const Section& s : sections) {
            const quint32 span = std::max(s.vsize, s.rawSize);
            if (rva >= s.va && rva < s.va + span)
                return qint64(s.rawPtr) + (rva - s.va);
        }
        return -1;
    };

    const qint64 impOff = toOffset(importRva);
    if (impOff < 0) return false;
    // A zeroed descriptor ends the array. The iteration bound only guards a
    // corrupt table; nothing imports anywhere near this many DLLs.
    for (int i = 0; i < 4096; ++i) {
        quint32 nameRva = 0;
        if (!readLE(f, impOff + qint64(i) * 20 + 12, nameRva)) return false;
        if (nameRva == 0) break;
        const qint64 nameOff = toOffset(nameRva);
        if (nameOff < 0) continue;
        char buf[64] = {};
        if (!f.seek(nameOff)) continue;
        if (f.read(buf, sizeof(buf) - 1) <= 0) continue;
        const QString dll = QString::fromLatin1(buf);   // stops at the NUL
        for (const QString& w : wanted)
            if (dll.compare(w, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}
#endif // Q_OS_WIN

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

QString flowgui::intelMpiRuntimeDir()
{
#ifdef Q_OS_WIN
    // Where the impi-rt wheel lands. Deliberately a per-user location: that is
    // what makes `pip install --user impi-rt` possible without administrator
    // rights, which is the whole reason this path exists.
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty()) return QString();
    const QString dir = QDir::cleanPath(appData + QStringLiteral("/Python/Library/bin"));
    // Test for the library itself, not the directory: a stale or half-removed
    // install would otherwise be put on PATH and change nothing but the error.
    if (!QFileInfo::exists(dir + QStringLiteral("/impi.dll"))) return QString();
    return QDir::toNativeSeparators(dir);
#else
    return QString();
#endif
}

bool flowgui::linksIntelMpi(const QString& simulatorExe)
{
#ifdef Q_OS_WIN
    return importsAnyOf(simulatorExe, {QStringLiteral("impi.dll")});
#else
    Q_UNUSED(simulatorExe);
    return false;
#endif
}

QString flowgui::intelMpiExec()
{
    const QString dir = intelMpiRuntimeDir();
    if (dir.isEmpty()) return QString();
    const QString exe = QDir(dir).filePath(QStringLiteral("mpiexec.exe"));
    if (!QFileInfo::exists(exe)) return QString();
    return QDir::toNativeSeparators(exe);
}

QProcessEnvironment flowgui::simulatorEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString impi = intelMpiRuntimeDir();
    if (impi.isEmpty()) return env;

    // Prepend rather than append: if some other MPI is also on PATH, the one
    // this binary was linked against has to win. mpiexec and the hydra proxies
    // live in the same directory, so this covers the parallel launch too.
    const QString sep  = QDir::listSeparator();
    const QString path = env.value(QStringLiteral("PATH"));
    env.insert(QStringLiteral("PATH"), path.isEmpty() ? impi : impi + sep + path);
    return env;
}
