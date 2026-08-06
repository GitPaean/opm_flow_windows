; Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
; opm-flow.iss - Inno Setup installer for OPM Flow for Windows.
;
; Build:  1. .\package-flow.ps1              (stages dist\opm-flow-<ver>\)
;         2. ISCC installer\opm-flow.iss     (or /DAppVersion=... to override)
; Output: dist\OPM-Flow-<ver>-Setup.exe
;
; The installer copies the staged bin\ tree to Program Files (or, for a
; per-user install, to %LOCALAPPDATA%\Programs), creates Start menu shortcuts,
; and silently installs the VC++ and MS-MPI runtimes when they are missing and
; it has the rights to do so.
;
; Sign (optional): pass /DSignExe plus a named SignTool "opmsign" to ISCC to
; code-sign the installer (and its embedded uninstaller) during compile
; (PowerShell; `$f is Inno's placeholder for the file, escaped so PowerShell
; passes it through literally):
;   ISCC /DSignExe `
;        /Sopmsign="\"<path-to>\signtool.exe\" sign /fd SHA256 /a /t http://timestamp.digicert.com `$f" `
;        installer\opm-flow.iss
; With a real OV/EV code-signing certificate this removes the SmartScreen
; "isn't commonly downloaded" / "Windows protected your PC" warnings (an EV cert
; earns reputation immediately; OV builds it up over time/downloads). A
; SELF-SIGNED cert does NOT help SmartScreen. Without /DSignExe the installer is
; built unsigned and those warnings are expected but dismissible (see README).

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
; Default to a machine-wide install, but let the user choose "just for me" when
; they have no administrator rights - the common case on a managed work laptop,
; where an admin-only installer is not an inconvenience but a wall. The {auto*}
; constants below follow that choice: {autopf} is Program Files for a machine
; install and {localappdata}\Programs for a per-user one, and {group} and
; {autodesktop} likewise pick the common or the per-user location.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog

#ifdef SignExe
; Code-sign the installer and its embedded uninstaller with the named SignTool
; "opmsign" supplied on the ISCC command line (see the header for the invocation).
SignTool=opmsign
SignedUninstaller=yes
#endif

[Files]
Source: "{#StageDir}\bin\*";    DestDir: "{app}\bin"; Flags: recursesubdirs
Source: "{#StageDir}\README.txt";  DestDir: "{app}"
Source: "{#StageDir}\LICENSE.txt"; DestDir: "{app}"
Source: "{#StageDir}\redist\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "{#StageDir}\redist\msmpisetup.exe";    DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\OPM Flow GUI";  Filename: "{app}\bin\flow-gui.exe"
Name: "{group}\README";        Filename: "{app}\README.txt"
Name: "{autodesktop}\OPM Flow GUI"; Filename: "{app}\bin\flow-gui.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Run]
; Both bundled runtimes install machine-wide and need administrator rights, so
; they are skipped in a per-user install. That is safe for the VC++ runtime -
; the CRT is also shipped app-local in bin\, which is why redist\ calls it a
; fallback - but not for MS-MPI: without it nothing runs at all, so say so
; rather than leave the user with a DLL error (see CurStepChanged below).
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; \
    StatusMsg: "Installing Microsoft Visual C++ runtime..."; \
    Check: InstallVCRedist
; MS-MPI runtime: required even for serial runs (the simulators link msmpi.dll).
Filename: "{tmp}\msmpisetup.exe"; Parameters: "-unattend"; \
    StatusMsg: "Installing Microsoft MPI runtime..."; \
    Check: InstallMsMpi
Filename: "{app}\bin\flow-gui.exe"; Description: "Launch OPM Flow GUI"; \
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

// Both installers write machine-wide, so only run them when we actually hold
// the rights to do so. Attempting them in a per-user install would either
// raise a UAC prompt the user cannot satisfy, or fail mid-way.
function InstallVCRedist: Boolean;
begin
  Result := VCRedistNeeded and IsAdminInstallMode;
end;

function InstallMsMpi: Boolean;
begin
  Result := MsMpiNeeded and IsAdminInstallMode;
end;

// If MS-MPI is still absent once we are done - either because this was a
// per-user install, or because its installer failed - the simulator will not
// start at all, and the error it gives ("the code execution cannot proceed
// because msmpi.dll was not found") says nothing about the cause. Say it here.
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and MsMpiNeeded then
    MsgBox('OPM Flow is installed, but Microsoft MPI is not present on this'
           + #13#10 + 'machine, and the simulator cannot start without it -'
           + #13#10 + 'not even for a serial run.'
           + #13#10 + #13#10
           + 'Installing MS-MPI needs administrator rights. Either ask an'
           + #13#10 + 'administrator to run:'
           + #13#10 + #13#10
           + '    winget install Microsoft.msmpi'
           + #13#10 + #13#10
           + 'or re-run this installer as an administrator, which installs'
           + #13#10 + 'it for you.',
           mbInformation, MB_OK);
end;
