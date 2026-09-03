# Probes

Small stand-alone programs that settle one question each about the Windows
build, kept so that a statement in `PATCHES.md` or in a PR description can be
re-checked rather than believed. They came out of an independent review of the
four `windows` branches on 2026-09-03. None of them is part of the build.

| probe | question | answer on 2026-09-03 |
|---|---|---|
| `mpi-spawn.c` | Does MS-MPI carry out `MPI_Comm_spawn`? | Yes, under `mpiexec` (MS-MPI 10.1.12498.52): the child reaches `MPI_Init`, the parent gets `MPI_SUCCESS`. Started without a process manager, the spawn fails - that is the launch context, not the API. |
| `constants.cpp` | Do the constants that opm-common now computes as `constexpr` keep their value for an autodiff scalar? | Yes: for `Evaluation<float, 3>` the old in-Scalar expressions and the new constants agree to the last digit, as does plain `float`. |
| `cxx-consumer/` | Can a project that enables only CXX consume the installed opm-common package? | No, and not because of the Windows series: upstream's export carries the `c_std_11` compile feature and `OpenMP::OpenMP_C`, which need C enabled, before `MPI::MPI_C` comes into it. With C enabled (`-DPROBE_ENABLE_C=ON`, the control) the package configures and the probe builds. |

Build and run instructions are at the top of each file. All of them need the
build environment (`. .\setup-env.ps1` from the harness root); the consumer
probe also needs an installed opm-common (`install-mpi\`).

This is an unofficial Windows build of OPM Flow, not an OPM release.
