# flow-gui-qt

A cross-platform (Windows / Linux / macOS) **Qt 6** GUI front end for running
[OPM Flow](https://opm-project.org) simulations — functional parity with the
FLTK `flow-gui` in this repository, and the better starting point for further
extension (dialogs, models, and later plotting via Qwt or Qt Charts).

Queue up input decks, pick the simulator binary and MPI/OpenMP settings, run
the queue sequentially, and watch the live simulator log. A running job
(including all of its MPI ranks) can be stopped at any time.

## Features
- **Job queue** of `*.DATA` input decks (add / remove / clear, multi-select).
- **Simulator** — always the `flow`(`.exe`) shipped next to the GUI (in a
  development checkout it falls back to the harness build tree); the resolved
  path is shown in the log at startup. `flow` contains every model variant,
  so no picker is needed.
- **Parallel runs** — MPI rank count (spawns `mpiexec -n N ...`) and
  `--threads-per-process` OpenMP threads; both default to 1 (serial).
- **Output directory policy** — per-deck `<deck>_run` next to the deck
  (default), or a custom directory used exactly as given (the directory
  chooser has a *New Folder* button on every platform; the path is created
  if needed).
- **Extra options** passed through to flow verbatim
  (e.g. `--linear-solver=ilu0`).
- **Live log** — merged stdout/stderr streams into the window while running.
- **Stop job** — kills the entire process tree (Windows: `taskkill /T`;
  Linux/macOS: the child leads a process group that is signalled as a whole).
- **Persistent settings** — simulator path, ranks/threads, output policy and
  extra options are remembered between sessions (QSettings).

## Building

### Windows (this harness)
Use the official **prebuilt** Qt (signed binaries; fast, and it avoids a
known pitfall: building Qt from source executes its own freshly built
`moc`/`rcc` tools, which Windows Smart App Control blocks on machines where
it is enforced — the same applies to vcpkg's `qtbase` port):
```powershell
winget install -e --id Python.Python.3.12
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --archives qtbase -O C:\Qt

. .\setup-env.ps1
cmake -S flow-gui-qt -B build-gui-qt -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build-gui-qt
C:\Qt\6.8.3\msvc2022_64\bin\windeployqt --release --no-translations build-gui-qt\flow-gui-qt.exe
build-gui-qt\flow-gui-qt.exe
```
`windeployqt` copies the required Qt DLLs and the platform plugin next to
the executable.

### Linux
```bash
sudo apt install qt6-base-dev            # or: dnf install qt6-qtbase-devel
cmake -S flow-gui-qt -B build-gui-qt -DCMAKE_BUILD_TYPE=Release
cmake --build build-gui-qt
./build-gui-qt/flow-gui-qt
```

### macOS
```bash
brew install qt
cmake -S flow-gui-qt -B build-gui-qt -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build-gui-qt
open build-gui-qt/flow-gui-qt.app
```

## Usage
1. **Add deck...** one or more `*.DATA` files to the queue (the simulator is
   the `flow` executable shipped with the GUI — see the log's first lines).
2. Choose **MPI ranks** / **OMP threads** (1/1 = serial), output policy and
   any extra flow options.
3. **Run queue** — jobs run one after another; the log streams live.
4. **Stop job** kills the currently running job and aborts the remainder of
   the queue.

`flow-gui-qt --version` prints the version and exits (headless smoke test).

## Extending
The application is deliberately a single `main.cpp` with an event-driven
`QProcess` job runner — no worker threads. Natural next steps:
- summary-vector plots (Qwt `QwtPlot` or Qt Charts) fed from `*.UNSMRY`,
- a PRT/DBG file viewer tab,
- per-deck run options and a persistent job list (`QListWidget` → model/view).

## Notes
- For MPI runs, `mpiexec` must be on `PATH` (on Windows it is after
  installing MS-MPI; run from a `setup-env.ps1` shell or add
  `C:\Program Files\Microsoft MPI\Bin` to `PATH`).
- On Windows, remember the firewall pre-authorization for freshly built
  simulators (`allow-firewall.ps1`, see the harness README).
