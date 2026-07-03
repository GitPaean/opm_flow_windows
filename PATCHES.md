# Windows / MSVC port patches

Patches applied to the source trees to build OPM-flow natively on Windows with
MSVC. Kept here so they can be upstreamed as PRs.

## Global build flags (not a source patch)
Configured via `build-module.ps1` `CMAKE_CXX_FLAGS`:
`/permissive- /Zc:__cplusplus /Zc:preprocessor /bigobj /EHsc /wd4068 -D_USE_MATH_DEFINES`
- `/permissive-` — enable alternative tokens (`and`/`or`/`not`) DUNE/OPM use as keywords.
- `/Zc:__cplusplus` — report the real `__cplusplus` value.
- `/Zc:preprocessor` — conformant preprocessor.
- `/bigobj` — template-heavy TUs exceed the default object section cap.
- `-D_USE_MATH_DEFINES` — expose `M_PI` etc. from `<cmath>`.

## dune-grid (v2.10.0)
- `dune/grid/io/file/gmshreader.hh`: MSVC lacks POSIX `ftello`/`fseeko`/`off_t`.
  Added an `#if defined(_MSC_VER)` shim mapping them to `_ftelli64`/`_fseeki64`
  (off_t is already provided by MSVC `<sys/types.h>`, so not redefined).

## Build flags note
- Setting `CMAKE_CXX_FLAGS` overrides `CMAKE_CXX_FLAGS_INIT`, dropping CMake's
  default `/DWIN32 /D_WINDOWS`. Restored them in `build-module.ps1` because some
  vendored code (ResInsight LibCore `cvfAtomicCounter.h`) checks `#ifdef WIN32`.

## opm-common (master)
- `cmake/Modules/DownloadCjson.cmake`: `create_symlink` for the downloaded cJSON
  sources fails on Windows without admin/Developer Mode. Use `copy_directory` on
  `WIN32` instead so `<cjson/cJSON.h>` resolves.
- `opm/input/eclipse/Generator/KeywordGenerator.cpp`: `write_file()` took
  `const std::string& file` but callers pass `std::filesystem::path`. path→string
  is implicit only on POSIX (on Windows path→wstring), so changed the parameter
  to `const std::filesystem::path&` and use `.string()` internally.
- `opm/input/eclipse/Deck/FileDeck.cpp:181`: `input_directory` (std::string) was
  initialised directly from a `fs::path` result; added `.string()`.
- `opm/input/eclipse/EclipseState/Grid/Fault.cpp`: added `#include <algorithm>`
  for `std::ranges::equal` (MSVC stdlib needs the explicit include).
- `opm/input/eclipse/Schedule/Well/NameOrder.cpp` and `PAvgCalculator.cpp`:
  added `#include <iterator>` for `std::back_inserter`.
- `opm/material/fluidmatrixinteractions/PiecewiseLinearTwoPhaseMaterialParams.hpp`:
  forward-declaration of the class was at global scope while the class lives in
  `namespace Opm`; under MSVC `/permissive-` the namespace-qualified friend
  `Opm::gpuistl::make_view` couldn't match. Moved the forward declaration into
  `namespace Opm` and dropped its duplicate default template argument.
- `CMakeLists.txt`: guard the unconditional `set_source_files_properties(... -Wno-shadow)`
  on Python.cpp with `if(NOT MSVC)` (MSVC rejects `/Wno-shadow` as `D8021`).

## opm-common (master) — building library only (BUILD_EXAMPLES=OFF)
The optional CLI tools (examples/ and test_util/ via the additionals satellite)
have many `std::filesystem::path` -> `std::string` mismatches on Windows and need
POSIX `getopt.h`. We build them OFF via `-DBUILD_EXAMPLES=OFF`, which required
fixing latent CMake assumptions that the tool targets always exist:
- `CMakeLists.txt`: guard `target_sources(compareECL ...)` with `if(TARGET compareECL)`.
- `CMakeLists.txt`: guard `list(APPEND opm-common_EXTRA_TARGETS compareECL rst_deck)`
  with `if(TARGET compareECL AND TARGET rst_deck)`.
- `cmake/Modules/OpmLibMain.cmake`: gate the `additionals` satellite on `BUILD_EXAMPLES`.
A Windows `getopt.h` shim exists at `compat/include/getopt.h` (added to the include
path via build-module.ps1) for when the tools are eventually built.
`examples/wellgraph.cpp`: `.stem()` -> `.stem().string()`.

