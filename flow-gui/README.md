# flow-gui

A small cross-platform (Windows / Linux) GUI front end for running
[OPM Flow](https://opm-project.org) simulations — inspired by the basic
functionality of [OPMRUN](https://github.com/OPM/opm-utilities/tree/master/opmrun),
but as a single native executable with no Python runtime required.

Queue up input decks, pick the simulator binary and MPI/OpenMP settings,
run the queue sequentially, and watch the live simulator log. A running job
(including all of its MPI ranks) can be stopped at any time.

## Features
- **Job queue** of `*.DATA` input decks (add / remove / clear, multi-select).
- **Simulator picker** — defaults to `flow`(`.exe`) found in a sibling
  `build-mpi\opm-simulators\bin` (this harness' layout), any flow variant works
  (`flow_blackoil`, `flow_gaswater`, ...).
- **Parallel runs** — MPI rank count (spawns `mpiexec -n N ...`) and
  `--threads-per-process` OpenMP threads; both default to 1 (serial).
- **Output directory policy** — per-deck `<deck>_run` next to the deck
  (default), or a custom directory used exactly as given (safe to share
  between decks: output files are named after each deck's stem).
- **Extra options** passed through to flow verbatim
  (e.g. `--linear-solver=ilu0`).
- **Live log** — merged stdout/stderr of the simulator streams into the
  window while it runs.
- **Stop job** — kills the entire process tree (on Windows via a Job Object
  so all MPI ranks die with mpiexec; on Linux via the process group).

## Building

The GUI toolkit (FLTK 1.4) is fetched and built from source automatically at
configure time — no vcpkg or system FLTK required (an installed FLTK is used
if CMake finds one). The result links statically into a single executable.

### Windows (this harness)
```powershell
. .\setup-env.ps1
cmake -S flow-gui -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-gui
build-gui\flow-gui.exe
```

### Linux
```bash
sudo apt install libx11-dev libxext-dev libxft-dev libxinerama-dev   # X11 headers
cmake -S flow-gui -B build-gui -DCMAKE_BUILD_TYPE=Release
cmake --build build-gui
./build-gui/flow-gui
```

## Usage
1. Check the **Simulator** path (auto-detected from the harness build tree).
2. **Add deck...** one or more `*.DATA` files to the queue.
3. Choose **MPI ranks** / **OMP threads** (1/1 = serial), output policy and
   any extra flow options.
4. **Run queue** — jobs run one after another; the log streams live.
5. **Stop job** kills the currently running job and aborts the remainder of
   the queue.

`flow-gui --version` prints the version and exits (useful as a headless
smoke test).

## Notes
- For MPI runs, `mpiexec` must be on `PATH` (on Windows it is after
  installing MS-MPI; run from a `setup-env.ps1` shell or add
  `C:\Program Files\Microsoft MPI\Bin` to `PATH`).
- On Windows, remember the firewall pre-authorization for freshly built
  simulators (`allow-firewall.ps1`, see the harness README).
