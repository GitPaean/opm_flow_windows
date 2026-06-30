# Building OPM Flow (`flow_blackoil`) natively on Windows with MSVC

This document describes how to build a **pure native Windows** `flow_blackoil.exe`
(MSVC PE32+ binary, no WSL / Cygwin / MSYS) from the OPM sources plus the
DUNE core modules.

It assumes the Windows/MSVC source fixes have been merged into (or are present in)
`opm-common`, `opm-grid`, and `opm-simulators`. Everything else — toolchain,
third-party dependencies, the small POSIX compatibility shims, the build flags,
and the build order — is captured here and automated by `build-all.ps1`.

---

## 1. Overview

```
opm_flow\                     <- OPM_ROOT
  src\
    dune-common\  dune-geometry\  dune-grid\  dune-istl\   (DUNE 2.10.0)
    opm-common\   opm-grid\       opm-simulators\          (OPM, with Win/MSVC fixes)
  vcpkg\                      <- dependency manager (Boost, fmt, SuiteSparse, BLAS/LAPACK)
  compat\include\            <- small POSIX shims supplied by THIS build harness
  build\                     <- out-of-source build trees (one per module)
  install\                   <- shared install prefix (DUNE + opm-common + opm-grid land here)
  setup-env.ps1              <- loads MSVC (vcvars64) + vcpkg into a shell
  build-module.ps1           <- configure/build/install one module with the right flags
  build-all.ps1              <- end-to-end driver (this is the "workflow")
```

