# Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
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

  The OPM modules are cloned from the 'windows' branch of the GitPaean forks,
  which carry the Windows/MSVC fixes on top of OPM master (-OpmOrg/-OpmBranch),
  and checked out at the commits opm-sources.json pins: a set verified to build
  and run together, where the branch heads move with every rebase of the series.
  -OpmLatest builds the branch heads instead. vcpkg and Trilinos are pinned in
  the same file (-VcpkgRef / -TrilinosRef override). A checkout already there
  is never moved; the build only warns when it is not at the pinned commit.

  Usage:
    .\build-all.ps1                      # serial build
    .\build-all.ps1 -Mpi                 # parallel (MPI) build
    .\build-all.ps1 -Mpi -OpenMP -SimTarget flow   # what the release packages ship
    .\build-all.ps1 -SkipClone -SkipDeps
    .\build-all.ps1 -OpmLatest           # the windows branch heads, not the pins
    .\build-all.ps1 -VcpkgRef master     # newest vcpkg ports (also -TrilinosRef)
#>
[CmdletBinding()]
param(
    [switch]$SkipClone,
    [switch]$SkipDeps,
    [switch]$Mpi,
    # Enable OpenMP threading (/openmp:llvm) in the OPM modules. Composes with
    # -Mpi to produce a hybrid MPI+OpenMP flow. DUNE is always built without it.
    [switch]$OpenMP,
    # Also clone + build opm-upscaling: its library plus the upscale_* / cpchop
    # tools (which live in examples/, so this builds with BUILD_EXAMPLES=ON).
    # Off by default to keep the core flow build lean.
    [switch]$Upscaling,
    # opm-simulators target to build. Default 'flow_blackoil' (one binary); use
    # 'all' to build every flow_* variant (the full simulator suite).
    [string]$SimTarget = 'flow_blackoil',
    # Parallel compile jobs forwarded to each module build. Default 4 (OPM's
    # heavy template TUs are RAM-hungry; more than ~4 cl.exe can exhaust memory
    # on typical machines). Raise on a machine with more RAM, e.g. -Jobs 6.
    [int]$Jobs = 4,
    [string]$DuneVersion = 'v2.10.0',
    # GitHub org and branch to clone opm-common/opm-grid/opm-simulators from.
    # Upstream OPM does not build with MSVC on its own: the Windows/MSVC fixes
    # live on the 'windows' branch of the GitPaean forks, rebased onto OPM
    # master as it moves, so that is the default. -OpmOrg OPM -OpmBranch master
    # takes upstream as it is.
    [string]$OpmOrg = 'GitPaean',
    [string]$OpmBranch = 'windows',
    # Clone the heads of -OpmBranch rather than the commits opm-sources.json
    # pins: for working on the series itself. The pins are what was verified.
    [switch]$OpmLatest,
    # vcpkg (the ports, and so every dependency's version) and Trilinos (where
    # Zoltan comes from, for -Mpi) are pinned in opm-sources.json too. Either
    # can be overridden with a commit, tag or branch - e.g. -VcpkgRef master
    # -TrilinosRef develop for their newest state.
    [string]$VcpkgRef,
    [string]$TrilinosRef
)

