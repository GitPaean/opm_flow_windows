# Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
<#
  package-flow.ps1 - stage a redistributable Windows package of OPM Flow
  (simulator + flow-gui) from this harness' build trees.

  Produces  dist\opm-flow-<Version>\
    bin\        flow*.exe, their runtime DLLs (vcpkg applocal set), the MSVC
                CRT + OpenMP runtime, flow-gui.exe + Qt runtime/plugins
    redist\     vc_redist.x64.exe + msmpisetup.exe (downloaded on demand)
    README.txt  install / run instructions
    LICENSE.txt GPLv3 notice + source availability statement

  Options:
    -Version    package version string          (default 2026.10-pre)
    -Simulators simulator exes to include       (default: flow — it contains
                all model variants incl. black-oil; add e.g. flow_blackoil
                only if a smaller single-model binary is wanted)
    -Zip        also produce dist\opm-flow-<Version>-win64.zip

  The staged tree is what installer\opm-flow.iss (Inno Setup) and
  packaging\build-msix.ps1 (MSIX) package further. See PACKAGING.md.
#>
[CmdletBinding()]
param(
    [string]  $Version    = '2026.10-pre',
    [string[]]$Simulators = @('flow'),
    # Package the Intel MPI build (build-impi\) instead of the MS-MPI one.
    # The Intel MPI runtime is NOT redistributed here: flow.exe is GPLv3, and
    # shipping a proprietary MPI inside the same package raises a licensing
    # question this project is not in a position to settle. The user installs
    # it themselves - which needs no administrator rights, unlike MS-MPI, and
    # is the whole point of this variant.
    [switch]  $IntelMpi,
    [switch]  $Zip
)

$ErrorActionPreference = 'Stop'
$Root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$SimBin = if ($IntelMpi) { Join-Path $Root 'build-impi\opm-simulators\bin' }
          else           { Join-Path $Root 'build-mpi\opm-simulators\bin' }
# Where the pip wheels (impi-rt / impi-devel) put the Intel MPI runtime.
$ImpiRoot = Join-Path $env:APPDATA 'Python\Library'
$GuiBin = Join-Path $Root 'build-gui'
# The Intel MPI package has to carry its own name. Both variants stage into
# dist\opm-flow-<Version>\ and zip to opm-flow-<Version>-win64.zip, so sharing
# a version means the second build silently overwrites the first - and the two
# differ only in which MPI the exes link, which nothing about the file shows.
if ($IntelMpi -and $Version -notmatch '-impi$') { $Version = "$Version-impi" }
$Stage  = Join-Path $Root "dist\opm-flow-$Version"
$Bin    = Join-Path $Stage 'bin'
$Redist = Join-Path $Stage 'redist'

function Step($m) { Write-Host "==== $m ====" -ForegroundColor Cyan }

