<#
  build-optests.ps1 - reconfigure + build the four OPM modules with tests AND
  examples enabled (BUILD_TESTING=ON, BUILD_EXAMPLES=ON), building the 'all'
  target with ninja keep-going so every buildable target compiles even if some
  test/example targets are not yet MSVC-clean. Operates in the existing
  build-mpi\ / install-mpi\ trees (incremental on top of the validated build).
#>
$ErrorActionPreference = 'Continue'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root
. (Join-Path $Root 'setup-env.ps1')

$bm = Join-Path $Root 'build-module.ps1'
$modules = 'opm-common','opm-grid','opm-simulators','opm-upscaling'
$results = @()

foreach ($m in $modules) {
    $mlog = Join-Path $Root "build-tests-$m.log"
    Write-Host "`n########## REBUILD $m (BUILD_TESTING=ON BUILD_EXAMPLES=ON) ##########" -ForegroundColor Magenta
    $ok = $true; $err = ''
    try {
        & $bm $m -Mpi -OpenMP -Target all -Extra '-DBUILD_TESTING=ON','-DBUILD_EXAMPLES=ON' *>&1 |
            Tee-Object -FilePath $mlog
    } catch {
        $ok = $false; $err = "$_"
        Write-Host "  -> $m did not fully build: $err" -ForegroundColor Yellow
    }
    $results += [pscustomobject]@{ Module = $m; FullyOk = $ok }
}

Write-Host "`n===================== REBUILD SUMMARY =====================" -ForegroundColor Cyan
foreach ($r in $results) {
    $state = if ($r.FullyOk) { 'all targets built' } else { 'partial (some targets failed)' }
    Write-Host ("  {0,-16} {1}" -f $r.Module, $state)
}
Write-Host "==== build-optests.ps1 done ===="