# 'windows' is the GitPaean forks' branch. Another org given without a branch
# means that org's own default branch - upstream OPM has no 'windows' to clone.
if ($OpmOrg -ne 'GitPaean' -and -not $PSBoundParameters.ContainsKey('OpmBranch')) { $OpmBranch = '' }

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
        if ($TrilinosCheckout) {
            # The one commit only: the build needs no history, and Trilinos'
            # is large.
            Invoke-Native { git init --quiet $tri }
            Invoke-Native { git -C $tri remote add origin https://github.com/trilinos/Trilinos.git }
            Invoke-Native { git -C $tri fetch --quiet --depth 1 origin $TrilinosCheckout }
            Invoke-Native { git -C $tri checkout --quiet --detach FETCH_HEAD }
            Write-Host "Trilinos checked out at $TrilinosCheckout"
        } else {
            Invoke-Native { git clone --depth 1 https://github.com/trilinos/Trilinos.git $tri }
        }
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
    Invoke-Native { cmake --build $bld --target install -- -j $Jobs }
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
# The OPM commits to build. opm-sources.json pins a set verified to build and
# run together (update-opm-pins.ps1 writes it). The pins belong to the org and
# branch they were taken from: with another -OpmOrg/-OpmBranch, or -OpmLatest,
# the branch heads are built.
$opmModules = @('opm-common','opm-grid','opm-simulators')
if ($Upscaling) { $opmModules += 'opm-upscaling' }
$lock = $null
$pinFile = Join-Path $Root 'opm-sources.json'
if (Test-Path $pinFile) { $lock = Get-Content -Raw $pinFile | ConvertFrom-Json }
$OpmPins = $null
if ($lock -and -not $OpmLatest -and $lock.org -eq $OpmOrg -and $lock.branch -eq $OpmBranch) { $OpmPins = $lock }
# vcpkg and Trilinos: the pinned commit, unless -VcpkgRef / -TrilinosRef name
# another. The *Pin variables are what an existing checkout is held against.
$VcpkgPin    = $null
$TrilinosPin = $null
if ($lock -and $lock.vcpkg    -and -not $VcpkgRef)    { $VcpkgPin    = $lock.vcpkg.commit }
if ($lock -and $lock.trilinos -and -not $TrilinosRef) { $TrilinosPin = $lock.trilinos.commit }
$VcpkgCheckout    = if ($VcpkgRef)    { $VcpkgRef }    else { $VcpkgPin }
$TrilinosCheckout = if ($TrilinosRef) { $TrilinosRef } else { $TrilinosPin }
function Get-OpmPin([string]$Module) {
    if ($OpmPins -and $OpmPins.modules.$Module) { return $OpmPins.modules.$Module.commit }
    return $null
}
function Get-GitHead([string]$Dir) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $sha = git -C $Dir rev-parse HEAD 2>$null
    $ErrorActionPreference = $prev
    if ($LASTEXITCODE -eq 0 -and $sha) { return "$sha".Trim() }
    return $null
}

if (-not $SkipClone) {
    Step "2/4  Clone vcpkg, DUNE ($DuneVersion) and OPM sources"
    New-Item -ItemType Directory -Force -Path (Join-Path $Root 'src') | Out-Null
    $vcpkgDir = Join-Path $Root 'vcpkg'
    if (-not (Test-Path (Join-Path $vcpkgDir '.git'))) {
        if ($VcpkgCheckout) {
            # The one commit only, as for Trilinos: classic-mode vcpkg reads its
            # ports from the working tree and needs no history. This happens
            # before the bootstrap below, so the vcpkg tool it fetches is the
            # one this commit names.
            Invoke-Native { git init --quiet $vcpkgDir }
            Invoke-Native { git -C $vcpkgDir remote add origin https://github.com/microsoft/vcpkg.git }
            Invoke-Native { git -C $vcpkgDir fetch --quiet --depth 1 origin $VcpkgCheckout }
            Invoke-Native { git -C $vcpkgDir checkout --quiet --detach FETCH_HEAD }
            Write-Host "vcpkg checked out at $VcpkgCheckout"
        } else {
            Invoke-Native { git clone https://github.com/microsoft/vcpkg.git $vcpkgDir }
        }
    }
    # Clone with core.autocrlf=false: git-for-Windows defaults to autocrlf=true,
    # which rewrites checked-out text files to CRLF. The OPM test data contains
    # byte-position-sensitive formatted Eclipse files (e.g. tests/ECLFILE.FINIT)
    # whose readers reject the injected \r, so sources must be checked out
    # byte-exact.
    #
    # The DUNE GitLab is the source of the core modules; github.com/dune-project
    # mirrors them with the same commits and tags. Fall back to the mirror when
    # GitLab does not answer: its proxy returning 524 once stopped the weekly
    # build before it compiled anything.
    foreach ($m in 'dune-common','dune-geometry','dune-grid','dune-istl') {
        $dst = Join-Path $Root "src\$m"
        if (-not (Test-Path (Join-Path $dst '.git'))) {
            $cloned = $false
            foreach ($url in "https://gitlab.dune-project.org/core/$m.git",
                             "https://github.com/dune-project/$m.git") {
                $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
                git clone -c core.autocrlf=false $url $dst
                $cloned = ($LASTEXITCODE -eq 0)
                $ErrorActionPreference = $prev
                if ($cloned) { break }
                Write-Warning "could not clone $m from $url"
                if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
            }
            if (-not $cloned) { throw "could not clone $m from GitLab or its GitHub mirror" }
        }
        # Only go back to the remote when the tag is not there yet, so a
        # rebuild in an existing tree does not depend on GitLab either.
        $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        git -C $dst rev-parse --verify --quiet "refs/tags/$DuneVersion" | Out-Null
        $haveTag = ($LASTEXITCODE -eq 0)
        $ErrorActionPreference = $prev
        if (-not $haveTag) { Invoke-Native { git -C $dst fetch --tags --quiet } }
        Invoke-Native { git -C $dst checkout --quiet $DuneVersion }
    }
    # Apply the DUNE Windows/MSVC patches (DUNE is built from source).
    Apply-DunePatch 'dune-common' 'dune-common-windows.patch'
    Apply-DunePatch 'dune-grid'   'dune-grid-windows.patch'
    foreach ($m in $opmModules) {
        $dst = Join-Path $Root "src\$m"
        if (-not (Test-Path (Join-Path $dst '.git'))) {
            $url = "https://github.com/$OpmOrg/$m.git"
            if ($OpmBranch) {
                Invoke-Native { git clone -c core.autocrlf=false --branch $OpmBranch $url $dst }
            } else {
                Invoke-Native { git clone -c core.autocrlf=false $url $dst }
            }
            $pin = Get-OpmPin $m
            if ($pin) {
                # A later rebase takes the pinned commit off the branch, but
                # GitHub still serves it by id.
                $prev = $ErrorActionPreference
                $ErrorActionPreference = 'Continue'
                git -C $dst cat-file -e "$pin^{commit}" 2>$null
                $have = ($LASTEXITCODE -eq 0)
                $ErrorActionPreference = $prev
                if (-not $have) { Invoke-Native { git -C $dst fetch --quiet origin $pin } }
                Invoke-Native { git -C $dst checkout --quiet --detach $pin }
                Write-Host "$m checked out at the pinned commit $pin"
            }
        }
    }
} else {
    Step "2/4  Clone (skipped)"
}

if ($OpmPins) {
    Write-Host "OPM sources: the commits opm-sources.json pins (snapshot $($OpmPins.snapshot))"
    # A checkout already under src\ stays as it is - the series is developed
    # there too - but a build of anything else is not the verified set.
    foreach ($m in $opmModules) {
        $pin  = Get-OpmPin $m
        $head = Get-GitHead (Join-Path $Root "src\$m")
        if ($pin -and $head -and $head -ne $pin) {
            Write-Warning ("src\{0} is at {1}, not at the pinned {2}: building what is checked out. For the verified set: git -C src\{0} fetch origin {3}; git -C src\{0} checkout --detach {3}" -f $m, $head.Substring(0, 10), $pin.Substring(0, 10), $pin)
        }
    }
} else {
    Write-Host "OPM sources: not pinned - a new clone takes the head of $OpmOrg/$(if ($OpmBranch) { $OpmBranch } else { 'its default branch' })"
}

# The same for vcpkg and Trilinos. A vcpkg checkout moved by hand would not
# change what is installed already, so here even more than for OPM the build
# only says so. Trilinos is cloned with Zoltan, later, if this build needs it.
$deps = @(@{ Name = 'vcpkg';    Dir = 'vcpkg';         Pin = $VcpkgPin;    Ref = $VcpkgRef;    Lock = $lock.vcpkg },
          @{ Name = 'Trilinos'; Dir = 'deps\Trilinos'; Pin = $TrilinosPin; Ref = $TrilinosRef; Lock = $lock.trilinos })
foreach ($d in $deps) {
    if ($d.Name -eq 'Trilinos' -and -not $Mpi) { continue }
    if ($d.Ref) {
        Write-Host "$($d.Name): $($d.Ref) (given), not the pinned commit"
    } elseif ($d.Pin) {
        Write-Host "$($d.Name): the commit opm-sources.json pins ($($d.Lock.date))"
        $head = Get-GitHead (Join-Path $Root $d.Dir)
        if ($head -and $head -ne $d.Pin) {
            Write-Warning ("{0}\ is at {1}, not at the pinned {2}: the dependencies are built from what is checked out. update-opm-pins.ps1 -Check compares; README.md, 'Pinned OPM sources', says how to move it." -f $d.Dir, $head.Substring(0, 10), $d.Pin.Substring(0, 10))
        }
    } else {
        Write-Host "$($d.Name): not pinned - a new clone takes its default branch"
    }
}

# --------------------------------------------------------------------------
# The vcpkg dependency set - one source of truth, used both to install the
# packages and (under -SkipDeps) to check that none of them is missing.
$VcpkgPackages = @(
    'suitesparse-umfpack', 'fmt', 'lapack',
    'boost-test', 'boost-date-time', 'boost-property-tree', 'boost-mpl',
    'boost-range', 'boost-spirit', 'boost-filesystem', 'boost-system',
    'boost-iostreams'
)

# Which of $VcpkgPackages are not installed for the triplet. vcpkg leaves a
# share\<port>\ directory behind for every installed port, so this costs no
# vcpkg.exe call and works even before bootstrap.
function Get-MissingVcpkgPackages {
    $share = Join-Path $Root 'vcpkg\installed\x64-windows\share'
    if (-not (Test-Path $share)) { return $VcpkgPackages }
    ,@($VcpkgPackages | Where-Object { -not (Test-Path (Join-Path $share $_)) })
}

function Install-VcpkgPackages {
    param([string[]]$Packages)
    if (-not (Test-Path (Join-Path $Root 'vcpkg\vcpkg.exe'))) {
        Invoke-Native { & (Join-Path $Root 'vcpkg\bootstrap-vcpkg.bat') -disableMetrics }
    }
    Invoke-Native {
        & (Join-Path $Root 'vcpkg\vcpkg.exe') install --triplet x64-windows $Packages
    }
}

if (-not $SkipDeps) {
    Step "3/4  Bootstrap vcpkg and install dependencies"
    Install-VcpkgPackages $VcpkgPackages
} else {
    # -SkipDeps must not mean "build without a dependency that was added to the
    # list since this tree was last populated". That failure surfaces much later
    # as a confusing compile error in whichever module needs it - a missing
    # boost-iostreams, for instance, breaks opm-upscaling on
    # boost/iostreams/copy.hpp after an hour of building everything else. So skip
    # the install, but still install anything actually absent.
    $missing = Get-MissingVcpkgPackages
    if ($missing.Count -eq 0) {
        Step "3/4  Dependencies (skipped; all $($VcpkgPackages.Count) packages present)"
    } else {
        Step "3/4  Dependencies (skipped, but $($missing.Count) missing - installing those)"
        Write-Warning ("-SkipDeps was given, but these vcpkg packages are missing: " +
                       ($missing -join ', ') + " - installing them now.")
        Install-VcpkgPackages $missing
    }
}

# --------------------------------------------------------------------------
Step "4/4  Build DUNE -> opm-common -> opm-grid -> opm-simulators$(if($Upscaling){' -> opm-upscaling'})$(if($Mpi){' (MPI)'})$(if($OpenMP){' (OpenMP)'})"
. (Join-Path $Root 'setup-env.ps1')

$buildModule = Join-Path $Root 'build-module.ps1'

# The executable to report at the end. -SimTarget names a target, and for every
# target except 'all' that is also the exe name; 'all' builds every flow_*
# variant, of which 'flow' is the one that carries them all.
$builtExe = if ($SimTarget -eq 'all') { 'flow' } else { $SimTarget }

# OpenMP applies to the OPM modules only; DUNE is always built without it.
if ($Mpi) {
    Ensure-MsMpi
    & $buildModule dune-common   -Mpi -Jobs $Jobs
    & $buildModule dune-geometry -Mpi -Jobs $Jobs
    & $buildModule dune-istl     -Mpi -Jobs $Jobs
    & $buildModule dune-grid     -Mpi -Jobs $Jobs
    Build-Zoltan                        # opm-grid (with MPI) requires Zoltan
    & $buildModule opm-common    -Mpi -OpenMP:$OpenMP -Jobs $Jobs
    & $buildModule opm-grid      -Mpi -OpenMP:$OpenMP -Jobs $Jobs
    & $buildModule opm-simulators -Mpi -OpenMP:$OpenMP -Target $SimTarget -Jobs $Jobs
    # opm-upscaling tools live in examples/, so build the 'all' target with examples on.
    if ($Upscaling) { & $buildModule opm-upscaling -Mpi -OpenMP:$OpenMP -Target all -Extra '-DBUILD_EXAMPLES=ON' -Jobs $Jobs }
    $exe = Join-Path $Root "build-mpi\opm-simulators\bin\$builtExe.exe"
} else {
    & $buildModule dune-common   -Jobs $Jobs
    & $buildModule dune-geometry -Jobs $Jobs
    & $buildModule dune-istl     -Jobs $Jobs
    & $buildModule dune-grid     -Jobs $Jobs
    & $buildModule opm-common    -OpenMP:$OpenMP -Jobs $Jobs
    & $buildModule opm-grid      -OpenMP:$OpenMP -Jobs $Jobs
    & $buildModule opm-simulators -OpenMP:$OpenMP -Target $SimTarget -Jobs $Jobs
    # opm-upscaling tools live in examples/, so build the 'all' target with examples on.
    if ($Upscaling) { & $buildModule opm-upscaling -OpenMP:$OpenMP -Target all -Extra '-DBUILD_EXAMPLES=ON' -Jobs $Jobs }
    $exe = Join-Path $Root "build\opm-simulators\bin\$builtExe.exe"
}

if (Test-Path $exe) {
    Write-Host "`nSUCCESS: $exe" -ForegroundColor Green
    Write-Host ("size: {0:N1} MB" -f ((Get-Item $exe).Length / 1MB))
    if ($Mpi) {
        Write-Host "Run in parallel:  mpiexec -n 2 `"$exe`" <deck>.DATA --output-dir=<dir>"
    }
} else {
    throw "$builtExe.exe was not produced"
}

if ($Upscaling) {
    $usBin = Join-Path $Root ("build$(if($Mpi){'-mpi'})\opm-upscaling\bin")
    $usTools = @(Get-ChildItem (Join-Path $usBin '*.exe') -ErrorAction SilentlyContinue)
    if ($usTools.Count -gt 0) {
        Write-Host "opm-upscaling: $($usTools.Count) tools in $usBin (upscale_perm, upscale_relperm, cpchop, ...)" -ForegroundColor Green
    } else {
        throw "opm-upscaling tools were not produced"
    }
}
