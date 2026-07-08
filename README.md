# opm_flow_windows

Build [OPM Flow](https://opm-project.org) as a **pure native Windows binary**
(MSVC PE — no WSL, Cygwin, or MSYS), both **serial** and **parallel (MPI + Zoltan)**.

This repo is just the **build harness**: scripts, the detailed guide, the small
Windows compatibility shims, the DUNE patches, and a CI workflow. It does **not**
contain the OPM / DUNE / Trilinos sources — `build-all.ps1` clones those into
`src/`, `deps/`, and `vcpkg/` (all git-ignored) when you run it.

> Status: validated end-to-end from a clean checkout — serial and 2-rank MPI runs
> of SPE1 pass (see [VALIDATION.md](VALIDATION.md)).

## Prerequisites
- Windows 10/11 x64; `git` and `winget` on PATH; ~15 GB free disk; internet.
- **Toolchain (one-time)** — VS 2022 C++ build tools + Windows SDK + CMake + Ninja
  (approve the UAC prompt):
  ```powershell
  winget install --id Microsoft.VisualStudio.2022.BuildTools -e `
    --accept-package-agreements --accept-source-agreements `
    --override "--quiet --wait --norestart `
      --add Microsoft.VisualStudio.Workload.VCTools `
      --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
      --add Microsoft.VisualStudio.Component.Windows11SDK.22621 `
      --add Microsoft.VisualStudio.Component.VC.CMake.Project `
      --includeRecommended"
  ```
- **For MPI builds only** — Microsoft MPI runtime + SDK:
  ```powershell
  winget install --id Microsoft.msmpi -e
  winget install --id Microsoft.msmpisdk -e
  ```

## Quick start (serial)
From a clone of this repo:
```powershell
.\build-all.ps1
```
Clones vcpkg + DUNE (v2.10.0) + OPM, applies the DUNE patches, installs deps, and
builds in order. Result: `build\opm-simulators\bin\flow_blackoil.exe`. Run it:
```powershell
.\build\opm-simulators\bin\flow_blackoil.exe <deck>.DATA --output-dir=<dir>
```

## Quick start (parallel / MPI)
```powershell
.\build-all.ps1 -Mpi
mpiexec -n 4 .\build-mpi\opm-simulators\bin\flow_blackoil.exe <deck>.DATA --output-dir=<dir>
```
Builds Zoltan (from a Trilinos clone) + the MPI modules into separate
`build-mpi\` / `install-mpi\` trees (the serial build is untouched). See
[BUILD_WINDOWS.md](BUILD_WINDOWS.md) §13.

## Until the OPM fixes are merged upstream
The Windows/MSVC fixes to opm-common/opm-grid/opm-simulators (and opm-upscaling,
with `-Upscaling`) must be present in the sources. Until they land in `OPM/*`,
point the build at the fork/branch that has them (the DUNE patches in `patches/`
are applied automatically regardless):
```powershell
.\build-all.ps1 -Mpi -OpmOrg GitPaean -OpmBranch windows
```
Once merged, drop `-OpmOrg/-OpmBranch`.

## Options
```powershell
.\build-all.ps1 -Mpi                 # parallel build (MPI + Zoltan)
.\build-all.ps1 -OpenMP              # OpenMP threading (/openmp:llvm)
.\build-all.ps1 -Mpi -OpenMP         # hybrid MPI + OpenMP
.\build-all.ps1 -SimTarget all       # build every flow_* variant (full suite), not just flow_blackoil
.\build-all.ps1 -Upscaling           # also clone + build opm-upscaling (upscale_* / cpchop tools)
.\build-all.ps1 -SkipClone           # sources already cloned
.\build-all.ps1 -SkipDeps            # vcpkg packages already installed
.\build-all.ps1 -Jobs 6              # parallel compile jobs per module (default 4; raise on a high-RAM machine)
.\build-all.ps1 -DuneVersion v2.10.0
.\build-all.ps1 -OpmOrg <user> -OpmBranch <branch>
```
`-Mpi` and `-OpenMP` are independent and compose. For hybrid runs use
`mpiexec -n <ranks> flow_blackoil <deck> --threads-per-process=<threads>`.

## What's in this repo
| Path | Purpose |
|------|---------|
| `build-all.ps1` | One-shot driver — the entry point (serial or `-Mpi`) |
| `build-module.ps1` | Configure/build/install one module with the MSVC flags |
| `setup-env.ps1` | Loads MSVC (vcvars64) + vcpkg (+ MS-MPI) into the shell |
| `compat/include/` | POSIX/Fortran shim headers MSVC lacks (getopt, unistd, sys/*, FCMacros) |
| `patches/` | DUNE Windows patches, auto-applied after the DUNE checkout |
| `flow-gui/` | Cross-platform (Windows/Linux) GUI front end for running flow — deck queue, MPI/OpenMP options, live log (see its README) |
| `flow-gui-qt/` | Qt 6 version of the GUI (Windows/Linux/macOS) — same features plus persistent settings; the base for further extension (see its README) |
| `package-flow.ps1` | Stage a redistributable package (bin + runtimes + prerequisites) and zip it |
| `installer/` | Inno Setup script producing `OPM-Flow-<ver>-Setup.exe` (see [PACKAGING.md](PACKAGING.md)) |
| `packaging/` | MSIX build script for sideloading / Microsoft Store (see [PACKAGING.md](PACKAGING.md)) |
| `PACKAGING.md` | Distribution guide: portable zip, installer, MSIX/Store, licensing notes |
| `release-notes/` | Per-release notes shown on the GitHub Releases page (e.g. `v2026.10-pre.md`); published with `gh release edit --notes-file` (see [PACKAGING.md](PACKAGING.md)) |
| `ci/windows.yml` | GitHub Actions workflow |
| `BUILD_WINDOWS.md` | Full step-by-step guide (§1–14): toolchain, deps, flags, MPI, patches, CI |
| `PATCHES.md` | Record of every source-level Windows/MSVC fix |
| `VALIDATION.md` | Clean-room validation walkthrough + results |

Ignored (cloned sources or generated build/packaging output): `src/`, `deps/`,
`vcpkg/`, `build*/`, `install*/`, `dist/` (packaging output — staged trees, the
downloaded VC++/MS-MPI runtime installers, and the built `Setup.exe`/`.zip`/`.msix`),
and `*.log`.

## Installing a released build (Windows SmartScreen)
The published `Setup.exe` / `.zip` are **not code-signed**, so Microsoft
SmartScreen shows a *reputation* warning (not a virus detection) — and it is
dismissible:
- **Downloading (Edge/Chrome):** if it says *"isn't commonly downloaded"*, open
  the download's **⋯** menu → **Keep**, then **Keep anyway**.
- **First launch:** if *"Windows protected your PC"* appears, click
  **More info → Run anyway**.

The `.exe` installer only *warns*; the **MSIX** package, by contrast, is
*blocked* unless its signing certificate is already trusted on the target
machine — so prefer the `Setup.exe` (or the portable zip) for general use.
Code-signing the installer with an OV/EV certificate removes the warnings
altogether (see [PACKAGING.md](PACKAGING.md)).

## License
This repository (build scripts, documentation, compatibility shims, and the
`flow-gui` / `flow-gui-qt` applications) is licensed under the
**GNU General Public License, version 3 or later** — see [LICENSE](LICENSE) —
matching the [OPM project](https://opm-project.org) it builds. The files in
`patches/` modify DUNE sources and are therefore available under the licenses
of the respective DUNE modules (GPL-2 with runtime exception).

## Notes
- Target `flow_blackoil`, not full `flow` (far less RAM/link time).
- The produced `.exe` is native Win64 (PE32+); for distribution it needs the VC++
  redistributable and, for MPI binaries, the MS-MPI runtime. See BUILD_WINDOWS.md §11.
- OpenMP is off by default; enable it with `-OpenMP` (uses MSVC `/openmp:llvm`,
  independent of `-Mpi`). Threaded runs need `libomp140.x86_64.dll` next to the
  `.exe`; see BUILD_WINDOWS.md §11 / §13.