# --- locate the MSVC redistributable runtimes ------------------------------
if (-not $env:VCToolsRedistDir) {
    . (Join-Path $Root 'setup-env.ps1') | Out-Null
}
$crtDir  = Join-Path $env:VCToolsRedistDir 'x64\Microsoft.VC143.CRT'
$ompDll  = Get-ChildItem -Recurse (Join-Path $env:VCToolsRedistDir 'x64') `
               -Filter 'libomp140.x86_64.dll' -ErrorAction SilentlyContinue |
           Select-Object -First 1 -ExpandProperty FullName
if (-not $ompDll) {   # Build Tools quirk: release libomp lives under debug_nonredist
    $ompDll = Get-ChildItem -Recurse $env:VCToolsRedistDir -Filter 'libomp140.x86_64.dll' |
              Where-Object Name -NotLike '*140d*' | Select-Object -First 1 -ExpandProperty FullName
}

Step "stage -> $Stage"
Remove-Item $Stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $Bin, $Redist | Out-Null

# --- simulators + their applocal DLLs --------------------------------------
foreach ($s in $Simulators) {
    $exe = Join-Path $SimBin "$s.exe"
    if (-not (Test-Path $exe)) { throw "simulator not found: $exe (build it first)" }
    Copy-Item $exe $Bin
}
Get-ChildItem "$SimBin\*.dll" |
    Where-Object Name -NotLike 'boost_unit_test*' |    # test-only dependency
    Copy-Item -Destination $Bin

# --- Intel MPI: launcher, not runtime ---------------------------------------
# The runtime itself is deliberately not copied in - see the -IntelMpi comment
# above. What ships instead is a launcher that finds a pip-installed Intel MPI
# and puts it on PATH for the command it runs, because pip installs to
# %APPDATA%\Python\Library\bin, which Windows does not search. Without that a
# user who has done everything right still gets "the code execution cannot
# proceed because impi.dll was not found", which names the symptom and not the
# cause.
if ($IntelMpi) {
    Step "Intel MPI launcher -> bin\flow-impi.cmd"
    @'
@echo off
rem Run flow with a pip-installed Intel MPI runtime.
rem
rem   python -m pip install --user impi-rt      (no administrator rights needed)
rem
rem pip puts impi.dll in %APPDATA%\Python\Library\bin, which is not on PATH, so
rem put it there for this command only - nothing is installed or changed.
rem
rem   flow-impi.cmd DECK.DATA --output-dir=out
rem   flow-impi.cmd -n 4 DECK.DATA --output-dir=out --threads-per-process=2
rem
rem The -n form runs under mpiexec; without it the simulator runs serially.
setlocal
set "IMPI=%APPDATA%\Python\Library"
if not exist "%IMPI%\bin\impi.dll" (
    echo.
    echo The Intel MPI runtime was not found at:
    echo     %IMPI%\bin
    echo.
    echo Install it once, no administrator rights needed:
    echo     python -m pip install --user impi-rt
    echo.
    exit /b 1
)
set "PATH=%IMPI%\bin;%PATH%"
set "HERE=%~dp0"

if /i not "%~1"=="-n" goto serial

rem Parallel: split off "-n <ranks>" and pass everything after it through
rem untouched.
rem
rem Not with shift and %1: batch treats '=' as an argument delimiter, so
rem --output-dir=C:\out arrives as two arguments and any attempt to rejoin
rem them loses the '='. flow then reports "OutputDir is missing a value".
rem for /f does not split on '=', and "tokens=1,2*" hands the whole remainder
rem back as one piece, quoting and all. %* is used rather than shift because
rem shift does not affect it.
for /f "tokens=1,2*" %%a in ("%*") do set "RANKS=%%b" & set "REST=%%c"
if not defined REST (
    echo No deck given. Usage: flow-impi.cmd -n RANKS DECK.DATA [options]
    exit /b 1
)
call "%IMPI%\bin\mpiexec.exe" -n %RANKS% "%HERE%flow.exe" %REST%
goto done

:serial
call "%HERE%flow.exe" %*

:done
endlocal
'@ | Set-Content -Encoding ascii (Join-Path $Bin 'flow-impi.cmd')
}

# --- MSVC CRT + OpenMP runtime (app-local; see PACKAGING.md on licensing) ---
Copy-Item "$crtDir\*.dll" $Bin
if ($ompDll) { Copy-Item $ompDll $Bin } else { Write-Warning "libomp140.x86_64.dll not found - OpenMP runs need it" }

# --- flow-gui + Qt runtime (windeployqt output) ---------------------------
if (Test-Path (Join-Path $GuiBin 'flow-gui.exe')) {
    Copy-Item (Join-Path $GuiBin 'flow-gui.exe') $Bin
    Copy-Item (Join-Path $GuiBin 'Qt6*.dll') $Bin
    foreach ($d in 'platforms','styles','imageformats','generic','iconengines',
                   'networkinformation','tls') {
        $p = Join-Path $GuiBin $d
        if (Test-Path $p) { Copy-Item $p $Bin -Recurse }
    }
} else {
    Write-Warning "flow-gui.exe not found in $GuiBin - GUI not packaged"
}

# --- prerequisite runtime installers ------------------------------------------
Step "download prerequisite installers (cached in redist\)"
$dl = @(
    @{ Name = 'vc_redist.x64.exe'
       Url  = 'https://aka.ms/vs/17/release/vc_redist.x64.exe' }
)
# MS-MPI ships its installer here because installing it needs administrator
# rights; the Intel MPI variant has no installer to bundle, since pip puts that
# runtime in the user's own profile.
if (-not $IntelMpi) {
    $dl += @{ Name = 'msmpisetup.exe'
              Url  = 'https://download.microsoft.com/download/7/2/7/72731ebb-b63c-4170-ade7-836966263a8f/msmpisetup.exe' }
}
foreach ($d in $dl) {
    $dst = Join-Path $Redist $d.Name
    $cache = Join-Path $Root "dist\_cache\$($d.Name)"
    if (Test-Path $cache) { Copy-Item $cache $dst; continue }
    Write-Host "  downloading $($d.Name) ..."
    New-Item -ItemType Directory -Force -Path (Join-Path $Root 'dist\_cache') | Out-Null
    Invoke-WebRequest -Uri $d.Url -OutFile $cache -UseBasicParsing
    Copy-Item $cache $dst
}

# --- README + LICENSE ------------------------------------------------------------
$mpiSection = if ($IntelMpi) { @"
Prerequisite: Intel(R) MPI - installs without administrator rights
  This build uses the Intel MPI Library. Install its runtime once, as
  yourself - no administrator, no service, no system-wide change:

      python -m pip install --user impi-rt

  (Python from python.org or the Microsoft Store also installs per-user.)
  Then run the simulator through the launcher in bin\, which finds that
  runtime and puts it on PATH for the command:

      bin\flow-impi.cmd DECK.DATA --output-dir=out
      bin\flow-impi.cmd -n 4 DECK.DATA --output-dir=out --threads-per-process=2

  flow-gui finds it the same way, so the GUI needs no extra setup.
  Running bin\flow.exe directly will fail with a missing impi.dll unless you
  put %APPDATA%\Python\Library\bin on PATH yourself.

  Why not bundled: OPM Flow is GPLv3 and the Intel MPI runtime is proprietary,
  so shipping them together is a licensing question this package does not try
  to answer. Installing it yourself avoids it entirely.

  The Visual C++ and OpenMP runtimes ARE inside bin\, so no VC++ install is
  needed; redist\vc_redist.x64.exe is included only as a fallback.
"@ } else { @"
Prerequisite: Microsoft MPI
  The simulator links msmpi.dll (needed even for serial runs). It is a Windows
  system component and is NOT bundled, so if MS-MPI is not already installed:
      run  redist\msmpisetup.exe  once      (or:  winget install Microsoft.msmpi)
  The Visual C++ and OpenMP runtimes ship inside bin\, so no VC++ install is
  needed; redist\vc_redist.x64.exe is included only as a fallback.
"@ }

@"
OPM Flow for Windows - $Version
================================

UNOFFICIAL BUILD. OPM Flow is developed by the Open Porous Media initiative
(https://opm-project.org). This is an independent Windows build of it, not a
release of the OPM project, and the OPM project does not support it. Report
problems with this package at
    https://github.com/GitPaean/opm_flow_windows/issues
and not to OPM - unless the same thing happens on Linux, in which case it is
OPM's to hear about.

Contents
  bin\flow.exe            reservoir simulator (all model variants,
                          including black-oil)
  bin\flow-gui.exe     graphical front end (job queue, live log)
                          Optional: flow.exe is a complete simulator on its
                          own, so bin\flow-gui.exe, bin\Qt6*.dll and the Qt
                          plugin folders (platforms, styles, imageformats,
                          generic, iconengines, networkinformation, tls) can
                          be deleted if only the simulator is wanted. The
                          installer offers the same choice as a component.
$(if ($IntelMpi) { "  bin\flow-impi.cmd       runs flow with a pip-installed Intel MPI`n" })  redist\                 $(if ($IntelMpi) { 'Visual C++ runtime installer (fallback only)' }
                            else            { 'Microsoft runtime installers (see prerequisite below)' })

