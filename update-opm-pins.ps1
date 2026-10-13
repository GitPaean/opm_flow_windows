# Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
<#
  update-opm-pins.ps1 - pin the sources the harness builds.

  build-all.ps1 checks the OPM modules out at the commits opm-sources.json
  names, not at the heads of the forks' 'windows' branches. The heads move with
  every rebase of the series, and in between they can be anything from a
  rebase in progress to a state that no longer builds against the other
  modules. The pins are a set that was built and run together. vcpkg - its
  ports decide every dependency's version - and Trilinos, where Zoltan comes
  from, are pinned in the same file, so that a new clone of the harness builds
  what was built here.

  After rebasing the series and checking the result (docs/release-checklist.md
  section 1), with src\opm-* at the commits that were checked, this writes them
  to opm-sources.json; commit that file with the harness. Each commit must be
  clean and on the fork's branch on GitHub: a pin has to be something anyone
  can fetch. Once pinned, a commit can be rebased off the branch - GitHub still
  serves it by id. The vcpkg and Trilinos pins are kept as they are unless
  -Dependencies is given: moving them is a separate decision, made after a
  build against the newer ones.

  Usage:
    .\update-opm-pins.ps1                 # pin what src\opm-* holds now
    .\update-opm-pins.ps1 -Dependencies   # ... and what vcpkg\, deps\Trilinos hold
    .\update-opm-pins.ps1 -Check          # exit 1 when anything is not at its pin