**Build order** (each step's output is consumed by the next):

```
DUNE: dune-common -> dune-geometry -> dune-istl -> dune-grid
OPM:  opm-common  -> opm-grid      -> opm-simulators (target: flow_blackoil)
```

**Standards / config:** OPM needs **C++20**, DUNE needs C++17. We build **Release**,
**static** OPM libraries, with MPI/OpenMP/Fortran **disabled** (serial,
single-threaded). See §8 for enabling those later.

---

## 2. Prerequisites

- Windows 10 or 11, x64.
- `git` and `winget` on PATH (both ship with current Windows).
- ~15 GB free disk (toolchain + vcpkg buildtrees + build output).
- Internet access (downloads the toolchain, vcpkg packages, and DUNE/OPM sources).

---

## 3. Install the toolchain (MSVC + CMake + Ninja)

One `winget` command installs the VS 2022 Build Tools with the C++ workload,
the Windows SDK, and the bundled CMake + Ninja. **It triggers a UAC prompt — approve it.**

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

This provides:
- `cl.exe` / `link.exe` — MSVC 14.4x (VS 17.14), full C++20 support
- the Windows 11 SDK
- CMake (>= 3.31) and Ninja, under the VS install

> The full Visual Studio IDE works too; only the C++ x64 toolset + Windows SDK
> + CMake + Ninja are required. `setup-env.ps1` locates whichever install via `vswhere`.

---

## 4. Lay out the tree and fetch sources

```powershell
$Root = "C:\Users\<you>\Desktop\opm_flow"   # pick any path without spaces
New-Item -ItemType Directory -Force -Path $Root\src | Out-Null
Set-Location $Root

# vcpkg
git clone https://github.com/microsoft/vcpkg.git vcpkg

# DUNE core modules, pinned to the 2.10.0 release
foreach ($m in 'dune-common','dune-geometry','dune-grid','dune-istl') {
  git clone https://gitlab.dune-project.org/core/$m.git src\$m
  git -C src\$m checkout v2.10.0
}

# OPM modules (use your fork/branch until the Windows fixes are upstream)
foreach ($m in 'opm-common','opm-grid','opm-simulators') {
  git clone https://github.com/OPM/$m.git src\$m
}
```

> DUNE 2.10.0 satisfies OPM's `>= 2.9` requirement and is well tested. Avoid
> DUNE `master` (2.11-dev), which can drift ahead of what OPM master tracks.

---

## 5. Build the third-party dependencies with vcpkg

```powershell
Set-Location $Root\vcpkg
.\bootstrap-vcpkg.bat -disableMetrics

.\vcpkg.exe install --triplet x64-windows `
  suitesparse-umfpack fmt lapack `
  boost-test boost-date-time boost-property-tree boost-mpl `
  boost-range boost-spirit boost-filesystem boost-system
```

What this gives OPM:
- **`lapack`** → pulls **OpenBLAS** (provides both BLAS and LAPACK; no Fortran
  compiler needed) and makes `find_package(BLAS)`/`find_package(LAPACK)` work.
- **`suitesparse-umfpack`** → UMFPACK (+ its AMD/CHOLMOD deps). We deliberately
  use `suitesparse-umfpack`, **not** the full `suitesparse` metapackage, to skip
  GraphBLAS/SPEX (heavy, need GMP/MPFR, unused by OPM).
- **`fmt`**, and the **Boost** components OPM actually uses.

cJSON is **not** installed via vcpkg — opm-common downloads and builds it itself
at configure time (works offline only if previously cached).

> First run compiles OpenBLAS and SuiteSparse and can take 20–40 min. vcpkg
> caches results, so re-runs are fast.

---

## 6. The POSIX compatibility shims (`compat\include\`)

Windows/MSVC lacks a handful of POSIX headers/functions that OPM uses, and it
has no Fortran compiler to generate the BLAS name-mangling header. Rather than
patch every `#include`, we put small shim headers on the include path. These are
part of **this build harness** (not the OPM repos) and are generated by
`build-all.ps1`. The files are:

| File | Purpose |
|------|---------|
| `compat\include\unistd.h` | maps POSIX I/O to MSVC `<io.h>`/`<process.h>`; adds `STD*_FILENO`, `R_OK/W_OK/F_OK/X_OK`, `sleep`/`usleep` |
| `compat\include\sys\ioctl.h` | empty stub (no `TIOCGWINSZ`) so terminal-width code uses its default branch |
| `compat\include\sys\utsname.h` | `struct utsname` + `uname()` from `COMPUTERNAME`/`PROCESSOR_ARCHITECTURE` |
| `compat\include\getopt.h` | minimal `getopt()` for the optional CLI tools |
| `compat\include\FCMacros.h` | Fortran name-mangling (`FC_GLOBAL(name,NAME) = name##_`, the gfortran/OpenBLAS ABI) — normally generated by CMake's FortranCInterface |

`build-module.ps1` adds `-I compat\include` to **both** `CMAKE_CXX_FLAGS` and
`CMAKE_C_FLAGS` (the C flag matters: `opm-grid/.../trans_tpfa.c` needs `FCMacros.h`).

---

## 7. Compile flags and configure options

`build-module.ps1` applies these to every module:

**`CMAKE_CXX_FLAGS`:**
```
/permissive- /Zc:__cplusplus /Zc:preprocessor /bigobj /EHsc /wd4068
-D_USE_MATH_DEFINES /DWIN32 /D_WINDOWS /I <OPM_ROOT>\compat\include
```
- `/permissive-` — accept the alternative tokens `and`/`or`/`not` DUNE uses, and conform generally.
- `/Zc:__cplusplus` — report the real `__cplusplus` value (MSVC otherwise lies).
- `/Zc:preprocessor` — conformant preprocessor.
- `/bigobj` — OPM/DUNE template-heavy TUs exceed the default object-section limit.
- `/wd4068` — ignore unknown `#pragma GCC ...`.
- `-D_USE_MATH_DEFINES` — expose `M_PI` etc.
- `/DWIN32 /D_WINDOWS` — CMake's default Windows defines (must be restored because
  setting `CMAKE_CXX_FLAGS` overrides `CMAKE_CXX_FLAGS_INIT`).

**`CMAKE_C_FLAGS`:** `/wd4068 -D_USE_MATH_DEFINES /DWIN32 /D_WINDOWS /I <OPM_ROOT>\compat\include`

