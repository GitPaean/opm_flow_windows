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
  when `BUILD_TESTING` is ON. With testing off, opm-simulators' modelTests.cmake
  hook still registered tests that `add_dependencies(test-suite <example-target>)`
  on example targets that were never created -> configure error.
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
- `NonlinearSystemBlackOilReservoir.hpp`: `#undef STRICT`/`RELAXED` before the
  `enum class DebugFlags` (windows.h leaks `#define STRICT 1` -> "1 = 0").
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
- `SatfuncConsistencyChecks.hpp`: `ViolationCollection` std::array -> std::vector
  (sized to NumLevels); moving the element type out was not enough — MSVC still
  treated the dependent element of a std::array *member* as incomplete.
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

## MPI support (work in progress) — MS-MPI on Windows
Goal: enable MPI. Status: MS-MPI installed; DUNE (all 4) + opm-common build with
MPI; **blocked at opm-grid because OPM couples MPI => Zoltan and Zoltan has no
Windows build** (not in vcpkg; part of Trilinos).

MPI builds use separate `build-mpi/` + `install-mpi/` trees (build-module.ps1 -Mpi).
Patches made so far:
- dune-common `cmake/modules/DuneCommonMacros.cmake`: `find_package(MPI 3.0 ...)`
  -> `find_package(MPI ...)`. CMake detects MS-MPI as v2.0, so the 3.0 gate
  rejected it. (MS-MPI implements most MPI-3 features but is labelled 2.0.)
- dune-common `dune/common/parallel/mpitraits.hh`: guard the
  `MPI_CXX_{DOUBLE,LONG_DOUBLE,FLOAT}_COMPLEX` traits with
  `#ifdef MPI_CXX_DOUBLE_COMPLEX` (these MPI-3 datatypes are absent in MS-MPI).
- opm-common `CMakeLists.txt` prereqs hook: under `USE_MPI`, explicitly
  `find_package(MPI COMPONENTS C)` and `target_link_libraries(opmcommon PUBLIC
  MPI::MPI_C)`. Needed because DUNE does not export MPI on its installed target
  (MPI lives only in DUNE build-time ALL_PKG_FLAGS) and this build consumes
  installed modules rather than using dunecontrol. This makes mpi_checks() define
  HAVE_MPI=1; opm-common then built with HAVE_MPI (parallel EclipseState etc.).

BLOCKER: opm-grid `GraphOfGridWrappers.{hpp,cpp}`, `ZoltanGraphFunctions`, etc.
reference Zoltan types (ZOLTAN_ID_PTR, ZOLTAN_FATAL) under `#if HAVE_MPI` with no
`HAVE_ZOLTAN` guard, so enabling MPI forces compiling Zoltan code. opm-grid's
CMake also hard-requires Zoltan when MPI is on (REQUIRE_ZOLTAN). vcpkg has
metis/parmetis but not zoltan. Decoupling MPI from Zoltan (guarding the Zoltan
code with HAVE_ZOLTAN and using (Par)METIS for partitioning) is the realistic
path but is substantial source work + a long MSVC-fix tail in the parallel code.

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
`-DWITH_NATIVE=OFF` (drop `-mtune=native`), `-DCMAKE_DISABLE_FIND_PACKAGE_OpenMP=TRUE`
(MSVC's OpenMP 2.0 requires *signed* loop indices; OPM uses size_t -> C3016.
Revisit with `/openmp:llvm` for the simulator if OpenMP perf is wanted).
Result: `opmi.exe` parses SPE1CASE1.DATA end-to-end (exit 0).
