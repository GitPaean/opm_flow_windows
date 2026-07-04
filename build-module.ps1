# build-module.ps1 <module-name> [extra cmake -D args...]
# Configures, builds, and installs one module (DUNE or OPM) into the shared
# install prefix using the MSVC conformance flags OPM/DUNE need.
#
# Usage (env must already be loaded via . .\setup-env.ps1):
#   .\build-module.ps1 dune-geometry
#   .\build-module.ps1 opm-common -DBUILD_TESTING=OFF --target opmi
param(
    [Parameter(Mandatory)] [string]$Module,
    [string]$Target = "install",
    [switch]$Mpi,
    # Enable OpenMP threading via MSVC's LLVM runtime (/openmp:llvm). The default
    # /openmp (OpenMP 2.0) is too old for OPM's unsigned/size_t parallel-for
    # counters; /openmp:llvm (OpenMP 3.1+) handles them. Needs libomp140.x86_64.dll
    # at runtime (ships with MSVC).
    [switch]$OpenMP,
    # Parallel compile jobs. OPM's heavy template TUs use a lot of RAM: 8 jobs
    # suit machines with >= 32 GB; drop to 4 on typical 16 GB machines, where
    # more concurrent cl.exe processes can exhaust memory.
    [int]$Jobs = 8,
    [string[]]$Extra = @()
)

$Root    = $env:OPM_ROOT
# Fall back to this script's own directory (the layout root) when OPM_ROOT is
# not set (e.g. build-module.ps1 invoked without sourcing setup-env.ps1 first).
if (-not $Root) { $Root = $PSScriptRoot }

# MPI builds use separate build-mpi\ and install-mpi\ trees so the validated
# serial build is left intact.
$suffix  = if ($Mpi) { '-mpi' } else { '' }
$Src     = Join-Path $Root "src\$Module"
$Build   = Join-Path $Root "build$suffix\$Module"
$Install = Join-Path $Root "install$suffix"

if (-not (Test-Path $Src)) { throw "source not found: $Src" }

# MSVC conformance flags required across DUNE + OPM:
#   /permissive-       enable alternative tokens (and/or/not) + conformance
#   /Zc:__cplusplus    report the real __cplusplus value
#   /Zc:preprocessor   conformant preprocessor (variadic macros etc.)
#   /bigobj            template-heavy TUs exceed the default object section cap
#   /EHsc              standard C++ exception handling
#   /wd4068            ignore unknown GCC pragmas (#pragma GCC ...)
#   -D_USE_MATH_DEFINES expose M_PI etc. from <cmath> on MSVC
#   /DWIN32 /D_WINDOWS  CMake's default Windows defines; setting CMAKE_CXX_FLAGS
#                       overrides CMAKE_CXX_FLAGS_INIT, so we must restore them
#                       (some vendored code, e.g. ResInsight LibCore, checks WIN32).
# Use forward slashes for the include path: DUNE bakes CMAKE_CXX_FLAGS verbatim
# into its installed *-config.cmake files, and CMake's string parser rejects
# backslashes (e.g. "\U" in C:\Users...) as invalid escapes. Forward slashes are
# accepted by both MSVC and CMake. (Assumes OPM_ROOT has no spaces.)
$compatInc = "/I" + (($Root -replace '\\','/') + '/compat/include')
$flags = "/permissive- /Zc:__cplusplus /Zc:preprocessor /bigobj /EHsc /wd4068 -D_USE_MATH_DEFINES /DWIN32 /D_WINDOWS $compatInc"
# C flags: C files (e.g. opm-grid trans_tpfa.c) need the compat include for
# FCMacros.h plus the Windows defines, but not the C++-only conformance switches.
$cflags = "/wd4068 -D_USE_MATH_DEFINES /DWIN32 /D_WINDOWS $compatInc"

# Assemble all cmake arguments into one array and splat once (multiple @splats
# with backtick continuations are fragile in Windows PowerShell 5.1).
$cmakeArgs = @(
    '-S', $Src, '-B', $Build, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_INSTALL_PREFIX=$Install",
    "-DCMAKE_PREFIX_PATH=$Install",
    "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_TOOLCHAIN",
    '-DVCPKG_TARGET_TRIPLET=x64-windows',
    "-DCMAKE_CXX_FLAGS=$flags",
    "-DCMAKE_C_FLAGS=$cflags",
    '-DDUNE_ENABLE_PYTHONBINDINGS=OFF',
    '-DCMAKE_DISABLE_FIND_PACKAGE_Doxygen=TRUE',
    '-DCMAKE_DISABLE_FIND_PACKAGE_LATEX=TRUE',
    '-DBUILD_TESTING=OFF',
    '-DBUILD_EXAMPLES=OFF',
    '-DWITH_NATIVE=OFF'
)
# MPI: when -Mpi is given, let CMake find Microsoft MPI; otherwise disable it.
if ($Mpi) { $cmakeArgs += '-DUSE_MPI=ON' } else { $cmakeArgs += '-DCMAKE_DISABLE_FIND_PACKAGE_MPI=TRUE' }

# OpenMP: -OpenMP pre-seeds FindOpenMP to use MSVC's /openmp:llvm (the default
# /openmp is OpenMP 2.0, too old for OPM's unsigned parallel-for counters).
# Otherwise disable OpenMP entirely.
if ($OpenMP) {
    $libomp = Join-Path $env:VCToolsInstallDir 'lib\x64\libomp.lib'
    $cmakeArgs += @(
        '-DUSE_OPENMP=ON',
        '-DOpenMP_CXX_FLAGS=/openmp:llvm', '-DOpenMP_CXX_LIB_NAMES=libomp', "-DOpenMP_libomp_LIBRARY=$libomp",
        '-DOpenMP_C_FLAGS=/openmp:llvm',   '-DOpenMP_C_LIB_NAMES=libomp'
    )
} else {
    $cmakeArgs += '-DCMAKE_DISABLE_FIND_PACKAGE_OpenMP=TRUE'
}
if ($Extra) { $cmakeArgs += $Extra }

Write-Host "==== configure $Module (MPI=$($Mpi.IsPresent)) ====" -ForegroundColor Cyan
cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "$Module configure failed ($LASTEXITCODE)" }

Write-Host "==== build $Module (target=$Target, -j $Jobs) ====" -ForegroundColor Cyan
# -k 0: keep going after a failing edge so all buildable targets still compile
# (test/example targets may not all be MSVC-clean; we want a full picture).
cmake --build $Build --target $Target -- -j $Jobs -k 0
if ($LASTEXITCODE -ne 0) { throw "$Module build failed ($LASTEXITCODE)" }
Write-Host "==== $Module OK ====" -ForegroundColor Green
