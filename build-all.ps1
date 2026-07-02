<#
  build-all.ps1 - end-to-end native-Windows (MSVC) build of OPM flow_blackoil.

  Runs the whole workflow described in BUILD_WINDOWS.md:
    1. generate the POSIX compatibility shims under compat\include
    2. clone vcpkg, DUNE (pinned to v2.10.0) and the OPM modules   (-SkipClone to skip)
    3. bootstrap vcpkg and install the dependencies                (-SkipDeps  to skip)
    4. build, in order: DUNE -> opm-common -> opm-grid -> opm-simulators (flow_blackoil)

  Prerequisite: the VS 2022 C++ toolset + Windows SDK + CMake + Ninja are installed
  (see BUILD_WINDOWS.md section 3). git must be on PATH.

  With -Mpi it additionally builds a parallel flow_blackoil (MS-MPI + Zoltan):
    - requires MS-MPI runtime+SDK (winget install Microsoft.msmpi Microsoft.msmpisdk)
    - clones Trilinos and builds the Zoltan package only (against MS-MPI)
    - builds DUNE + OPM into separate build-mpi\ / install-mpi\ trees
  The serial build (build\ / install\) is left untouched.

  Usage:
    .\build-all.ps1                      # serial build
    .\build-all.ps1 -Mpi                 # parallel (MPI) build
    .\build-all.ps1 -SkipClone -SkipDeps
