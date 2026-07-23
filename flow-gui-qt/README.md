# flow-gui-qt

A cross-platform (Windows / Linux / macOS) **Qt 6** GUI front end for running
[OPM Flow](https://opm-project.org) simulations — functional parity with the
FLTK `flow-gui` in this repository, and the better starting point for further
extension (dialogs, models, and later plotting via Qwt or Qt Charts).

Queue up input decks, pick the simulator binary and MPI/OpenMP settings, run
the queue sequentially, and watch the live simulator log. A running job
(including all of its MPI ranks) can be stopped at any time.

## Features
- **Projects** — the *Project* menu saves/loads a `.opmproj` file (readable
  JSON) holding the deck queue, MPI ranks / OMP threads, output policy and
  directory, extra flow arguments, and the Results-tab cases with their
  checked state — so a whole study setup is one *Open* away (Ctrl+O/Ctrl+S;
  missing decks/cases are skipped with a note in the log).
- **Job queue table** of `*.DATA` input decks (add / remove / clear,
  multi-select, **drag & drop** onto the window) with per-job **status,
  progress bar, elapsed time and ETA** parsed live from flow's
  `Report step X/N at day Y/Z` output, plus **Open folder** and **View PRT**
  for the selected job.
- **Results tab** (when built with summary support): plot summary vectors
  (FOPR, WBHP, ...) straight from a run's `SMSPEC`/`UNSMRY` via opm-common's
  `EclIO::ESmry`. The **vector picker is grouped and filtered** — using
  opm-common's own `SummaryNode` classification it offers **Category**
  (Field / Well / Group / Region / Block / ... — only those present),
  **Type** (Rate / Total / Ratio / Pressure / ...), an **Item** dropdown
  (the wells / groups / region numbers that exist; block and connection cells
  are shown as grid `I,J,K` indices) and a text search, then a
  tree grouped by quantity with human-readable names (WOPR → "Oil Production
  Rate", ~130 mnemonics). Multi-select plots several curves, with a second
  Y axis when units differ (e.g. rate vs. pressure); 10 s auto-refresh
  updates the plot while a simulation is still writing. Drag to zoom
  (Reset zoom button to restore), optional calendar-date X axis, and
  Save PNG for reports. Any external `SMSPEC` can be opened too.
- **Simulator** — always the `flow`(`.exe`) shipped next to the GUI (in a
  development checkout it falls back to the harness build tree); the resolved
  path is shown in the log at startup. `flow` contains every model variant,
  so no picker is needed.
- **Queue control** — *Stop queue* kills the running job and aborts the rest;
  *Skip job* kills the running job and continues with the next; *Validate
  deck* parse-and-initializes the selected deck (`flow --enable-dry-run`)
  without running the simulation. The queue itself is remembered between
  sessions.
- **Case manager & comparison** — loaded cases appear in a checkable list:
  **checked cases are plotted together** (legend shows `case | vector`), the
  highlighted case drives the vector tree, and *Remove* drops a case from
  the list. Same-named cases from different runs are disambiguated with the
  run directory (full path in the tooltip). The *markers* toggle marks the
  actual data points on each curve. Finished runs can be opened by dropping
  an `.SMSPEC` on the window (or *Open SMSPEC...*), and adding a deck whose
  `<deck>_run` output already exists registers its case automatically.
- **Completion notification** — a system-tray toast when the queue finishes
  (clicking it opens the finished case in the Results tab), and an
  oversubscription note in the log when ranks × threads exceed the machine's
  logical cores.
- **PRT viewer** with free-text search and a *Next problem* button cycling
  through Error/Warning lines; the log pane batches appends (100 ms) and
  only follows the tail while you are at the bottom.
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
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 -m qtcharts --archives qtbase qtcharts -O C:\Qt

. .\setup-env.ps1
cmake -S flow-gui-qt -B build-gui-qt -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build-gui-qt
C:\Qt\6.8.3\msvc2022_64\bin\windeployqt --release --no-translations build-gui-qt\flow-gui-qt.exe
build-gui-qt\flow-gui-qt.exe
```
`windeployqt` copies the required Qt DLLs and the platform plugin next to
the executable.

**Summary plotting** is enabled automatically when the harness'
`install-mpi` (opm-common with `EclIO::ESmry`) and Qt Charts are found at
configure time — watch for `flow-gui-qt summary plotting: ON`. Build from a
`setup-env.ps1` shell (the opm-common linkage needs the MSVC/MS-MPI env);
the needed runtime DLLs (`fmt.dll`, `libomp140.x86_64.dll`) are copied next
to the exe automatically. On Linux point `-DFLOWGUI_OPM_PREFIX` at an
opm-common install prefix and install `qt6-charts-dev` (or equivalent).

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