**Configure options (per module):**
```
-DCMAKE_BUILD_TYPE=Release
-DCMAKE_INSTALL_PREFIX=<OPM_ROOT>\install
-DCMAKE_PREFIX_PATH=<OPM_ROOT>\install
-DCMAKE_TOOLCHAIN_FILE=<OPM_ROOT>\vcpkg\scripts\buildsystems\vcpkg.cmake
-DVCPKG_TARGET_TRIPLET=x64-windows
-DBUILD_TESTING=OFF
-DBUILD_EXAMPLES=OFF          # skip optional CLI tools (path/getopt issues)
-DWITH_NATIVE=OFF             # drop -mtune=native (a GCC flag)
-DCMAKE_DISABLE_FIND_PACKAGE_MPI=TRUE
-DCMAKE_DISABLE_FIND_PACKAGE_OpenMP=TRUE   # MSVC OpenMP 2.0 rejects size_t loop counters
-G Ninja
```

---

## 8. Build, in order

With the env loaded (`. .\setup-env.ps1`), `build-module.ps1 <module> [-Target <t>]`
configures + builds + installs one module. The full sequence:

```powershell
. .\setup-env.ps1
.\build-module.ps1 dune-common
.\build-module.ps1 dune-geometry
.\build-module.ps1 dune-istl
.\build-module.ps1 dune-grid
.\build-module.ps1 opm-common          # installs opmcommon.lib + headers + cmake config
.\build-module.ps1 opm-grid            # installs opmgrid.lib
.\build-module.ps1 opm-simulators -Target flow_blackoil
```

The result: `build\opm-simulators\bin\flow_blackoil.exe` (plus the vcpkg DLLs
`fmt.dll`, `umfpack.dll` copied next to it).

> **Target `flow_blackoil`, not `flow`.** The full `flow` target links every
> simulator variant (black-oil, gas-water, CO2, energy, …) and needs far more
> RAM and link time. `flow_blackoil` is the black-oil-only simulator.

---

## 9. Run / verify

```powershell
.\build\opm-simulators\bin\flow_blackoil.exe <deck>.DATA --output-dir=<dir>
```

A clean black-oil deck (e.g. SPE1, with `WELLDIMS` consistent with its well count)
runs to completion and writes `EGRID / INIT / UNRST / SMSPEC / UNSMRY / PRT`.

Verify it is a native binary (no POSIX emulation):

```powershell
dumpbin /headers .\build\opm-simulators\bin\flow_blackoil.exe | findstr /C:"machine" /C:"magic"
dumpbin /dependents .\build\opm-simulators\bin\flow_blackoil.exe
```
Expect `PE32+`, `x64`, and dependencies only on `KERNEL32`/`USER32`, the MSVC
runtime (`MSVCP140*`, `VCRUNTIME140*`), the Universal CRT (`api-ms-win-crt-*`),
and `fmt.dll`/`umfpack.dll` — **no** `cygwin1.dll` or `msys-2.0.dll`.

---

## 10. One-shot automation

`build-all.ps1` runs §4–§8 unattended (it generates the compat shims, bootstraps
vcpkg, installs deps, checks out DUNE, and builds every module in order):

```powershell
# after §3 (toolchain) is installed:
.\build-all.ps1                 # full SERIAL build from scratch
.\build-all.ps1 -Mpi            # full PARALLEL (MPI) build (see §13)
.\build-all.ps1 -SkipClone      # sources already present
.\build-all.ps1 -SkipDeps       # vcpkg packages already installed
```

---

## 11. Distribution & next steps

- **Redistributable:** the `api-ms-win-crt-*` deps are the Universal CRT (ships
  with Windows 10/11). `MSVCP140/VCRUNTIME140` come from the VC++ Redistributable.
  For a self-contained `.exe`, build dependencies with the `x64-windows-static`
  triplet and add `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` to static-link the CRT.
