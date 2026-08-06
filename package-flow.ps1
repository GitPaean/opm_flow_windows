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
    [switch]  $Zip
)

$ErrorActionPreference = 'Stop'
$Root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$SimBin = Join-Path $Root 'build-mpi\opm-simulators\bin'
$GuiBin = Join-Path $Root 'build-gui'
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
       Url  = 'https://aka.ms/vs/17/release/vc_redist.x64.exe' },
    @{ Name = 'msmpisetup.exe'
       Url  = 'https://download.microsoft.com/download/7/2/7/72731ebb-b63c-4170-ade7-836966263a8f/msmpisetup.exe' }
)
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
@"
OPM Flow for Windows - $Version
================================

Contents
  bin\flow.exe            reservoir simulator (all model variants,
                          including black-oil)
  bin\flow-gui.exe     graphical front end (job queue, live log)
  redist\                 Microsoft runtime installers (see prerequisite below)

Prerequisite: Microsoft MPI
  The simulator links msmpi.dll (needed even for serial runs). It is a Windows
  system component and is NOT bundled, so if MS-MPI is not already installed:
      run  redist\msmpisetup.exe  once      (or:  winget install Microsoft.msmpi)
  The Visual C++ and OpenMP runtimes ship inside bin\, so no VC++ install is
  needed; redist\vc_redist.x64.exe is included only as a fallback.

Running
  GUI:       double-click bin\flow-gui.exe, add a *.DATA deck, Run.
  Terminal:  bin\flow.exe  DECK.DATA  --output-dir=RESULTS
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
  Windows/MSVC-specific patches that are pending upstream merge. The
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
  Once the patches are merged upstream, the OPM repositories alone are
  the source.

Third-party redistributables in this package: Microsoft Visual C++
runtime and Microsoft MPI (their own licenses apply); Qt 6 (LGPLv3,
dynamically linked - relink is possible by replacing the Qt DLLs);
OpenBLAS, SuiteSparse, Boost and fmt under their respective licenses.
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