#>
[CmdletBinding()]
param(
    # Only compare the checkouts with the pins; change nothing.
    [switch]$Check,
    # Also pin vcpkg\ and deps\Trilinos as they are checked out now.
    [switch]$Dependencies,
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

# The dependencies pinned beside the OPM modules: key in the file, checkout,
# and the repository a new clone takes them from.
$depDefs = @(
    [pscustomobject]@{ Key = 'vcpkg';    Dir = 'vcpkg';         Url = 'https://github.com/microsoft/vcpkg.git' },
    [pscustomobject]@{ Key = 'trilinos'; Dir = 'deps\Trilinos'; Url = 'https://github.com/trilinos/Trilinos.git' }
)

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
$carried = $false
# Every module the file pins is written back: those not asked for, or not
# cloned here (opm-upscaling is built only with -Upscaling), keep their pins.
$all = @()
if ($lock) { $all += @($lock.modules.PSObject.Properties.Name) }
$all += @($Modules | Where-Object { $all -notcontains $_ })
foreach ($m in $all) {
    $dir = Join-Path $Root "src\$m"
    $old = $null
    if ($lock -and $lock.modules.$m) { $old = $lock.modules.$m.commit }
    $cloned = Test-Path (Join-Path $dir '.git')
    if (($Modules -notcontains $m) -or -not $cloned) {
        if (-not $old) { throw "src\$m is not a git checkout" }
        if ($Check) {
            '{0,-15} not cloned (pinned {1})' -f $m, (Short $old)
        } else {
            if ($Modules -contains $m) { Write-Warning "src\$m is not cloned here: its pin is kept as it is" }
            $rows += [pscustomobject]@{ Module = $m; Commit = $old; Upstream = $lock.modules.$m.upstream; Date = $null; Since = $null; Old = $old; OnHead = $null }
            $carried = $true
        }
        continue
    }
    $head = Invoke-Git $dir rev-parse HEAD

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

# vcpkg and Trilinos: compared under -Check, re-pinned under -Dependencies,
# otherwise carried over as they are.
$deps = @()
foreach ($d in $depDefs) {
    $dir  = Join-Path $Root $d.Dir
    $old  = $null
    if ($lock -and $lock.($d.Key)) { $old = $lock.($d.Key) }
    $head = $null
    if (Test-Path (Join-Path $dir '.git')) { $head = Invoke-Git $dir rev-parse HEAD }

    if ($Check) {
        if (-not $old) {
            '{0,-15} not pinned' -f $d.Key
        } elseif (-not $head) {
            '{0,-15} not cloned (pinned {1})' -f $d.Key, (Short $old.commit)
        } elseif ($head -eq $old.commit) {
            '{0,-15} at the pin {1}' -f $d.Key, (Short $head)
        } else {
            $mismatch++
            '{0,-15} at {1}, pinned {2}' -f $d.Key, (Short $head), (Short $old.commit)
        }
        continue
    }

    if ($Dependencies -and $head) {
        if (Invoke-Git $dir status --porcelain --untracked-files=no) {
            throw "$($d.Dir) has changes to tracked files: a pin must name what upstream serves"
        }
        # GitHub must serve the commit. --depth only where the clone is shallow
        # already: in a full clone it would make it shallow.
        $shallow = (Invoke-Git $dir rev-parse --is-shallow-repository) -eq 'true'
        if ($shallow) {
            Invoke-Git $dir fetch --quiet --depth 1 $d.Url $head | Out-Null
        } else {
            Invoke-Git $dir fetch --quiet $d.Url $head | Out-Null
        }
        $deps += [pscustomobject]@{ Key = $d.Key; Commit = $head; Date = Invoke-Git $dir log -1 --format=%cs $head; Old = $old }
    } elseif ($old) {
        $deps += [pscustomobject]@{ Key = $d.Key; Commit = $old.commit; Date = $old.date; Old = $old }
    } elseif ($Dependencies) {
        Write-Warning "$($d.Dir) is not cloned, so $($d.Key) stays unpinned"
    }
}

# The README's quick start says which OPM snapshot a build gets, between these
# markers; this script keeps it in step with the pins.
$readme  = Join-Path $Root 'README.md'
$marker  = '(<!-- opm-snapshot -->)[^<]*(<!-- /opm-snapshot -->)'

if ($Check) {
    if ($lock -and (Test-Path $readme)) {
        $hit  = [regex]::Match([IO.File]::ReadAllText($readme), $marker)
        $said = if ($hit.Success) { $hit.Value -replace '<!-- /?opm-snapshot -->', '' } else { $null }
        if ($said -ne $lock.snapshot) {
            Write-Warning "README.md names the snapshot '$said', opm-sources.json '$($lock.snapshot)': run update-opm-pins.ps1"
        }
    }
    if ($mismatch -gt 0) {
        Write-Warning "$mismatch checkout(s) not at the commits opm-sources.json pins"
        exit 1
    }
    exit 0
}

foreach ($r in $rows) {
    if (-not $r.Date) {
        '{0,-15} {1} (kept: not pinned anew here)' -f $r.Module, (Short $r.Commit)
        continue
    }
    $note = if ($r.Old -eq $r.Commit) { 'unchanged' } else { "was $(Short $r.Old)" }
    $head = if ($r.OnHead) { "head of $OpmBranch" } else { "behind the head of $OpmBranch" }
    '{0,-15} {1} ({2}; {3}) on OPM master {4} of {5}, {6} upstream commit(s) since' -f `
        $r.Module, (Short $r.Commit), $note, $head, (Short $r.Upstream), $r.Date, $r.Since
}
foreach ($d in $deps) {
    $note = if ($d.Old -and $d.Old.commit -eq $d.Commit) { 'unchanged' } elseif ($d.Old) { "was $(Short $d.Old.commit)" } else { 'new' }
    '{0,-15} {1} ({2}) of {3}' -f $d.Key, (Short $d.Commit), $note, $d.Date
}

# The snapshot is dated by the newest upstream commit in the set, as the
# release packages are. A kept pin's date is not known here, but the file's
# old snapshot is no older than it.
$dates = @($rows | Where-Object { $_.Date } | ForEach-Object { $_.Date })
if ($carried -and $lock) { $dates += $lock.snapshot }
$snapshot = $dates | Sort-Object -Descending | Select-Object -First 1

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
$close = if ($deps.Count -gt 0) { '  },' } else { '  }' }
$lines += $close
for ($i = 0; $i -lt $deps.Count; $i++) {
    $d = $deps[$i]
    $comma = if ($i -lt $deps.Count - 1) { ',' } else { '' }
    $lines += "  `"$($d.Key)`": {"
    $lines += "    `"commit`": `"$($d.Commit)`","
    $lines += "    `"date`": `"$($d.Date)`""
    $lines += "  }$comma"
}
$lines += '}'
$text = ($lines -join $nl) + $nl

$current = if (Test-Path $pinFile) { [IO.File]::ReadAllText($pinFile) -replace "`r`n", "`n" } else { '' }
if ($text -eq $current) {
    Write-Host "opm-sources.json already pins these commits (snapshot $snapshot)."
} else {
    [IO.File]::WriteAllText($pinFile, $text, (New-Object Text.UTF8Encoding($false)))
    Write-Host "Wrote opm-sources.json (snapshot $snapshot). Commit it with the harness."
}

if (Test-Path $readme) {
    $doc = [IO.File]::ReadAllText($readme)
    if (-not [regex]::IsMatch($doc, $marker)) {
        Write-Warning "README.md has no <!-- opm-snapshot --> marker: say by hand which snapshot it builds"
    } else {
        $new = [regex]::Replace($doc, $marker, "`${1}$snapshot`${2}")
        if ($new -ne $doc) {
            [IO.File]::WriteAllText($readme, $new, (New-Object Text.UTF8Encoding($false)))
            Write-Host "Updated the snapshot date README.md gives. Commit it too."
        }
    }
}
