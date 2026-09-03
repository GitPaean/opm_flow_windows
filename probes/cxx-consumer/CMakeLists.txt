# Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
#
# Can a C++-only project consume the installed opm-common package?
# No: with C not enabled, the exported opmcommon target's link interface
# fails on OpenMP::OpenMP_C (and on MPI::MPI_C on Windows, where the series
# links MPI directly), and its c_std_11 compile feature needs C too - all of
# it upstream's export. This is the failing case; the control below passes.
#
#   cmake -S probes\cxx-consumer -B build-consumer -G Ninja -DCMAKE_CXX_COMPILER=cl
#         "-DCMAKE_PREFIX_PATH=<harness>/install-mpi;<harness>/vcpkg/installed/x64-windows"
#         -DOpenMP_CXX_FLAGS=/openmp:llvm -DOpenMP_CXX_LIB_NAMES=libomp
#         "-DOpenMP_libomp_LIBRARY=<VC tools>\lib\x64\libomp.lib"
#   cmake --build build-consumer
#
# -DPROBE_ENABLE_C=ON is the control: with C enabled the package configures.
cmake_minimum_required(VERSION 3.23)
project(CxxOnlyConsumer LANGUAGES CXX)
if(PROBE_ENABLE_C)
  enable_language(C)
endif()
find_package(opm-common CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE opmcommon)