$mpiSection

Running
  GUI:       double-click bin\flow-gui.exe, add a *.DATA deck, Run.
  Terminal:  $(if ($IntelMpi) { 'bin\flow-impi.cmd DECK.DATA --output-dir=RESULTS' }
               else            { 'bin\flow.exe  DECK.DATA  --output-dir=RESULTS' })
  Parallel:  mpiexec -n 4 bin\flow.exe DECK.DATA --threads-per-process=2 ...

Windows Firewall may prompt on the first parallel run - allow access
(private networks) so the MPI ranks can communicate locally.

First run on Windows (SmartScreen)
  These binaries are not code-signed, so Microsoft SmartScreen may warn when
  you download or first launch them. This is a reputation warning, not a virus
  detection. To proceed:
    - Downloading in a browser: if it says "isn't commonly downloaded", open
      the download's "..." menu and choose Keep, then Keep anyway.
    - First launch: if "Windows protected your PC" appears, click "More info"
      then "Run anyway".

License and source code
  OPM Flow is free software under the GNU GPL v3+ (see LICENSE.txt).
"@ | Set-Content -Encoding utf8 (Join-Path $Stage 'README.txt')

# --- exact source revisions this package was built from ---------------------
# GPLv3 requires the *corresponding* source, so naming a branch is not enough:
# a branch moves, and then the statement below points at code these binaries
# were never built from. Record the commit each module was actually built at,
# and a tag if one points there. Branch names are given only for orientation.
function Source-Revision {
    param([string]$Module)
    $dir = Join-Path $Root "src\$Module"
    if (-not (Test-Path (Join-Path $dir '.git'))) { return "(source tree not present at package time)" }
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $sha    = (& git -C $dir rev-parse HEAD 2>$null)
    $branch = (& git -C $dir rev-parse --abbrev-ref HEAD 2>$null)
    $tag    = (& git -C $dir describe --tags --exact-match HEAD 2>$null)
    $dirty  = (& git -C $dir status --porcelain 2>$null)
    $ErrorActionPreference = $prev
    if (-not $sha) { return "(revision could not be determined)" }
    $out = "commit $sha"
    if ($tag)    { $out += " (tag $tag)" }
    elseif ($branch -and $branch -ne 'HEAD') { $out += " (on branch $branch at build time)" }
    if ($dirty)  { $out += " + UNCOMMITTED LOCAL CHANGES" }
    return $out
}
$revCommon     = Source-Revision 'opm-common'
$revGrid       = Source-Revision 'opm-grid'
$revSimulators = Source-Revision 'opm-simulators'
$prevEA = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
$revHarness = (& git -C $Root rev-parse HEAD 2>$null)
$ErrorActionPreference = $prevEA
if (-not $revHarness) { $revHarness = '(revision could not be determined)' } else { $revHarness = "commit $revHarness" }
foreach ($r in $revCommon, $revGrid, $revSimulators) {
    if ($r -match 'UNCOMMITTED') {
        Write-Warning "Source tree has uncommitted changes - the LICENSE.txt source statement cannot be satisfied by any published commit. Commit and push before releasing."
    }
}

