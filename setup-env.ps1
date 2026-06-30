# setup-env.ps1
# Dot-source this script to load the MSVC x64 build environment + vcpkg into the
# current PowerShell session:   . .\setup-env.ps1
#
# It discovers Visual Studio via vswhere, imports the vcvars64 environment, and
# exports VCPKG_ROOT / toolchain paths used by the OPM build steps.

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$env:OPM_ROOT = $Root

# --- locate Visual Studio with the C++ x64 toolset ---
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found - is Visual Studio installed?" }

# Prefer the newest install that has the x64 C++ toolset.
$vsPath = & $vswhere -products * -latest `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) { throw "No VS install with the C++ x64 toolset found." }

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

# --- import the vcvars64 environment into this session ---
# Run vcvars in cmd, then dump the resulting environment and re-apply it here.
$tmp = [System.IO.Path]::GetTempFileName()
cmd /c "`"$vcvars`" > nul 2>&1 && set" > $tmp
Get-Content $tmp | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') {
        Set-Item -Path "Env:\$($matches[1])" -Value $matches[2]
    }
}
Remove-Item $tmp -Force

# Set vcpkg AFTER vcvars (vcvars64 sets VCPKG_ROOT to VS's bundled copy - override it).
$env:VCPKG_ROOT = Join-Path $Root 'vcpkg'
$env:VCPKG_TOOLCHAIN = Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'

# Import MS-MPI env vars (MSMPI_INC / MSMPI_LIB64 / MSMPI_BIN) from the machine
# registry. The MS-MPI SDK installer sets these machine-wide, but shells that
# were already running before the install do not see them; CMake's FindMPI needs
# MSMPI_INC/MSMPI_LIB64 to locate Microsoft MPI. Also put mpiexec on PATH.
foreach ($v in 'MSMPI_INC','MSMPI_LIB64','MSMPI_LIB32','MSMPI_BIN') {
    if (-not [Environment]::GetEnvironmentVariable($v, 'Process')) {
        $val = [Environment]::GetEnvironmentVariable($v, 'Machine')
        if ($val) { Set-Item -Path "Env:\$v" -Value $val }
    }
}
if ($env:MSMPI_BIN -and (Test-Path $env:MSMPI_BIN) -and ($env:Path -notlike "*$($env:MSMPI_BIN)*")) {
    $env:Path = "$env:MSMPI_BIN;$env:Path"
}

# Restore normal error handling now (critical setup with explicit throws is done).
# Leaving 'Stop' makes native tools (cl/cmake/git) that write to stderr raise
# spurious NativeCommandError terminating errors.
$ErrorActionPreference = 'Continue'

Write-Host "[setup-env] VS:     $vsPath"
Write-Host "[setup-env] vcpkg:  $env:VCPKG_ROOT"
$cl = (Get-Command cl -ErrorAction SilentlyContinue)
if ($cl) {
    $ver = (& cl 2>&1 | Select-Object -First 1)
    Write-Host "[setup-env] cl:     $($cl.Source)"
    Write-Host "[setup-env] $ver"
} else {
    Write-Warning "cl.exe not on PATH after vcvars - check the toolset install."
}