- **OpenMP (parallel):** MSVC's default `/openmp` is OpenMP 2.0 and rejects
  unsigned/`size_t` loop counters (C3016). Re-enable with `/openmp:llvm` (OpenMP
  3.0+) instead of disabling, or patch the affected loops to signed indices.
- **MPI:** install MS-MPI (or Intel MPI) and drop `-DCMAKE_DISABLE_FIND_PACKAGE_MPI`.
- **Optional CLI tools** (`opmpack`, `compareECL`, …): currently off via
  `BUILD_EXAMPLES=OFF`; they have their own `path`→`string` / `getopt` fixes
  pending.

---

## 12. CI (GitHub Actions)

A full, ready-to-adapt workflow is provided in [`ci/windows.yml`](ci/windows.yml).
Intended location: `opm-simulators/.github/workflows/windows.yml`, with the build
harness (`setup-env.ps1`, `build-module.ps1`, `build-all.ps1`) committed under
`opm-simulators/windows/`.

Key points it implements:
- `runs-on: windows-2022` — the runner already has MSVC + CMake + Ninja, so §3 is skipped.
- Checks out `opm-simulators` (this repo) plus `opm-common` and `opm-grid` as
  siblings, and clones DUNE at `v2.10.0`, into a single layout root.
- Copies the harness scripts to that root so `build-all.ps1`'s `$Root` resolves
  to it, then runs `build-all.ps1 -SkipClone`.
- Uses vcpkg's **GitHub Actions binary cache** (`VCPKG_BINARY_SOURCES=clear;x-gha,readwrite`)
  so OpenBLAS/SuiteSparse/Boost are rebuilt only when versions change.
- Smoke-tests `flow_blackoil.exe --help` and uploads the exe (+ DLLs) as an artifact.

---

## 13. Parallel (MPI) build

This produces a **parallel** `flow_blackoil.exe` that runs across MPI ranks with
Zoltan domain decomposition — a true native Windows binary (links `msmpi.dll`).
It is built into **separate** `build-mpi\` / `install-mpi\` trees, so it does not
disturb the serial build.

### One command

```powershell
.\build-all.ps1 -Mpi
```

This: ensures MS-MPI is installed, clones Trilinos and builds the **Zoltan
package only** (against MS-MPI), then builds DUNE + opm-common/grid/simulators
with MPI enabled. Output: `build-mpi\opm-simulators\bin\flow_blackoil.exe`.

### Prerequisites (one-time)

Install **Microsoft MPI** — both the runtime and the SDK (UAC prompts):
```powershell
winget install --id Microsoft.msmpi -e        # runtime: msmpi.dll, mpiexec
winget install --id Microsoft.msmpisdk -e     # SDK: mpi.h, msmpi.lib
```
The SDK sets machine-level `MSMPI_INC` / `MSMPI_LIB64`; `setup-env.ps1` imports
them into the build shell (already-open shells won't see them otherwise).

### Why Zoltan, and how it's built

opm-grid couples MPI to **Zoltan** (its parallel-partitioning code is under
`#if HAVE_MPI`). Zoltan isn't in vcpkg, so `build-all.ps1 -Mpi` builds the Zoltan
package from a Trilinos clone, against MS-MPI, into `install-mpi`:
```
-DTrilinos_ENABLE_ALL_PACKAGES=OFF -DTrilinos_ENABLE_Zoltan=ON
-DTrilinos_ENABLE_Fortran=OFF
-DTPL_ENABLE_MPI=ON -DMPI_USE_COMPILER_WRAPPERS=OFF
-DTPL_MPI_INCLUDE_DIRS="$MSMPI_INC" -DTPL_MPI_LIBRARIES="$MSMPI_LIB64\msmpi.lib"
-DTPL_ENABLE_DLlib=OFF -DTPL_ENABLE_Pthread=OFF        # no libdl/pthread on Windows
```
Zoltan's built-in PHG/GRAPH partitioner (what OPM uses) needs no external
partitioner TPLs (no ParMETIS/Scotch). Its C code compiles unmodified with MSVC.

