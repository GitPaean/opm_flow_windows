# Copyright (C) 2026 SINTEF Digital; GPL-3.0-or-later (see LICENSE).
# build-intelmpi.ps1 - EXPERIMENTAL: build the parallel flow stack against
# Intel MPI (oneAPI) instead of MS-MPI, into separate build-impi\ /
# install-impi\ trees. The MS-MPI build (build-mpi\) is left untouched.
#
# Prereq (no admin needed): python -m pip install --user impi-rt impi-devel
# Sources must already be cloned/patched (run build-all.ps1 once first).
#
# Motivation: the Windows-vs-Linux Norne matrix (BUILD_WINDOWS.md §9) shows
# the remaining parallel gap grows with MPI rank count (output gather,
# pre/post collectives) — this tree tests whether Intel MPI closes it.
[CmdletBinding()]
param([int]$Jobs = 8)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $Root 'setup-env.ps1') | Out-Null

$impi = Join-Path $env:APPDATA 'Python\Library'
if (-not (Test-Path "$impi\lib\impi.lib")) { throw "Intel MPI not found at $impi" }
function Step($m) { Write-Host "==== $m ====" -ForegroundColor Cyan }

# --- Zoltan against Intel MPI ----------------------------------------------
$tri = Join-Path $Root 'deps\Trilinos'
if (-not (Test-Path (Join-Path $tri '.git'))) { throw 'Trilinos clone missing (run build-all.ps1 -Mpi once first)' }
if (-not (Test-Path (Join-Path $Root 'install-impi\lib\zoltan.lib'))) {
    Step 'Zoltan (Intel MPI)'
    $bld = Join-Path $Root 'build-impi\Trilinos'
    cmake -S $tri -B $bld -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_INSTALL_PREFIX="$Root\install-impi" `
        -DBUILD_SHARED_LIBS=OFF `
        -DTrilinos_ENABLE_ALL_PACKAGES=OFF `
        -DTrilinos_ENABLE_Zoltan=ON `
        -DTrilinos_ENABLE_Fortran=OFF `
        -DTrilinos_ENABLE_TESTS=OFF `
        -DTrilinos_ENABLE_EXAMPLES=OFF `
        -DZoltan_ENABLE_TESTS=OFF `
        -DZoltan_ENABLE_EXAMPLES=OFF `
        -DTPL_ENABLE_MPI=ON `
        -DMPI_USE_COMPILER_WRAPPERS=OFF `
        -DTPL_MPI_INCLUDE_DIRS="$impi\include" `
        -DTPL_MPI_LIBRARIES="$impi\lib\impi.lib" `
        -DTPL_ENABLE_DLlib=OFF `
        -DTPL_ENABLE_Pthread=OFF
    if ($LASTEXITCODE -ne 0) { throw 'Zoltan configure failed' }
    cmake --build $bld --target install -- -j $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'Zoltan build failed' }
}
Write-Host 'PHASE OK: zoltan'

# --- DUNE + OPM chain -------------------------------------------------------
$bm = Join-Path $Root 'build-module.ps1'
foreach ($m in 'dune-common','dune-geometry','dune-istl','dune-grid') {
    & $bm $m -IntelMpi -Jobs $Jobs
    Write-Host "PHASE OK: $m"
}
& $bm opm-common -IntelMpi -OpenMP -Jobs $Jobs
Write-Host 'PHASE OK: opm-common'
& $bm opm-grid -IntelMpi -OpenMP -Jobs $Jobs
Write-Host 'PHASE OK: opm-grid'
& $bm opm-simulators -IntelMpi -OpenMP -Target flow_blackoil -Jobs $Jobs
Write-Host 'PHASE OK: opm-simulators'
Write-Host 'INTELMPI BUILD DONE'
