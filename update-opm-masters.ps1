<#
  update-opm-masters.ps1 - sync each OPM fork's master with upstream/master.

  For every module under src\, on its 'master' branch, runs:
      git checkout master
      git remote update            # fetch origin + upstream
      git merge upstream/master
      git push origin master

  Safety:
    - Skips a repo (with a warning) if its working tree is dirty, so local
      edits are never disturbed.
    - Stops a repo's sequence on the first failing step, so a bad/conflicted
      state is never pushed. Other repos still get processed.
    - Prints a summary table and the final branch each repo is left on.

  Usage:
      .\update-opm-masters.ps1                 # exactly the 4 steps above
      .\update-opm-masters.ps1 -FfOnly         # merge with --ff-only (recommended
                                               #   for a fork sync: fails loudly if
                                               #   master has diverged instead of
                                               #   silently making a merge commit)
      .\update-opm-masters.ps1 -RestoreBranch  # return each repo to the branch it
                                               #   was on before (e.g. 'windows')
      .\update-opm-masters.ps1 -Modules opm-grid,opm-common
#>
[CmdletBinding()]
param(
    [string]$Root = $PSScriptRoot,
    [string[]]$Modules = @('opm-common','opm-grid','opm-simulators','opm-upscaling'),
    [switch]$FfOnly,
    [switch]$RestoreBranch
)

$ErrorActionPreference = 'Continue'   # git writes progress to stderr; judge by exit code

# Run a git command in $Dir, echo it, and return $true on exit code 0.
function Invoke-GitStep {
    param([string]$Dir, [Parameter(ValueFromRemainingArguments)] [string[]]$GitArgs)
    Write-Host ("    git " + ($GitArgs -join ' ')) -ForegroundColor DarkGray
    & git -C $Dir @GitArgs
    return ($LASTEXITCODE -eq 0)
}

$mergeArgs = if ($FfOnly) { @('merge','--ff-only','upstream/master') }
             else         { @('merge','upstream/master') }

$results = @()
foreach ($m in $Modules) {
    $d = Join-Path $Root "src\$m"
    Write-Host "`n==== $m ====" -ForegroundColor Cyan

    if (-not (Test-Path (Join-Path $d '.git'))) {
        Write-Warning "  not a git repo at $d - skipping"
        $results += [pscustomobject]@{ Module = $m; Result = 'SKIP (no repo)'; Branch = '-' }
        continue
    }

    # Guard: never touch a repo with uncommitted changes.
    if (git -C $d status --porcelain) {
        Write-Warning "  working tree is dirty - skipping (commit/stash first)"
        $results += [pscustomobject]@{ Module = $m; Result = 'SKIP (dirty)'; Branch = (git -C $d branch --show-current) }
        continue
    }

    $startBranch = git -C $d branch --show-current

    $ok = $true
    if ($ok) { $ok = Invoke-GitStep $d checkout master }
    if ($ok) { $ok = Invoke-GitStep $d remote update }
    if ($ok) { $ok = Invoke-GitStep $d @mergeArgs }
    if ($ok) { $ok = Invoke-GitStep $d push origin master }

    if ($ok -and $RestoreBranch -and $startBranch -and $startBranch -ne 'master') {
        Invoke-GitStep $d checkout $startBranch | Out-Null
    }

    $head   = git -C $d rev-parse --short HEAD
    $branch = git -C $d branch --show-current
    $result = if ($ok) { "OK ($head)" } else { 'FAILED (see output above)' }
    if (-not $ok) { Write-Warning "  ${m}: a step failed - stopped before pushing a bad state" }
    $results += [pscustomobject]@{ Module = $m; Result = $result; Branch = $branch }
}

Write-Host "`n==== summary ====" -ForegroundColor Cyan
$results | Format-Table -AutoSize

if ($results.Result -match 'FAILED') { exit 1 }
