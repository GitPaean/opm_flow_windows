; opm-flow.iss - Inno Setup installer for OPM Flow for Windows.
;
; Build:  1. .\package-flow.ps1              (stages dist\opm-flow-<ver>\)
;         2. ISCC installer\opm-flow.iss     (or /DAppVersion=... to override)
; Output: dist\OPM-Flow-<ver>-Setup.exe
;
; The installer copies the staged bin\ tree to Program Files, creates Start
; menu shortcuts, and silently installs the VC++ and MS-MPI runtimes when
; they are missing.

#ifndef AppVersion
  #define AppVersion "2026.10-pre"
#endif
#define StageDir "..\dist\opm-flow-" + AppVersion

[Setup]
AppId={{6F1C6E39-8B0A-4D0B-9C63-0OPMFLOW0001}
AppName=OPM Flow
AppVersion={#AppVersion}
AppPublisher=The Open Porous Media project (Windows build)
AppPublisherURL=https://opm-project.org
DefaultDirName={autopf}\OPM Flow
DefaultGroupName=OPM Flow
LicenseFile={#StageDir}\LICENSE.txt
OutputDir=..\dist
OutputBaseFilename=OPM-Flow-{#AppVersion}-Setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
PrivilegesRequired=admin

[Files]
Source: "{#StageDir}\bin\*";    DestDir: "{app}\bin"; Flags: recursesubdirs
Source: "{#StageDir}\README.txt";  DestDir: "{app}"
Source: "{#StageDir}\LICENSE.txt"; DestDir: "{app}"
Source: "{#StageDir}\redist\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "{#StageDir}\redist\msmpisetup.exe";    DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\OPM Flow GUI";  Filename: "{app}\bin\flow-gui-qt.exe"
Name: "{group}\README";        Filename: "{app}\README.txt"
Name: "{autodesktop}\OPM Flow GUI"; Filename: "{app}\bin\flow-gui-qt.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Run]
; VC++ runtime: quiet, no restart; harmless if already present (fast no-op).
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; \
    StatusMsg: "Installing Microsoft Visual C++ runtime..."; \
    Check: VCRedistNeeded
; MS-MPI runtime: required even for serial runs (the simulators link msmpi.dll).
Filename: "{tmp}\msmpisetup.exe"; Parameters: "-unattend"; \
    StatusMsg: "Installing Microsoft MPI runtime..."; \
    Check: MsMpiNeeded
Filename: "{app}\bin\flow-gui-qt.exe"; Description: "Launch OPM Flow GUI"; \
    Flags: nowait postinstall skipifsilent

[Code]
function VCRedistNeeded: Boolean;
var Installed: Cardinal;
begin
  Result := not (RegQueryDWordValue(HKLM64,
      'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64',
      'Installed', Installed) and (Installed = 1));
end;

function MsMpiNeeded: Boolean;
begin
  Result := not FileExists(ExpandConstant('{sys}\msmpi.dll'));
end;
