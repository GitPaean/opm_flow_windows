/*
  GuiPaths - where a file dialog should start, and keeping the process'
  working directory usable.

  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).
*/
#pragma once

#include <QProcessEnvironment>
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

// A simulator built against Intel MPI needs impi.dll, and the way to get that
// runtime without administrator rights is `pip install --user impi-rt`, which
// puts it under %APPDATA%\Python\Library\bin - a directory Windows does not
// search. Returns that directory when it holds an Intel MPI runtime, else an
// empty string. Empty is the normal answer for an MS-MPI build, whose DLL
// lives in System32.
QString intelMpiRuntimeDir();

// The environment to run flow (or mpiexec) in: the current one, with the
// directory above prepended to PATH when there is one. Handing this to
// QProcess means a user who has run that one pip command needs no further
// setup - no PATH edit, no shell, nothing to get wrong.
//
// Note that this does NOT settle which mpiexec gets launched: QProcess
// resolves a bare program name against the PATH of the process doing the
// launching, not the one set here. Use intelMpiExec() for that.
QProcessEnvironment simulatorEnvironment();

// Does this simulator link Intel MPI (rather than MS-MPI)? Read from the
// binary's own import table, so it is right for a build tree, an unpacked
// release, or a single exe someone copied somewhere.
//
// It matters because a parallel run must be started by the launcher belonging
// to the MPI the binary links, and the two are both called `mpiexec`. MS-MPI's
// hands its child a named-pipe GUID where Intel's runtime expects host:port,
// so an Intel-MPI flow started by MS-MPI's mpiexec dies in MPI_Init with
// "unable to decode hostport from <guid>" - before reading a line of the deck.
bool linksIntelMpi(const QString& simulatorExe);

// Absolute path of the Intel MPI launcher from a pip-installed runtime, or an
// empty string when there is none. Empty for a binary that needs it is worth
// reporting: launching the mpiexec on PATH instead just produces the MPI_Init
// abort above.
QString intelMpiExec();

// Does this path live somewhere that a second reader can disturb a writer -
// a network share, or a folder a sync client (OneDrive, Dropbox, ...) watches?
//
// On a local disk it does not: a reader opened by the C++ runtime denies
// nothing (measured on Windows: MSVC opens with _SH_DENYNO, and a writer opens
// and writes while a reader holds the file), so reading a simulator's output
// while it runs is safe. Over SMB an oplock break, and under a sync client a
// scan of a file that just changed, can each turn that read into a real
// sharing violation - for the WRITER, i.e. the simulation, not the reader.
// So this is the question worth asking before reading a running case's files.
//
// A false answer is the safe one to guess wrong: it only means reading a file
// that was going to be readable anyway.
bool isSyncedOrNetworkPath(const QString& path);

} // namespace flowgui
