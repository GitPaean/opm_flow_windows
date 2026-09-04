# Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
#
# Build one OPM module with clang-cl instead of MSVC, against the DUNE, Zoltan
# and vcpkg libraries this harness already built. clang-cl targets the MSVC
# ABI and uses the MSVC standard library and linker, so install-mpi\ is
# reusable as it stands and only the OPM modules are recompiled.
#
# This is what the `windows_clang` branches of the four OPM forks are built
# with. They are the same port with the pure-MSVC workarounds removed; a
# Visual Studio build needs the `windows` branches instead.
#
#   .\build-clang.ps1 opm-common -Target all
#   .\build-clang.ps1 opm-grid
#   .\build-clang.ps1 opm-simulators -Target flow
#
# Toolchain: unpack the LLVM project's own Windows build (no installer, no
# administrator rights) next to this harness:
#
#   curl.exe -sSL -o llvm.tar.xz https://github.com/llvm/llvm-project/releases/download/llvmorg-23.1.0/clang%2Bllvm-23.1.0-x86_64-pc-windows-msvc.tar.xz
#   mkdir C:\opm\llvm; tar -xf llvm.tar.xz -C C:\opm\llvm --strip-components=1
#
# It still needs the Visual Studio Build Tools for the headers, the standard
# library and the Windows SDK; setup-env.ps1 puts those in the environment.
param(
    [Parameter(Mandatory=$true)][string]$Module,
    [string]$Target = 'all',
    [string]$Llvm = 'C:\opm\llvm',
    [string[]]$Extra = @(),
    [int]$Jobs = 8,
    [switch]$ConfigureOnly
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root
. .\setup-env.ps1 | Out-Null
if (-not (Test-Path "$Llvm\bin\clang-cl.exe")) { throw "clang-cl not found under $Llvm (see the header of this script)" }
$env:PATH = "$Llvm\bin;$Root\vcpkg\installed\x64-windows\bin;$env:PATH"

$Src     = "$Root\src\$Module"
$Build   = "$Root\build-clang\$Module"
$Install = "$Root\install-clang"
if (-not (Test-Path $Src)) { throw "source not found: $Src" }

# The same conformance set as the MSVC build - clang-cl accepts the /Zc: and
# /wd spellings - plus two suppressions for the GNU-style flags that OPM and
# DUNE hand every "GCC-compatible" compiler.
$compatInc = "/I" + (($Root -replace '\\','/') + '/compat/include')
$flags  = "/permissive- /Zc:__cplusplus /Zc:preprocessor /bigobj /EHsc /wd4068 -Wno-unknown-warning-option -Wno-unused-command-line-argument -D_USE_MATH_DEFINES /DWIN32 /D_WINDOWS $compatInc"
$cflags = "/wd4068 -Wno-unknown-warning-option -Wno-unused-command-line-argument -D_USE_MATH_DEFINES /DWIN32 /D_WINDOWS $compatInc"

$cmakeArgs = @(
    '-S', $Src, '-B', $Build, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_C_COMPILER=$Llvm/bin/clang-cl.exe",
    "-DCMAKE_CXX_COMPILER=$Llvm/bin/clang-cl.exe",
    "-DCMAKE_INSTALL_PREFIX=$Install",
    "-DCMAKE_PREFIX_PATH=$Install;$Root\install-mpi",
    "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_TOOLCHAIN",
    '-DVCPKG_TARGET_TRIPLET=x64-windows',
    "-DCMAKE_CXX_FLAGS=$flags",
    "-DCMAKE_C_FLAGS=$cflags",
    '-DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=TRUE',
    '-DCMAKE_DISABLE_FIND_PACKAGE_LATEX=TRUE',
    '-DBUILD_TESTING=OFF',
    '-DBUILD_EXAMPLES=OFF',
    '-DWITH_NATIVE=OFF',
    # OPM asks every GCC-compatible compiler for -Wall -Wextra -Wshadow, and
    # clang-cl reads -Wall as /Wall, which is -Weverything: tens of thousands
    # of C++98-compatibility warnings. Off until that is sorted out upstream.
    '-DOPM_ENABLE_WARNINGS=OFF',
    # OPM's AVX2 probe compiles with -mavx2 -mfma, which MSVC rejects and
    # clang-cl accepts, so clang pulls in the mixed-precision sources - and
    # bslv.c calls aligned_alloc(), which the Microsoft CRT does not have.
    '-DHAVE_AVX2_EXTENSION=0',
    '-DUSE_MPI=ON',
    '-DUSE_OPENMP=ON',
    # clang-cl spells it -openmp; it rejects the GNU driver's -fopenmp.
    '-DOpenMP_CXX_FLAGS=-openmp', '-DOpenMP_CXX_LIB_NAMES=libomp',
    '-DOpenMP_C_FLAGS=-openmp',   '-DOpenMP_C_LIB_NAMES=libomp',
    "-DOpenMP_libomp_LIBRARY=$Llvm/lib/libomp.lib"
) + $Extra

Write-Host "==== configure $Module with clang-cl ====" -ForegroundColor Cyan
cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "$Module configure failed ($LASTEXITCODE)" }
if ($ConfigureOnly) { return }

Write-Host "==== build $Module (target=$Target, -j $Jobs) ====" -ForegroundColor Cyan
cmake --build $Build --target $Target -- -j $Jobs -k 0
if ($LASTEXITCODE -ne 0) { throw "$Module build failed ($LASTEXITCODE)" }
