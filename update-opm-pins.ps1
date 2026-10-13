# Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
<#
  update-opm-pins.ps1 - pin the OPM commits the harness builds.

  build-all.ps1 checks the OPM modules out at the commits opm-sources.json
  names, not at the heads of the forks' 'windows' branches. The heads move with
  every rebase of the series, and in between they can be anything from a
  rebase in progress to a state that no longer builds against the other
  modules. The pins are a set that was built and run together.

  After rebasing the series and checking the result (docs/release-checklist.md
  section 1), with src\opm-* at the commits that were checked, this writes them
  to opm-sources.json; commit that file with the harness. Each commit must be
  clean and on the fork's branch on GitHub: a pin has to be something anyone
  can fetch. Once pinned, a commit can be rebased off the branch - GitHub still
  serves it by id.

  Usage:
    .\update-opm-pins.ps1            # pin what src\opm-* holds now
    .\update-opm-pins.ps1 -Check     # exit 1 when src\opm-* is not at the pins
#>
[CmdletBinding()]
param(
    # Only compare src\opm-* with the pins; change nothing.
    [switch]$Check,
    # The modules to pin. Default: those opm-sources.json pins already; for a
    # new file, the three flow needs, plus opm-upscaling when it is cloned.
    [string[]]$Modules,
    # The fork the pins refer to. Default: the file's own, else build-all.ps1's
    # defaults, GitPaean / windows.
    [string]$OpmOrg,
    [string]$OpmBranch
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$pinFile = Join-Path $Root 'opm-sources.json'

# 'powershell -File' passes a comma list as one string.
$Modules = @($Modules | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

$lock = $null
if (Test-Path $pinFile) { $lock = Get-Content -Raw $pinFile | ConvertFrom-Json }
if (-not $OpmOrg)    { $OpmOrg    = if ($lock) { $lock.org }    else { 'GitPaean' } }
if (-not $OpmBranch) { $OpmBranch = if ($lock) { $lock.branch } else { 'windows' } }
if ($Modules.Count -eq 0) {
    if ($lock) {
        $Modules = @($lock.modules.PSObject.Properties.Name)
    } else {
        $Modules = @('opm-common', 'opm-grid', 'opm-simulators')
        if (Test-Path (Join-Path $Root 'src\opm-upscaling\.git')) { $Modules += 'opm-upscaling' }
    }
}

# Invoke-Git <dir> <git arguments...>: git's trimmed output; throws when it fails.
# A plain function, so that arguments such as --quiet reach git untouched.
function Invoke-Git {
    $dir  = $args[0]
    $rest = @($args | Select-Object -Skip 1)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out  = & git -C $dir @rest 2>$null
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($code -ne 0) { throw "git $($rest -join ' ') failed in $dir (exit $code)" }
    return "$out".Trim()
}

# Test-Git <dir> <git arguments...>: whether git exits 0.
function Test-Git {
    $dir  = $args[0]
    $rest = @($args | Select-Object -Skip 1)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & git -C $dir @rest 2>$null | Out-Null
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    return ($code -eq 0)
}

function Short([string]$Sha) { if ($Sha) { $Sha.Substring(0, 10) } else { '(none)' } }

$rows = @()
$mismatch = 0
foreach ($m in $Modules) {
    $dir = Join-Path $Root "src\$m"
    if (-not (Test-Path (Join-Path $dir '.git'))) { throw "src\$m is not a git checkout" }
    $head = Invoke-Git $dir rev-parse HEAD
    $old  = $null
    if ($lock -and $lock.modules.$m) { $old = $lock.modules.$m.commit }

    if ($Check) {
        if ($head -eq $old) {
            '{0,-15} at the pin {1}' -f $m, (Short $head)
        } else {
            $mismatch++
            '{0,-15} at {1}, pinned {2}' -f $m, (Short $head), (Short $old)
        }
        continue
    }

    # Changes to tracked files would make the pin name a tree that was not built.
    if (Invoke-Git $dir status --porcelain --untracked-files=no) {
        throw "src\$m has uncommitted changes: commit and push them, or check out a clean commit"
    }

    # Anyone must be able to fetch the pin, so it has to be on the fork's branch.
    Invoke-Git $dir fetch --quiet "https://github.com/$OpmOrg/$m.git" $OpmBranch | Out-Null
    $branchHead = Invoke-Git $dir rev-parse FETCH_HEAD
    if (-not (Test-Git $dir merge-base --is-ancestor $head $branchHead)) {
        throw "src\$m is at $head, which is not on $OpmOrg/$m ${OpmBranch}: push it first"
    }

    # The upstream commit the series sits on: the snapshot is dated by it.
    Invoke-Git $dir fetch --quiet "https://github.com/OPM/$m.git" master | Out-Null
    $upHead = Invoke-Git $dir rev-parse FETCH_HEAD
    $base   = Invoke-Git $dir merge-base $head $upHead

    $rows += [pscustomobject]@{
        Module   = $m
        Commit   = $head
        Upstream = $base
        Date     = Invoke-Git $dir log -1 --format=%cs $base
        Since    = Invoke-Git $dir rev-list --count "$base..$upHead"
        Old      = $old
        OnHead   = ($head -eq $branchHead)
    }
}

if ($Check) {
    if ($mismatch -gt 0) {
        Write-Warning "$mismatch module(s) not at the commits opm-sources.json pins"
        exit 1
    }
    exit 0
}

foreach ($r in $rows) {
    $note = if ($r.Old -eq $r.Commit) { 'unchanged' } else { "was $(Short $r.Old)" }
    $head = if ($r.OnHead) { "head of $OpmBranch" } else { "behind the head of $OpmBranch" }
    '{0,-15} {1} ({2}; {3}) on OPM master {4} of {5}, {6} upstream commit(s) since' -f `
        $r.Module, (Short $r.Commit), $note, $head, (Short $r.Upstream), $r.Date, $r.Since
}

# The snapshot is dated by the newest upstream commit in the set, as the
# release packages are.
$snapshot = ($rows | Sort-Object Date -Descending | Select-Object -First 1).Date

# LF, as the repository stores it (*.json is text in .gitattributes, so git
# normalizes whichever ending a checkout has).
$nl = "`n"
$lines = @('{', "  `"snapshot`": `"$snapshot`",", "  `"org`": `"$OpmOrg`",",
           "  `"branch`": `"$OpmBranch`",", '  "modules": {')
for ($i = 0; $i -lt $rows.Count; $i++) {
    $r = $rows[$i]
    $comma = if ($i -lt $rows.Count - 1) { ',' } else { '' }
    $lines += "    `"$($r.Module)`": {"
    $lines += "      `"commit`": `"$($r.Commit)`","
    $lines += "      `"upstream`": `"$($r.Upstream)`""
    $lines += "    }$comma"
}
$lines += '  }', '}'
$text = ($lines -join $nl) + $nl

$current = if (Test-Path $pinFile) { [IO.File]::ReadAllText($pinFile) -replace "`r`n", "`n" } else { '' }
if ($text -eq $current) {
    Write-Host "opm-sources.json already pins these commits (snapshot $snapshot)."
} else {
    [IO.File]::WriteAllText($pinFile, $text, (New-Object Text.UTF8Encoding($false)))
    Write-Host "Wrote opm-sources.json (snapshot $snapshot). Commit it with the harness."
}
