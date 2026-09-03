# Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
#
# Can a C++-only project consume the installed opm-common package?
# Not this OpenMP-enabled one: with C not enabled, the exported opmcommon
# target's link interface fails on MPI::MPI_C (the series links MPI directly
# on Windows) and, that aside, on OpenMP::OpenMP_C from upstream's
# UseOpenMP.cmake. The exported c_std_11 compile feature alone is no such
# requirement. This is the failing case; the control below passes.
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
