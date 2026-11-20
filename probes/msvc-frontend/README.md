# MSVC front-end probes

Each file reduces one compiler workaround on the `windows` branches to a few
lines, in the form upstream has, so it can be checked against a newer MSVC in
isolation: `compiler-explorer.py` compiles them on godbolt.org with the flags
`build-module.ps1` gives OPM. A probe that compiles on an MSVC is a workaround
that MSVC no longer needs. Each probe also compiles with GCC, which is the
check that the reduction is valid C++.

Answers on 2026-11-20 (Compiler Explorer's newest MSVC was 19.51, VS 18.6):

| probe | workaround on the branch | 19.44 (VS 17.14) | 19.51 (VS 18.6) |
|---|---|---|---|
| `local_enum.cpp` | opm-simulators `GenericOutputModule.cpp`: phase enum moved to namespace scope | C2440, C2439 | C2440, C2439 |
| `satfunc_array.cpp` | opm-simulators `SatfuncConsistencyChecks`: violation sample moved out of the class | C2079 | C2079 |
| `dilu_omp.cpp` | opm-simulators `DILU.hpp`: loop bound hoisted, `<` for `!=` (`/openmp:llvm`) | C1001 (ICE) | C1001 (ICE) |
| `tpsa_conversion.cpp` | opm-simulators `TpsaVector.cpp`: conversion type spelled out | C2833 | C7753 |
| `friend_class.cpp` | opm-grid `CpGridData`: `cpgrid::Intersection` forward-declared | C2990 | C2990 |
| `omp_range_for.cpp` | opm-grid `ElementChunks::operator[]` for index-based OpenMP loops | C2059 | C2059 |
| `fieldmatrix_mulassign.cpp` | opm-upscaling `IncompFlowSolverHybrid.hpp`: `*= Scalar{2}` | C2666 | C2666 |
| `long_raw_string` (generated) | opm-common tests: raw literals over 16380 characters split | C2026 | compiles |

- `satfunc_array.cpp` fails only because the header uses `std::array` without
  including `<array>`. MSVC's other headers declare `std::array` without
  defining it. With `-DWITH_ARRAY` the include is there, the class stays
  nested, and every compiler accepts it. The include is the whole fix, so
  moving the class out is not needed.
- `long_raw_string`: 19.51 takes a single literal of 38400 characters, so the
  split is needed only while VS 2022 (19.44) is supported.
- The runners build with 19.52 (VS 18.12), newer than anything on Compiler
  Explorer. These probes say nothing about it until Compiler Explorer adds it
  or they are compiled on a runner.
- The 19.51 miscompile of the hysteresis snapshot (opm-common, see PATCHES.md)
  is a code-generation bug. A compile check cannot show it.
