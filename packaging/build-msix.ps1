<#
  build-msix.ps1 - build an MSIX package of OPM Flow (GUI + simulators) from
  the tree staged by package-flow.ps1.

  Usage:
    .\package-flow.ps1                                  # stage first
    .\packaging\build-msix.ps1                          # unsigned .msix (for Store upload)
    .\packaging\build-msix.ps1 -Sign                    # + self-signed cert for local sideloading

  Notes (see PACKAGING.md for the full story):
  - Store submissions are uploaded UNSIGNED; the Store signs them. The
    Identity Name/Publisher below must then be replaced with the values
    Partner Center reserves for your app.
  - MSIX cannot run prerequisite installers: MS-MPI must be installed
    separately (winget install Microsoft.msmpi) or the simulators will not
    start (they link msmpi.dll). The GUI itself has no such dependency.
#>
[CmdletBinding()]
param(
    [string]$StageVersion = '2026.10-pre',       # folder suffix from package-flow.ps1
    [string]$MsixVersion  = '2026.10.0.0',       # MSIX requires numeric x.x.x.x
    [string]$Publisher    = 'CN=OPM-Windows-Dev',
    [switch]$Sign
)
$ErrorActionPreference = 'Stop'
$Root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Stage = Join-Path $Root "dist\opm-flow-$StageVersion"
$Pack  = Join-Path $Root 'dist\msix-stage'
if (-not (Test-Path "$Stage\bin\flow-gui-qt.exe")) { throw "run package-flow.ps1 first ($Stage missing)" }

# --- locate SDK tools -------------------------------------------------------
$kits = 'C:\Program Files (x86)\Windows Kits\10\bin'
$makeappx = Get-ChildItem "$kits\*\x64\makeappx.exe" | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
$signtool = Get-ChildItem "$kits\*\x64\signtool.exe" | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $makeappx) { throw 'makeappx.exe not found (Windows SDK required)' }

# --- stage ------------------------------------------------------------------
Remove-Item $Pack -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$Pack\Assets" | Out-Null
Copy-Item "$Stage\bin"        "$Pack\bin" -Recurse
Copy-Item "$Stage\README.txt"  $Pack
Copy-Item "$Stage\LICENSE.txt" $Pack

# --- generate simple logo assets (green square, OPM monogram) ----------------
Add-Type -AssemblyName System.Drawing
function New-Logo([int]$w, [int]$h, [string]$path, [int]$font) {
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::FromArgb(0x0b, 0x3d, 0x63))
    $fnt = New-Object System.Drawing.Font('Segoe UI', $font, [System.Drawing.FontStyle]::Bold)
    $fmt = New-Object System.Drawing.StringFormat
    $fmt.Alignment = 'Center'; $fmt.LineAlignment = 'Center'
    $g.DrawString('OPM', $fnt, [System.Drawing.Brushes]::White,
                  (New-Object System.Drawing.RectangleF(0, 0, $w, $h)), $fmt)
    $g.Dispose()
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
New-Logo 150 150 "$Pack\Assets\Square150x150Logo.png" 34
New-Logo  44  44 "$Pack\Assets\Square44x44Logo.png"   10
New-Logo  50  50 "$Pack\Assets\StoreLogo.png"         11

# --- manifest ------------------------------------------------------------------
@"
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities">
  <Identity Name="OPM.Flow" Publisher="$Publisher" Version="$MsixVersion"
            ProcessorArchitecture="x64" />
  <Properties>
    <DisplayName>OPM Flow</DisplayName>
    <PublisherDisplayName>Open Porous Media (Windows build)</PublisherDisplayName>
    <Logo>Assets\StoreLogo.png</Logo>
  </Properties>
  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0"
                        MaxVersionTested="10.0.26100.0" />
  </Dependencies>
  <Resources>
    <Resource Language="en-us" />
  </Resources>
  <Applications>
    <Application Id="OPMFlowGUI" Executable="bin\flow-gui-qt.exe"
                 EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements DisplayName="OPM Flow"
          Description="Run OPM Flow reservoir simulations from a graphical job queue."
          BackgroundColor="transparent"
          Square150x150Logo="Assets\Square150x150Logo.png"
          Square44x44Logo="Assets\Square44x44Logo.png" />
    </Application>
  </Applications>
  <Capabilities>
    <rescap:Capability Name="runFullTrust" />
  </Capabilities>
</Package>
"@ | Set-Content -Encoding utf8 "$Pack\AppxManifest.xml"

# --- pack ----------------------------------------------------------------------
$msix = Join-Path $Root "dist\OPM-Flow-$MsixVersion.msix"
Remove-Item $msix -Force -ErrorAction SilentlyContinue
& $makeappx pack /d $Pack /p $msix /o | Select-Object -Last 2
if ($LASTEXITCODE -ne 0) { throw "makeappx failed ($LASTEXITCODE)" }
Write-Host "packed: $msix ($([math]::Round((Get-Item $msix).Length/1MB,1)) MB)" -ForegroundColor Green

# --- optional: self-sign for local sideloading -----------------------------------
if ($Sign) {
    if (-not $signtool) { throw 'signtool.exe not found' }
    $cert = Get-ChildItem Cert:\CurrentUser\My |
            Where-Object Subject -eq $Publisher | Select-Object -First 1
    if (-not $cert) {
        Write-Host "creating self-signed certificate $Publisher (CurrentUser\My)"
        $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher `
            -KeyUsage DigitalSignature -FriendlyName 'OPM Flow MSIX dev signing' `
            -CertStoreLocation 'Cert:\CurrentUser\My' `
            -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    }
    & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $msix | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) { throw "signtool failed ($LASTEXITCODE)" }
    Write-Host @"
signed with self-signed cert (thumbprint $($cert.Thumbprint)).
To sideload on a test machine, first trust the certificate (elevated):
  Export-Certificate -Cert (Get-Item Cert:\CurrentUser\My\$($cert.Thumbprint)) -FilePath opmflow.cer
  Import-Certificate -FilePath opmflow.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
then double-click the .msix or:  Add-AppxPackage $msix
"@
}