@"
OPM Flow is developed by the Open Porous Media (OPM) initiative
  https://opm-project.org        https://github.com/OPM
and is Copyright (C) the OPM project contributors, licensed under the
GNU General Public License, version 3 or later
(https://www.gnu.org/licenses/gpl-3.0.html).

The Windows build harness and the flow-gui application in this package
are Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics, under the same GPL v3+ license.

Source code
  The project's home is the upstream OPM repositories:
    https://github.com/OPM/opm-common
    https://github.com/OPM/opm-grid
    https://github.com/OPM/opm-simulators
  These Windows binaries additionally contain a small set of
  Windows/MSVC-specific patches, maintained in the forks below. The
  complete corresponding source for exactly these binaries (upstream
  code + those patches + the build harness and GUI) is the following
  revisions. These are commit ids, not branch names: branches move on,
  and a moved branch would no longer be the source of these binaries.
    https://github.com/GitPaean/opm_flow_windows
      $revHarness
    https://github.com/GitPaean/opm-common
      $revCommon
    https://github.com/GitPaean/opm-grid
      $revGrid
    https://github.com/GitPaean/opm-simulators
      $revSimulators
  Fetch one with, for example:
    git clone https://github.com/GitPaean/opm-common && cd opm-common
    git checkout <commit id above>

Third-party redistributables in this package: Microsoft Visual C++
runtime$(if($IntelMpi){''}else{' and Microsoft MPI'}) (their own licenses apply); Qt 6 (LGPLv3,
dynamically linked - relink is possible by replacing the Qt DLLs);
OpenBLAS, SuiteSparse, Boost and fmt under their respective licenses.
$(if ($IntelMpi) { @"

No MPI library is redistributed in this package. This build links the
Intel(R) MPI Library, which you install yourself (see README.txt); it is
Intel's software under Intel's own terms and is no part of this
distribution.
"@ })
"@ | Set-Content -Encoding utf8 (Join-Path $Stage 'LICENSE.txt')

Step "staged package summary"
$exes = (Get-ChildItem "$Bin\*.exe").Count
$dlls = (Get-ChildItem "$Bin\*.dll").Count
$size = [math]::Round(((Get-ChildItem $Stage -Recurse | Measure-Object Length -Sum).Sum/1MB),1)
Write-Host "  $exes exes, $dlls DLLs, total $size MB"

if ($Zip) {
    Step "zip"
    $zipPath = Join-Path $Root "dist\opm-flow-$Version-win64.zip"
    Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path $Stage -DestinationPath $zipPath
    Write-Host "  $zipPath ($([math]::Round((Get-Item $zipPath).Length/1MB,1)) MB)"
}
Write-Host "done." -ForegroundColor Green