#>
[CmdletBinding()]
param(
    [switch]$SkipClone,
    [switch]$SkipDeps,
    [switch]$Mpi,
    # Enable OpenMP threading (/openmp:llvm) in the OPM modules. Composes with
    # -Mpi to produce a hybrid MPI+OpenMP flow. DUNE is always built without it.
    [switch]$OpenMP,
    # opm-simulators target to build. Default 'flow_blackoil' (one binary); use
    # 'all' to build every flow_* variant (the full simulator suite).
    [string]$SimTarget = 'flow_blackoil',
    [string]$DuneVersion = 'v2.10.0',
    # GitHub org and branch to clone opm-common/opm-grid/opm-simulators from.
    # Until the Windows/MSVC fixes are merged upstream, point these at the fork
    # and branch that contain them, e.g. -OpmOrg myuser -OpmBranch windows-msvc.
    [string]$OpmOrg = 'OPM',
    [string]$OpmBranch = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

function Step($msg) { Write-Host "`n==== $msg ====" -ForegroundColor Cyan }

# Run a native command and throw on non-zero exit. Force ErrorActionPreference to
# 'Continue' locally so that tools writing progress to stderr (e.g. git) do not
# raise a spurious NativeCommandError terminating error under 'Stop'.
function Invoke-Native {
    param([scriptblock]$Cmd)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $global:LASTEXITCODE = 0
    & $Cmd
    $ErrorActionPreference = $prev
    if ($LASTEXITCODE -ne 0) { throw "command failed (exit $LASTEXITCODE): $Cmd" }
}

# Ensure MS-MPI (runtime + SDK) is present; the SDK provides the machine-level
# MSMPI_INC env var. Try winget if missing (needs an approved UAC prompt).
function Ensure-MsMpi {
    if ([Environment]::GetEnvironmentVariable('MSMPI_INC','Machine')) { return }
    Write-Host "MS-MPI not found; attempting winget install (approve the UAC prompt)..." -ForegroundColor Yellow
    winget install --id Microsoft.msmpi    -e --accept-package-agreements --accept-source-agreements
    winget install --id Microsoft.msmpisdk -e --accept-package-agreements --accept-source-agreements
    if (-not [Environment]::GetEnvironmentVariable('MSMPI_INC','Machine')) {
        throw "MS-MPI SDK not detected after install. Install Microsoft.msmpi and Microsoft.msmpisdk, then re-run."
    }
}

# Build the Zoltan package (only) from a Trilinos clone, against MS-MPI, into
# install-mpi so opm-grid's FindZOLTAN locates it.
function Build-Zoltan {
    $tri = Join-Path $Root 'deps\Trilinos'
    if (-not (Test-Path (Join-Path $tri '.git'))) {
        Invoke-Native { git clone --depth 1 https://github.com/trilinos/Trilinos.git $tri }
    }
    if (Test-Path (Join-Path $Root 'install-mpi\lib\zoltan.lib')) {
        Write-Host "Zoltan already built (install-mpi\lib\zoltan.lib)"; return
    }
    $bld = Join-Path $Root 'build-mpi\Trilinos'
    Invoke-Native { cmake -S $tri -B $bld -G Ninja `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_INSTALL_PREFIX="$Root\install-mpi" `
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
        -DTPL_MPI_INCLUDE_DIRS="$env:MSMPI_INC" `
        -DTPL_MPI_LIBRARIES="$env:MSMPI_LIB64\msmpi.lib" `
        -DTPL_ENABLE_DLlib=OFF `
        -DTPL_ENABLE_Pthread=OFF }
    Invoke-Native { cmake --build $bld --target install -- -j 4 }
}

# Apply a Windows/MSVC patch to a DUNE module (DUNE is built from source, so the
# fixes must be applied to the fresh checkout). Idempotent: skips if already applied.
function Apply-DunePatch {
    param([string]$Module, [string]$PatchFile)
    $dst   = Join-Path $Root "src\$Module"
    $patch = Join-Path $Root "patches\$PatchFile"
    if (-not (Test-Path $patch)) { Write-Warning "patch not found: $patch"; return }
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    git -C $dst apply --reverse --check $patch 2>$null    # succeeds if already applied
    $already = ($LASTEXITCODE -eq 0)
    $ErrorActionPreference = $prev
    if ($already) { Write-Host "  ${Module}: Windows patch already applied"; return }
    Invoke-Native { git -C $dst apply $patch }
    Write-Host "  ${Module}: applied $PatchFile"
}

# --------------------------------------------------------------------------
Step "1/4  Generate POSIX compatibility shims (compat\include)"
# --------------------------------------------------------------------------
$compat = Join-Path $Root 'compat\include'
New-Item -ItemType Directory -Force -Path (Join-Path $compat 'sys') | Out-Null

# In this repo the shims are committed under compat\include and are the source of
# truth; only (re)generate them if missing (e.g. running the script standalone).
if (Test-Path (Join-Path $compat 'FCMacros.h')) {
  Write-Host "compat shims already present in $compat (using committed copies)"
} else {
@'
/* Minimal <unistd.h> shim for MSVC. */
#ifndef OPM_COMPAT_UNISTD_H
#define OPM_COMPAT_UNISTD_H
#ifndef _CRT_NONSTDC_NO_DEPRECATE
#define _CRT_NONSTDC_NO_DEPRECATE
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif
#include <io.h>
#include <process.h>
#include <direct.h>
#include <stdlib.h>
#include <string.h>
#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 0
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef STDIN_FILENO
#define STDIN_FILENO  0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) void __stdcall Sleep(unsigned long dwMilliseconds);
#ifdef __cplusplus
}
#endif
static __inline unsigned int sleep(unsigned int seconds) { Sleep(seconds * 1000u); return 0; }
static __inline int usleep(unsigned int usec) { Sleep(usec / 1000u); return 0; }
/* POSIX gethostname(): absent on MSVC (winsock only). Best-effort from the
   COMPUTERNAME environment variable, enough for the log banners OPM uses it for. */
static __inline int gethostname(char* name, size_t namelen) {
    const char* cn = getenv("COMPUTERNAME");
    if (!name || namelen == 0) { return -1; }
    strncpy_s(name, namelen, cn ? cn : "localhost", _TRUNCATE);
    return 0;
}
#endif /* OPM_COMPAT_UNISTD_H */
'@ | Set-Content -Encoding ascii (Join-Path $compat 'unistd.h')

@'
/* Minimal <sys/ioctl.h> shim for MSVC. Intentionally does NOT define TIOCGWINSZ,
   so OPM falls back to its default terminal width. */
#ifndef OPM_COMPAT_SYS_IOCTL_H
#define OPM_COMPAT_SYS_IOCTL_H
#endif
'@ | Set-Content -Encoding ascii (Join-Path $compat 'sys\ioctl.h')

@'
/* Minimal <sys/utsname.h> shim for MSVC. */
#ifndef OPM_COMPAT_SYS_UTSNAME_H
#define OPM_COMPAT_SYS_UTSNAME_H
#include <string.h>
#include <stdlib.h>
#define _UTSNAME_LENGTH 256
struct utsname {
    char sysname[_UTSNAME_LENGTH];
    char nodename[_UTSNAME_LENGTH];
    char release[_UTSNAME_LENGTH];
    char version[_UTSNAME_LENGTH];
    char machine[_UTSNAME_LENGTH];
};
static __inline int uname(struct utsname* buf) {
    if (!buf) { return -1; }
    strncpy_s(buf->sysname, _UTSNAME_LENGTH, "Windows", _TRUNCATE);
    const char* host = getenv("COMPUTERNAME");
    strncpy_s(buf->nodename, _UTSNAME_LENGTH, host ? host : "localhost", _TRUNCATE);
    buf->release[0] = '\0';
    buf->version[0] = '\0';
    const char* arch = getenv("PROCESSOR_ARCHITECTURE");
    strncpy_s(buf->machine, _UTSNAME_LENGTH, arch ? arch : "x86_64", _TRUNCATE);
    return 0;
}
#endif
'@ | Set-Content -Encoding ascii (Join-Path $compat 'sys\utsname.h')

@'
/* Minimal POSIX getopt() shim for MSVC (single-TU tools only). */
#ifndef OPM_COMPAT_GETOPT_H
#define OPM_COMPAT_GETOPT_H
#include <stdio.h>
#include <string.h>
static char* optarg = NULL;
static int   optind = 1;
static int   opterr = 1;
static int   optopt = 0;
static int getopt(int argc, char* const argv[], const char* optstring) {
    static int optpos = 1;
    const char* arg; int optchar; const char* match;
    optarg = NULL;
    if (optind >= argc) return -1;
    arg = argv[optind];
    if (arg[0] != '-' || arg[1] == '\0') return -1;
    if (arg[1] == '-' && arg[2] == '\0') { ++optind; return -1; }
    optchar = (unsigned char)arg[optpos];
    match = strchr(optstring, optchar);
    if (optchar == ':' || match == NULL) {
        optopt = optchar;
        if (opterr && *optstring != ':') fprintf(stderr, "%s: illegal option -- %c\n", argv[0], optchar);
        if (arg[++optpos] == '\0') { ++optind; optpos = 1; }
        return '?';
    }
    if (match[1] == ':') {
        if (arg[optpos + 1] != '\0') { optarg = (char*)&arg[optpos + 1]; ++optind; }
        else if (optind + 1 < argc) { optarg = (char*)argv[optind + 1]; optind += 2; }
        else {
            optopt = optchar; ++optind; optpos = 1;
            if (opterr && *optstring != ':') fprintf(stderr, "%s: option requires an argument -- %c\n", argv[0], optchar);
            return (*optstring == ':') ? ':' : '?';
        }
        optpos = 1;
    } else {
        if (arg[++optpos] == '\0') { ++optind; optpos = 1; }
    }
    return optchar;
}
#endif
'@ | Set-Content -Encoding ascii (Join-Path $compat 'getopt.h')

@'
/* Fortran C-interface name-mangling for the OpenBLAS/LAPACK ABI on Windows
   (gfortran convention: lowercase + trailing underscore). Normally generated
   by CMake's FortranCInterface, which needs a Fortran compiler. */
#ifndef OPM_COMPAT_FCMACROS_H
#define OPM_COMPAT_FCMACROS_H
#define FC_GLOBAL(name, NAME)  name##_
#define FC_GLOBAL_(name, NAME) name##_
#endif
'@ | Set-Content -Encoding ascii (Join-Path $compat 'FCMacros.h')
Write-Host "generated compat shims in $compat"
}

# --------------------------------------------------------------------------
if (-not $SkipClone) {
    Step "2/4  Clone vcpkg, DUNE ($DuneVersion) and OPM sources"
    New-Item -ItemType Directory -Force -Path (Join-Path $Root 'src') | Out-Null
    if (-not (Test-Path (Join-Path $Root 'vcpkg\.git'))) {
        Invoke-Native { git clone https://github.com/microsoft/vcpkg.git (Join-Path $Root 'vcpkg') }
    }
    foreach ($m in 'dune-common','dune-geometry','dune-grid','dune-istl') {
        $dst = Join-Path $Root "src\$m"
        if (-not (Test-Path (Join-Path $dst '.git'))) {
            Invoke-Native { git clone "https://gitlab.dune-project.org/core/$m.git" $dst }
        }
        Invoke-Native { git -C $dst fetch --tags --quiet }
        Invoke-Native { git -C $dst checkout --quiet $DuneVersion }
    }
    # Apply the DUNE Windows/MSVC patches (DUNE is built from source).
    Apply-DunePatch 'dune-common' 'dune-common-windows.patch'
    Apply-DunePatch 'dune-grid'   'dune-grid-windows.patch'
    foreach ($m in 'opm-common','opm-grid','opm-simulators') {
        $dst = Join-Path $Root "src\$m"
        if (-not (Test-Path (Join-Path $dst '.git'))) {
            $url = "https://github.com/$OpmOrg/$m.git"
            if ($OpmBranch) {
                Invoke-Native { git clone --branch $OpmBranch $url $dst }
            } else {
                Invoke-Native { git clone $url $dst }
            }
        }
    }
} else {
    Step "2/4  Clone (skipped)"
}

# --------------------------------------------------------------------------
if (-not $SkipDeps) {
    Step "3/4  Bootstrap vcpkg and install dependencies"
    if (-not (Test-Path (Join-Path $Root 'vcpkg\vcpkg.exe'))) {
        Invoke-Native { & (Join-Path $Root 'vcpkg\bootstrap-vcpkg.bat') -disableMetrics }
    }
    Invoke-Native {
        & (Join-Path $Root 'vcpkg\vcpkg.exe') install --triplet x64-windows `
            suitesparse-umfpack fmt lapack `
            boost-test boost-date-time boost-property-tree boost-mpl `
            boost-range boost-spirit boost-filesystem boost-system
    }
} else {
    Step "3/4  Dependencies (skipped)"
}

# --------------------------------------------------------------------------
Step "4/4  Build DUNE -> opm-common -> opm-grid -> opm-simulators$(if($Mpi){' (MPI)'})$(if($OpenMP){' (OpenMP)'})"
. (Join-Path $Root 'setup-env.ps1')

$buildModule = Join-Path $Root 'build-module.ps1'

# OpenMP applies to the OPM modules only; DUNE is always built without it.
if ($Mpi) {
    Ensure-MsMpi
    & $buildModule dune-common   -Mpi
    & $buildModule dune-geometry -Mpi
    & $buildModule dune-istl     -Mpi
    & $buildModule dune-grid     -Mpi
    Build-Zoltan                        # opm-grid (with MPI) requires Zoltan
    & $buildModule opm-common    -Mpi -OpenMP:$OpenMP
    & $buildModule opm-grid      -Mpi -OpenMP:$OpenMP
    & $buildModule opm-simulators -Mpi -OpenMP:$OpenMP -Target $SimTarget
    $exe = Join-Path $Root 'build-mpi\opm-simulators\bin\flow_blackoil.exe'
} else {
    & $buildModule dune-common
    & $buildModule dune-geometry
    & $buildModule dune-istl
    & $buildModule dune-grid
    & $buildModule opm-common    -OpenMP:$OpenMP
    & $buildModule opm-grid      -OpenMP:$OpenMP
    & $buildModule opm-simulators -OpenMP:$OpenMP -Target $SimTarget
    $exe = Join-Path $Root 'build\opm-simulators\bin\flow_blackoil.exe'
}

if (Test-Path $exe) {
    Write-Host "`nSUCCESS: $exe" -ForegroundColor Green
    Write-Host ("size: {0:N1} MB" -f ((Get-Item $exe).Length / 1MB))
    if ($Mpi) {
        Write-Host "Run in parallel:  mpiexec -n 2 `"$exe`" <deck>.DATA --output-dir=<dir>"
    }
} else {
    throw "flow_blackoil.exe was not produced"
}
