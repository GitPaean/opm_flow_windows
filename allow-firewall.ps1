<#
  allow-firewall.ps1 - pre-authorize the built OPM executables in Windows
  Defender Firewall so MPI runs do not trigger per-exe popups (or silent
  Block rules if a popup was ever cancelled).

  - Adds inbound Allow rules (scoped to LocalSubnet) for every exe in this
    tree's build*\opm-simulators\bin and build*\opm-upscaling\bin.
  - Removes any existing Block rules that reference those exe paths.
  - All rules carry Group "OPM Flow MPI" so they can be listed or removed
    wholesale:  Get-NetFirewallRule -Group "OPM Flow MPI" | Remove-NetFirewallRule
  - Idempotent; rules are per-exe-path, so rebuilds keep working. Re-run only
    after adding a new build tree.

  Must run elevated (Run as administrator).
#>
$ErrorActionPreference = 'Stop'
$group = 'OPM Flow MPI'
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

$bins = Get-ChildItem (Join-Path $Root 'build*\opm-simulators\bin'), (Join-Path $Root 'build*\opm-upscaling\bin') -Directory -ErrorAction SilentlyContinue
$exes = $bins | Get-ChildItem -Filter '*.exe' | Select-Object -ExpandProperty FullName

# Drop stale Block rules for these paths (created by cancelled popups)
$blocked = Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue |
    Where-Object { $exes -contains $_.Program } |
    Get-NetFirewallRule | Where-Object Action -eq 'Block'
if ($blocked) {
    $blocked | ForEach-Object { Write-Host "removing Block rule: $($_.DisplayName)" }
    $blocked | Remove-NetFirewallRule
}

# Existing rules (any action) for quick lookup, to keep this script idempotent
$existing = @{}
Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue |
    Get-NetFirewallApplicationFilter |
    ForEach-Object { $existing[$_.Program] = $true }

$added = 0
foreach ($exe in $exes) {
    if ($existing.ContainsKey($exe)) { continue }
    $name = [IO.Path]::GetFileNameWithoutExtension($exe)
    New-NetFirewallRule -DisplayName "OPM $name" -Group $group `
        -Direction Inbound -Action Allow -Program $exe `
        -RemoteAddress LocalSubnet -Profile Any | Out-Null
    $added++
}
Write-Host "done: $added rule(s) added, $((@($blocked)).Count) block rule(s) removed, $($exes.Count) exes covered."
Read-Host "Press Enter to close"
