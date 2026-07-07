<#
  package-flow.ps1 - stage a redistributable Windows package of OPM Flow
  (simulator + flow-gui-qt) from this harness' build trees.

  Produces  dist\opm-flow-<Version>\
    bin\        flow*.exe, their runtime DLLs (vcpkg applocal set), the MSVC
                CRT + OpenMP runtime, flow-gui-qt.exe + Qt runtime/plugins
    redist\     vc_redist.x64.exe + msmpisetup.exe (downloaded on demand)
    README.txt  install / run instructions
    LICENSE.txt GPLv3 notice + source availability statement

  Options:
    -Version    package version string          (default 2026.10-pre)
    -Simulators simulator exes to include       (default flow, flow_blackoil)
    -Zip        also produce dist\opm-flow-<Version>-win64.zip

  The staged tree is what installer\opm-flow.iss (Inno Setup) and
  packaging\build-msix.ps1 (MSIX) package further. See PACKAGING.md.
#>
[CmdletBinding()]
param(
    [string]  $Version    = '2026.10-pre',
    [string[]]$Simulators = @('flow', 'flow_blackoil'),
    [switch]  $Zip
)

$ErrorActionPreference = 'Stop'
$Root  = Split-Path -Parent $MyInvocation.MyCommand.Path
$SimBin = Join-Path $Root 'build-mpi\opm-simulators\bin'
$GuiBin = Join-Path $Root 'build-gui-qt'
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

# --- flow-gui-qt + Qt runtime (windeployqt output) ---------------------------
if (Test-Path (Join-Path $GuiBin 'flow-gui-qt.exe')) {
    Copy-Item (Join-Path $GuiBin 'flow-gui-qt.exe') $Bin
    Copy-Item (Join-Path $GuiBin 'Qt6*.dll') $Bin
    foreach ($d in 'platforms','styles','imageformats','generic','iconengines',
                   'networkinformation','tls') {
        $p = Join-Path $GuiBin $d
        if (Test-Path $p) { Copy-Item $p $Bin -Recurse }
    }
} else {
    Write-Warning "flow-gui-qt.exe not found in $GuiBin - GUI not packaged"
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
  bin\flow.exe            reservoir simulator (all model variants)
  bin\flow_blackoil.exe   black-oil-only simulator (smaller/faster to load)
  bin\flow-gui-qt.exe     graphical front end (job queue, live log)
  redist\                 prerequisite runtime installers

One-time prerequisites (skip any already installed)
  1. redist\vc_redist.x64.exe     Microsoft Visual C++ runtime
  2. redist\msmpisetup.exe        Microsoft MPI runtime (needed even for
                                  serial runs: the simulators link msmpi.dll)

Running
  GUI:       double-click bin\flow-gui-qt.exe, add a *.DATA deck, Run.
  Terminal:  bin\flow.exe  DECK.DATA  --output-dir=RESULTS
  Parallel:  mpiexec -n 4 bin\flow.exe DECK.DATA --threads-per-process=2 ...

Windows Firewall may prompt on the first parallel run - allow access
(private networks) so the MPI ranks can communicate locally.

License and source code
  OPM Flow is free software under the GNU GPL v3+ (see LICENSE.txt).
"@ | Set-Content -Encoding utf8 (Join-Path $Stage 'README.txt')

@"
OPM Flow is Copyright (C) the Open Porous Media project contributors,
licensed under the GNU General Public License, version 3 or later
(https://www.gnu.org/licenses/gpl-3.0.html).

This binary distribution was produced with the opm_flow_windows build
harness. Complete corresponding source code is available at:
  https://github.com/GitPaean/opm_flow_windows        (build harness + GUI)
  https://github.com/GitPaean/opm-common               (branch: windows)
  https://github.com/GitPaean/opm-grid                 (branch: windows)
  https://github.com/GitPaean/opm-simulators           (branch: windows)
  https://github.com/OPM                               (upstream project)

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