## opm-grid (master)
- `CMakeLists.txt`: `project(opm-grid C CXX Fortran)` -> `project(opm-grid C CXX)`
  and gate the FortranCInterface `language_hook` on `CMAKE_Fortran_COMPILER`
  (MSVC has no Fortran compiler; FCMacros.h is provided via compat/, see below).
- `opm/grid/cpgrid/Geometry.hpp:1036`: `typename` added to dependent type
  `Geometry<3,cdim>::ctype`.
- `opm/grid/cpgrid/Geometry.hpp:641`: inside the `Geometry<3,cdim>` spec, the
  hard-coded self type `cpgrid::Geometry<3,3>` -> dependent `cpgrid::Geometry<3,cdim>`.
- `opm/grid/cpgrid/DefaultGeometryPolicy.hpp`: **key fix** - forward-declare the
  partial specializations `Geometry<0,dim>`/`Geometry<2,dim>`/`Geometry<3,dim>`
  next to the primary. This header instantiates `Geometry<3,3>` before Geometry.hpp
  defines the specializations; without the forward declarations MSVC binds
  `Geometry<3,3>` to the empty *primary* template and caches it, after which the
  partial specialization is never selected (symptom: "volume/center/ctype is not a
  member of Dune::cpgrid::Geometry<3,3>", note points at the primary). GCC defers
  the instantiation so it is unaffected.
- `FCMacros.h`: opm-common `blas_lapack.h` needs Fortran name-mangling macros
  (only `FC_GLOBAL`). Provided hand-written at `compat/include/FCMacros.h`
  (gfortran/OpenBLAS ABI: lowercase + trailing underscore). C files need it too,
  so build-module.ps1 adds the compat include to BOTH CMAKE_CXX_FLAGS and CMAKE_C_FLAGS.
Result: `opmgrid.lib` + cmake config installed.

## opm-simulators (master) — configure stage
- vcpkg: installed `lapack` (in addition to openblas) so `find_package(LAPACK)`
  succeeds (opm-simulators requires BLAS+LAPACK).
- opm-common `cmake/Modules/FindSuiteSparse.cmake`: `MATH_LIBRARY` (`find_library(m)`)
  is NOTFOUND on MSVC (no separate libm) and was added unconditionally to the
  UMFPACK try_compile link line, failing configure. Only append it `if(MATH_LIBRARY)`.
- opm-common `cmake/Modules/OpmLibMain.cmake`: only call `${project}_tests_hook`
  when `BUILD_TESTING` is ON **on Windows** (`AND (BUILD_TESTING OR NOT WIN32)`,
  so other platforms keep the historical always-call behaviour). With testing
  off, opm-simulators' modelTests.cmake hook still registered tests that
  `add_dependencies(test-suite <example-target>)` on example targets that were
  never created -> configure error.
  (opm-simulators reads opm-common's CMake modules from the source tree.)

## opm-simulators (master) — compile stage (flow_blackoil)
POSIX compatibility shims added under `compat/include/` (on the include path):
- `unistd.h` — wraps MSVC `<io.h>`/`<process.h>` (isatty/read/write/close/dup2/...),
  adds STDIN/OUT/ERR_FILENO and sleep()/usleep() (via Win32 Sleep). Covers the many
  files including opm-models `newtonmethod.hh`/`start.hh`/`simulatorutils.cpp`/etc.
- `sys/ioctl.h` — intentionally empty (no TIOCGWINSZ) so terminal-width code falls
  back to its default branch.
- `sys/utsname.h` — struct utsname + uname() filled from COMPUTERNAME / PROCESSOR_ARCHITECTURE.

Source fixes:
- opm-simulators `opm/models/utils/alignedallocator.hh`: `posix_memalign`/`free`
  -> `_aligned_malloc`/`_aligned_free` (+`<malloc.h>`) under `_MSC_VER`.
- opm-grid `opm/grid/utility/createThreadIterators.hpp`: `std::max(.., 1ul)` ->
  `std::max(.., std::size_t{1})` (LLP64: `unsigned long` is 32-bit on Windows, so
  the two std::max args had different types).
More compat shims: `compat/include/sys/stat.h` not needed (S_ISDIR defined inline).
Additional source fixes:
- Missing includes: `<numeric>` in RegionPhasePVAverage.cpp, GasLiftStage2.cpp,
  WellInterfaceFluidSystem.cpp; `<stdexcept>` in KeywordValidation.hpp.
- `simulatorutils.cpp`: define `S_ISDIR` for MSVC (`(mode & _S_IFMT) == _S_IFDIR`).
- `terminal.cpp`: guard `{SIGKILL,...}` map entry with `#ifdef SIGKILL`.
- `Banners.cpp`: replace `sysconf(_SC_PHYS_PAGES)` with `GlobalMemoryStatusEx`
  on `_WIN32` (include <windows.h> with WIN32_LEAN_AND_MEAN+NOMINMAX).
- `NonlinearSystemBlackOilReservoir.hpp`: renamed the `enum class DebugFlags`
  enumerators `STRICT/RELAXED/TUNINGDP` -> `Strict/Relaxed/TuningDP`
  (windows.h leaks `#define STRICT 1` -> "1 = 0"; renaming sidesteps the macro
  without any `#undef`).
- path->string: `SimulatorSerializer.cpp` (loadFile_ = path.string()),
  `ParallelFileMerger.cpp` (.native() -> .string(), native() is wstring on Win).
- `ConvergenceOutputConfiguration.cpp`: construct vector<string> from the
  cregex_token_iterator range explicitly (braced {first,last} was read as an
  initializer_list).
- `SatfuncConsistencyChecks.hpp/.cpp`: moved nested `ViolationSample` out to
  `Detail::SatfuncConsistencyViolationSample<Scalar>` (MSVC C2079: nested type
  used as std::array element of the same class template treated as incomplete).
- `GenericOutputBlackoilModule.cpp`: moved local `enum class EntryPhaseType` out
  of `doAllocBuffers` to namespace scope (MSVC treated the function-local enum as
  a distinct type per template instantiation, breaking the default member init).

Batch 3/4 (down to last files):
- `compat/include/unistd.h`: add POSIX access() mode bits F_OK/R_OK/W_OK/X_OK.
- `terminal.cpp`: guard `SIGHUP`/`SIGPIPE` signal() calls with `#ifdef` (absent on Windows).
- `Banners.cpp`: `getlogin()` -> `getenv("USERNAME")` on `_WIN32`.
- `SatfuncConsistencyChecks.hpp`: `ViolationCollection` stays a `std::array`
  (with an explicit `<array>` include); moving the element type out to
  `Detail::` (previous item) is sufficient for MSVC.
- `vtkmultiwriter.hh`: `fullPath.filename()` -> `.filename().string()`.
- `ConvergenceOutputConfiguration.cpp`: pass `options.data()`/`data()+size()`
  (const char*) to cregex_token_iterator — MSVC's string_view::begin() is a
  checked-iterator class, not const char*.

## opm-simulators (master) — runtime fix (parameter names)
flow_blackoil.exe built & linked but aborted at runtime: every CLI parameter
name was mangled (e.g. `--ecl-deck-file-name` shown as `-eters::-ecl-deck-file-name`),
so the deck and all options were rejected as "unknown parameters".
- Root cause: `opm/models/utils/parametersystem.hpp` `getParamName()` did
  `paramName.replace(0, strlen("Opm::Parameters::"), "")`, assuming the
  Dune::className string *starts* with "Opm::Parameters::". On MSVC the string is
  "struct Opm::Parameters::Xxx" (leading "struct "/"class "), so a fixed 17-char
  erase from position 0 cut "struct Opm::Para", leaving "eters::Xxx".
- Fix: `find("Opm::Parameters::")` and erase up to past it (handles the prefix).
  This corrects ALL parameter names on MSVC.

## MPI support — MS-MPI on Windows  (RESOLVED — see "Zoltan for Windows" + "MPI RESULT" below)
MS-MPI installed; DUNE (all 4) + opm-common build with MPI. opm-grid couples MPI
=> Zoltan, and Zoltan is not in vcpkg — resolved by building the Zoltan package
from a Trilinos clone against MS-MPI (see below), not by decoupling.

MPI builds use separate `build-mpi/` + `install-mpi/` trees (build-module.ps1 -Mpi).
Patches made so far:
- dune-common `cmake/modules/DuneCommonMacros.cmake`: `find_package(MPI 3.0 ...)`
  -> `find_package(MPI ...)`. CMake detects MS-MPI as v2.0, so the 3.0 gate
  rejected it. (MS-MPI implements most MPI-3 features but is labelled 2.0.)
- dune-common `dune/common/parallel/mpitraits.hh`: guard the
  `MPI_CXX_{DOUBLE,LONG_DOUBLE,FLOAT}_COMPLEX` traits with
  `#ifdef MPI_CXX_DOUBLE_COMPLEX` (these MPI-3 datatypes are absent in MS-MPI).
- opm-common `cmake/Modules/UseMPI.cmake`: new option `OPM_LINK_MPI_DIRECTLY`
  (default ON on Windows, OFF elsewhere, overridable). When ON and the target
  does not already get MPI transitively, `mpi_checks()` runs
  `find_package(MPI COMPONENTS C)` and links `MPI::MPI_C` directly. Needed
  because DUNE does not export MPI on its installed targets (MPI lives only in
  DUNE build-time ALL_PKG_FLAGS) and this build consumes installed modules
  rather than using dunecontrol. This makes mpi_checks() define HAVE_MPI=1 for
  every OPM module (parallel EclipseState etc.); no per-module CMake hack.

Why Zoltan is required: opm-grid `GraphOfGridWrappers.{hpp,cpp}`,
`ZoltanGraphFunctions`, etc. reference Zoltan types (ZOLTAN_ID_PTR, ZOLTAN_FATAL)
under `#if HAVE_MPI` with no `HAVE_ZOLTAN` guard, and opm-grid's CMake hard-requires
Zoltan when MPI is on (REQUIRE_ZOLTAN). Rather than decouple MPI from Zoltan
(guarding with HAVE_ZOLTAN + using (Par)METIS — a large source change), we build
Zoltan itself for Windows (next section).

## Zoltan for Windows (built via Trilinos, against MS-MPI)
Zoltan is required by opm-grid when MPI is on. It is not in vcpkg. Built the
Zoltan package only from a shallow Trilinos clone (deps/Trilinos), installed to
install-mpi (zoltan.h + zoltan.lib). Key configure args:
  -DTrilinos_ENABLE_ALL_PACKAGES=OFF -DTrilinos_ENABLE_Zoltan=ON
  -DTrilinos_ENABLE_Fortran=OFF -DZoltan_ENABLE_TESTS=OFF
  -DTPL_ENABLE_MPI=ON -DMPI_USE_COMPILER_WRAPPERS=OFF
  -DTPL_MPI_INCLUDE_DIRS="$MSMPI_INC" -DTPL_MPI_LIBRARIES="$MSMPI_LIB64\msmpi.lib"
  -DTPL_ENABLE_DLlib=OFF -DTPL_ENABLE_Pthread=OFF   (no libdl/pthread on Windows)
Zoltan's built-in PHG partitioner (what OPM uses) needs no external TPLs.
Zoltan's C code compiled cleanly with MSVC + MS-MPI (no source patches).
opm's FindZOLTAN finds it in install-mpi (accepts lib name "zoltan").

## opm-grid (MPI build) source fix
- `opm/grid/cpgrid/CpGridData.cpp` getIndex(T): MSVC's std::array<int,N> iterator
  is a wrapper class (not raw const int* as in libstdc++), so the template
  overload was chosen and `i->index()` failed. Added `if constexpr
  (is_integral pointee) return *i; else return i->index();`.

## opm-simulators (MPI build) source fix
- `opm/simulators/flow/rescoup/ReservoirCouplingSpawnSlaves.{cpp,hpp}`:
  getSlaveArgv_ took `const std::filesystem::path& data_file` and did
  `const_cast<char*>(data_file.c_str())`, but path::c_str() is `wchar_t*` on
  Windows while the argv array is `char*`. Changed the parameter to
  `const std::string&` and the caller passes `full_path.string()` (kept alive
  through MPI_Comm_spawn). Cross-platform.

## MPI RESULT
flow_blackoil.exe (MPI) at build-mpi/opm-simulators/bin/ runs in TRUE PARALLEL on
native Windows: `mpiexec -n 2 flow_blackoil SPE1.DATA` -> "Using 2 MPI processes",
"ZOLTAN Load balancing method = 9 (GRAPH)", grid split rank0=153 / rank1=147 owned
cells, 123 timesteps, exit 0, full ECLIPSE output. Links msmpi.dll; Zoltan static.

## Full build — all flow variants (serial, GPU off)
Building the default `all` target produced **all 39 production flow_* variants**
(incl. the `flow` omnibus). Only the experimental compositional solvers
(flowexperimental/comp/*) and a couple of examples (lens_*) failed. Fixes for those:

- dune-grid `dune/grid/yaspgrid.hh` (size() count loop): `std::array<...>::iterator`
  typedef -> `auto`. dataBegin()/dataEnd() return raw pointers, which are the
  array iterator on libstdc++ but a distinct wrapper class on MSVC.
- opm-common `opm/material/Constants.hpp`: `kb = R/Na` and
  `hRed = h/(2*std::numbers::pi_v<Scalar>)` fail when Scalar is the compositional
  autodiff type (its arithmetic isn't constexpr, and `pi_v<Scalar>` instantiates
  the ill-formed primary template). Compute both in `double` and cast to Scalar.

(Build with `-j 4` max — heavy template TUs are RAM-hungry; see build-module.ps1 -Jobs.)

## Configure options used for opm-common (MSVC)
`-DWITH_NATIVE=OFF` (drop `-mtune=native`). OpenMP is off by default
(`-DCMAKE_DISABLE_FIND_PACKAGE_OpenMP=TRUE`); enable it with `build-all.ps1 -OpenMP`
(see below). Result: `opmi.exe` parses SPE1CASE1.DATA end-to-end (exit 0).

## OpenMP support (build-all.ps1 -OpenMP)  (RESOLVED)
MSVC's default `/openmp` is OpenMP 2.0 and rejects OPM's unsigned/`size_t`
parallel-for counters (C3016); OPM's `ElementChunks` idiom also uses OpenMP-5.0
range-based-for after `#pragma omp parallel for`, which MSVC does not support at
all. Enabled via MSVC's **`/openmp:llvm`** (OpenMP 3.1+) plus source changes:
- opm-grid `ElementChunks.hpp`: added a random-access `operator[]`.
- opm-simulators (FIBlackoilModel, TracerModel, FacePropertiesTPSA_impl,
  Transmissibility_impl, getQuasiImpesWeights, tpsamodel) + opm-grid griditer
  example: converted the range-based `omp parallel for` loops to index-based
  (`for (size_t ci=0; ci<chunks.size(); ++ci) { auto chunk = chunks[ci]; ... }`).
- opm-simulators `DILU.hpp`: MSVC C1001 ICE on `row != A_.N()` under omp; hoist
  the bound and use `<`.
All index-based/`operator[]` forms are equivalent on GCC (Linux-safe).
`find_package(OpenMP)` is pre-seeded to `/openmp:llvm` + libomp in build-module.ps1.
Runtime dep: `libomp140.x86_64.dll` (ships with MSVC). Threads via
`--threads-per-process=N`; composes with MPI for hybrid runs. VERIFIED:
`flow_blackoil` runs serial-threaded and `mpiexec -n 2 ... --threads-per-process=3`
("Using 2 MPI processes with 3 OMP threads on each").

## opm-upscaling (master) — MSVC build (build-all.ps1 -Upscaling)
Builds the opmupscaling library plus the upscale_* / cpchop tools, which live in
examples/ (so this uses BUILD_EXAMPLES=ON). Depends only on opm-common + opm-grid
+ DUNE (not opm-simulators). Fixes:
- `CMakeLists.txt`: `project(opm-upscaling C CXX Fortran)` made host-conditional
  (`C CXX` on a Windows host) and the FortranCInterface language_hook gated on
  `CMAKE_Fortran_COMPILER`. Unlike opm-grid (whose FCMacros.h is never included),
  opm-upscaling's `blas_lapack.hpp` hard-requires `<FCMacros.h>`, so without a
  Fortran compiler the hook now writes a fallback FCMacros.h (lowercase +
  trailing underscore, the gfortran/OpenBLAS ABI) into the build dir — the
  module builds standalone, without relying on the harness compat shim. (The
  compat `FCMacros.h` is still needed for opm-common's `blas_lapack.h`.)
- `opm/porsol/mimetic/IncompFlowSolverHybrid.hpp`: `S_[0][0] *= 2` on a
  `Dune::FieldMatrix<double,1,1>` is an MSVC C2666 overload ambiguity (int vs the
  scalar/matrix operator*=); use a double literal `*= 2.0` (4 solver-variant sites).
- compat `unistd.h`: added a `gethostname()` shim (MSVC only provides it via
  winsock); upscale_elasticity.cpp uses it for a log banner.
Result: opmupscaling.lib + 24 tools build clean (serial / MPI / OpenMP / hybrid).
Optional deps cJSON / QuadMath / PTScotch / Boost-iostreams are not required — only
upscale_relperm_benchmark needs boost-iostreams and is skipped without it.

## dune-common PoolAllocator::max_size() — AMG/CPR crash at runtime (MSVC STL)
Symptom: any AMG-based linear solver (`amg`, `cpr`, `cprw` — flow's default)
aborts on the very first solve with
`Error rethrown as CriticalError at [...ISTLSolver.hpp:391]` /
`Original error: map/set too long`, serial and MPI alike (reproduced on Norne).
One-level solvers (`ilu0`, `dilu`) are unaffected. The "faulty linear solver
JSON" hint printed with it is a red herring (generic catch-site text).

Root cause: `dune/common/poolallocator.hh` had a placeholder
`max_size() const { return 1; }` (comment: "Not correctly implemented, yet!").
MSVC's `std::_Tree` (std::set/std::map) checks `size() == allocator max_size()`
on *every* insert and throws `length_error("map/set too long")` when equal —
so the second insert into any PoolAllocator-backed set throws. libstdc++ never
performs this per-insert check, which is why Linux builds are unaffected.
dune-istl's AMG aggregation (`paamg/aggregates.hh`) uses
`std::set<Vertex,std::less<Vertex>,PoolAllocator<Vertex,100>>`, so every AMG
hierarchy build died immediately.

Fix (in `patches/dune-common-windows.patch`, applies to serial + MPI):
`max_size()` now returns `std::numeric_limits<std::size_t>::max() / sizeof(T)`
— the standard meaning (largest request), matching repeated single-node
allocations by node-based containers. Upstream-relevant for any MSVC user of
dune-istl AMG. VERIFIED: Norne (`NORNE_ATW2013.DATA`) runs with the default
`cprw` solver, serial and `mpiexec -n 4 ... --threads-per-process=2`.

## Unit-test suite on Windows — findings from running full ctest (all 4 modules)
Prereq: sources must be checked out **byte-exact** — git-for-Windows' default
`core.autocrlf=true` injects `\r` into the formatted Eclipse test data
(e.g. `tests/ECLFILE.FINIT`), whose fixed-format readers then reject it
(EclIO / EclipseGridTests failures). `build-all.ps1` now clones everything
with `-c core.autocrlf=false`; existing clones can be repaired with
`git -C src\<mod> config core.autocrlf false` + re-checkout of `tests/`.

Source fixes committed on the `windows` branches:
- opm-common `Parser.cpp`: absolute-path test was `dataFileName[0] == '/'`
  (never true for `C:/...`), so absolute deck paths were relativized by the
  `proximate()` branch → cwd-relative IOConfig output dirs (EclipseStateTests).
  Now `std::filesystem::path::is_absolute()`.
- opm-common `TimeService.cpp`: `*std::gmtime(&t)` without a null check; the
  Windows CRT returns nullptr past year ~3000 (test_timer's deck reaches ~7014)
  → access violation. Falls back to an explicit civil-from-days conversion.
- opm-simulators `test_outputdir.cpp`: fixture removed its own cwd while
  OpmLog held files in it open → throw in noexcept dtor → 0xC0000409.
  chdir out + drop log backends + non-throwing remove_all.

Remaining, not code bugs:
- ~85 tests are shell-script driven (`run-vtu-test.sh`, `run-compareUpscaling.sh`)
  and cannot run under Windows ctest (needs a bash launcher upstream).
- Windows "Smart App Control" (if enforced) blocks freshly built unsigned test
  exes with BAD_COMMAND; it has no exclusion mechanism — disable it on build
  machines (Windows Security > App & browser control), or accept blocked tests.

## Known MS-MPI limitation: reservoir coupling (MPI_Comm_spawn)
MS-MPI does not implement MPI dynamic process management (`MPI_Comm_spawn`,
`MPI_Comm_connect`/`accept`, `MPI_Open_port`) and will not gain it. Coupled
reservoir runs (master decks with the `SLAVES` keyword) therefore cannot work
under MS-MPI: the code compiles and links (the symbols exist in msmpi.dll)
but spawning slaves fails at runtime with a clean error
(`ReservoirCouplingSpawnSlaves.cpp` logs the MPI error strings and throws
"Failed to spawn slave process"). Ordinary domain-decomposed `mpiexec` runs
are unaffected — MS-MPI fully covers point-to-point, collectives and
communicators. Possible future routes: **Intel MPI may bridge this under
Windows** — it ships natively for Windows (free, oneAPI HPC toolkit),
implements dynamic process management incl. `MPI_Comm_spawn` (needs its Hydra
service), and should slot in via the plain `find_package(MPI)` path by
rebuilding the MPI tree against it instead of MS-MPI; promising but unverified
with OPM, so test spawn before relying on it. Otherwise an upstream MPMD
redesign of the coupling that avoids spawn. Worth stating in any upstream PR
text and, ideally, as a hint appended to that runtime error message.
