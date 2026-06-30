# Clean-room validation walkthrough

Goal: prove the shareable bundle builds OPM Flow (serial + MPI) from scratch on a
fresh tree, exactly as a recipient would experience it.

Prereqs assumed already present (machine-level, can't be "clean"): VS 2022 C++
toolset + Windows SDK + CMake + Ninja, git, and MS-MPI runtime+SDK.

Steps (each validated below):
1. Create an empty directory and unzip `opm-flow-windows-mpi-build.zip` into it.
2. `./build-all.ps1 -Mpi -OpmOrg gitPaean -OpmBranch windows`
   - generates compat shims
   - clones vcpkg, DUNE (v2.10.0), OPM (fork/branch), Trilinos
   - applies the DUNE patches from patches/
   - vcpkg installs deps (restored from binary cache if warm)
   - builds Zoltan, then DUNE + opm-common/grid/simulators with MPI
3. Run serial deck (SPE1) — confirm exit 0 + outputs.
4. `mpiexec -n 2 ...` — confirm parallel run with Zoltan partitioning.

Results are appended at the bottom as each step completes.

---

## Results (clean run in C:\Users\paean\Desktop\opm_clean)

- **Step 1 (unzip):** bundle extracted to a fresh dir — 9 files, nothing pre-built. OK.
- **Step 2 (build-all -Mpi -OpmOrg gitPaean -OpmBranch windows):**
  - compat shims generated (getopt/unistd/sys/* /FCMacros) — OK
  - cloned vcpkg, DUNE (v2.10.0), OPM (gitPaean/windows fork), [Trilinos next] — OK
  - **DUNE patches auto-applied to fresh clone — verified:** gmshreader.hh has
    `_ftelli64`; mpitraits.hh has the `#ifdef MPI_CXX_DOUBLE_COMPLEX` guard;
    DuneCommonMacros.cmake has `find_package(MPI COMPONENTS C)` (3.0 gate gone). OK
  - deps restored from vcpkg binary cache — OK
  - built (MPI): DUNE x4, Zoltan (via Trilinos), opm-common, opm-grid — all OK
  - **FINDING:** opm-simulators failed at `ReservoirCouplingSpawnSlaves.cpp:188`
    (path::c_str()=wchar_t* vs char*). Cause: the fork `gitPaean/windows` is
    **missing the pushed opm-simulators "MPI build with MS-MPI" commit** — it is
    committed locally but is 1 commit ahead of origin/windows (not pushed).
    opm-common and opm-grid MPI commits ARE pushed; only opm-simulators is behind.
    **ACTION FOR USER:** `git -C opm-simulators push origin windows`.
  - Applied that one commit's diff to the clean tree and resumed.
  - **Resumed build: SUCCESS** — `build-mpi\opm-simulators\bin\flow_blackoil.exe` (≈22.9 MB).
- **Step 3 (serial / 1 rank):** `flow_blackoil SPE1.DATA` → exit 0, 123 timesteps.
- **Step 4 (parallel / 2 ranks):** `mpiexec -n 2 flow_blackoil SPE1.DATA` →
  `Using 2 MPI processes`, `ZOLTAN Load balancing method = 9 (GRAPH)`,
  123 timesteps, exit 0, UNRST written. PASS.

## Verdict (first run)
The bundle + guide build and run OPM Flow (serial + parallel) from a clean tree.
The ONE blocker for a from-fork build was unpushed: the opm-simulators
"MPI build with MS-MPI" commit. (Now pushed — see re-validation below.)

---

## Re-validation — fresh tree, complete fork, ZERO manual steps (opm_clean2)

After the opm-simulators commit was pushed to gitPaean/windows, re-ran the whole
thing in a brand-new directory from the bundle, with no manual patching:

    Expand-Archive opm-flow-windows-mpi-build.zip -DestinationPath opm_clean2
    .\build-all.ps1 -Mpi -OpmOrg gitPaean -OpmBranch windows

- DUNE patches auto-applied (log: "dune-common: applied dune-common-windows.patch",
  "dune-grid: applied dune-grid-windows.patch"). OK
- Clone (incl. complete fork) + Zoltan + all modules (MPI) built. SUCCESS:
  build-mpi\opm-simulators\bin\flow_blackoil.exe (~22.9 MB).
- `mpiexec -n 2 flow_blackoil SPE1.DATA`: "Using 2 MPI processes",
  "ZOLTAN Load balancing method = 9 (GRAPH)", 123 timesteps, exit 0, UNRST written.

**VERDICT: PASS with zero manual steps.** A recipient needs only:
  1. install toolchain + MS-MPI (one-time),
  2. unzip the bundle,
  3. `.\build-all.ps1 -Mpi -OpmOrg gitPaean -OpmBranch windows`.
(Drop -OpmOrg/-OpmBranch once the fixes are merged into OPM/*.)