### Manual build (equivalent to -Mpi)

```powershell
. .\setup-env.ps1
.\build-module.ps1 dune-common   -Mpi
.\build-module.ps1 dune-geometry -Mpi
.\build-module.ps1 dune-istl     -Mpi
.\build-module.ps1 dune-grid     -Mpi
#  ... build Zoltan into install-mpi (see args above) ...
.\build-module.ps1 opm-common    -Mpi
.\build-module.ps1 opm-grid      -Mpi
.\build-module.ps1 opm-simulators -Mpi -Target flow_blackoil
```
`-Mpi` selects the `build-mpi\` / `install-mpi\` trees and enables `USE_MPI=ON`.

### Run in parallel

```powershell
mpiexec -n 2 .\build-mpi\opm-simulators\bin\flow_blackoil.exe <deck>.DATA --output-dir=<dir>
```
Expect `Using 2 MPI processes`, `ZOLTAN Load balancing method = ... (GRAPH)`, and
a per-rank owned/overlap cell breakdown. `mpiexec` comes from the MS-MPI runtime
(`setup-env.ps1` adds it to PATH).

### Notes / limits

- MS-MPI is MPI-2-level (CMake reports v2.0). DUNE's `MPI 3.0` requirement and its
  `MPI_CXX_*_COMPLEX` traits were patched out; black-oil doesn't need them.
- Distribution of an MPI binary also needs the MS-MPI **runtime** (`msmpi.dll`,
  in `System32`) on the target machine.
- OpenMP is still off; MPI and OpenMP are independent. For hybrid MPI+OpenMP,
  also enable OpenMP via `/openmp:llvm` (see §11).

---

## 14. Patches to dependencies (outside the OPM modules)

The OPM source fixes live in the opm-common/opm-grid/opm-simulators commits.
Beyond those, the **only dependency source patches are in DUNE** — Trilinos/Zoltan
and every vcpkg package build unmodified. The DUNE patches are shipped as
`patches\*.patch` and applied automatically by `build-all.ps1` right after the
DUNE checkout (idempotently).

| Package | File | Why | Needed for |
|---------|------|-----|------------|
| dune-grid | `dune/grid/io/file/gmshreader.hh` | POSIX `ftello`/`fseeko` → `_ftelli64`/`_fseeki64` | **serial + MPI** |
| dune-common | `cmake/modules/DuneCommonMacros.cmake` | drop `find_package(MPI 3.0)` gate (CMake sees MS-MPI as v2.0) | MPI only |
| dune-common | `dune/common/parallel/mpitraits.hh` | guard `MPI_CXX_*_COMPLEX` (MPI-3 types absent in MS-MPI) | MPI only |

dune-geometry and dune-istl need **no** patches.

**Zoltan / Trilinos:** zero source patches — only CMake configure flags
(Zoltan-only, MS-MPI without wrappers, `-DTPL_ENABLE_DLlib=OFF -DTPL_ENABLE_Pthread=OFF`); see §13.

**vcpkg packages** (Boost, fmt, SuiteSparse, OpenBLAS, LAPACK, MS-MPI): zero patches — used as built by vcpkg / the installer.

**Not patches, but supplied substitute headers** in `compat\include\` (generated by
`build-all.ps1`): `getopt.h`, `unistd.h`, `sys/ioctl.h`, `sys/utsname.h` (POSIX
shims) and `FCMacros.h` (Fortran name-mangling normally generated by
FortranCInterface). These stand in for headers MSVC/Windows lacks — needed for
the **serial** build already.

> To refresh a DUNE patch after editing the source:
> `git -C src\dune-common diff > patches\dune-common-windows.patch` (write it as
> ASCII/UTF-8, not PowerShell's default UTF-16 — e.g. via `cmd /c "... > ..."`).
