# Packaging OPM Flow for Windows distribution

How to turn this harness' build output (`flow.exe`, `flow_blackoil.exe`,
`flow-gui-qt.exe`) into something other people can install and run. Three
tiers, from simplest to Store-ready — all start from the same staging step.

> Build everything first (`build-all.ps1 ... -SimTarget all`, and
> `flow-gui-qt` per its README). Package from a freshly rebuilt tree so the
> binaries match the sources you publish (GPL — see below).

## 0. Stage: `package-flow.ps1`

```powershell
.\package-flow.ps1 -Zip          # -Version <v>; default simulator: flow
                                 # (contains every model variant incl. black-oil)
```

Stages `dist\opm-flow-<version>\`:

| Piece | Contents |
|-------|----------|
| `bin\` | simulators + their runtime DLLs (OpenBLAS, SuiteSparse, fmt, gfortran runtime), MSVC CRT (`msvcp140*`, `vcruntime140*`), OpenMP runtime (`libomp140.x86_64.dll`), `flow-gui-qt.exe` + Qt 6 DLLs and plugins |
| `redist\` | `vc_redist.x64.exe`, `msmpisetup.exe` (downloaded once, cached in `dist\_cache`) |
| `README.txt`, `LICENSE.txt` | end-user instructions; GPLv3 + source-availability statement |

`-Zip` also emits `dist\opm-flow-<version>-win64.zip`.

**Tier 1 — portable ZIP.** Ship that zip. Users unzip and double-click
`bin\flow-gui-qt.exe`. The VC++ and OpenMP runtimes are bundled in `bin\`, so
the only external prerequisite is **MS-MPI** — required even for serial runs
(the simulators link `msmpi.dll`, a system component that is not bundled). If it
isn't already installed, run `redist\msmpisetup.exe` once (or `winget install
Microsoft.msmpi`). `redist\vc_redist.x64.exe` is included only as a fallback;
the app-local CRT means it is not normally needed.

## Tier 2 — installer: `installer\opm-flow.iss` (Inno Setup)

```powershell
winget install -e --id JRSoftware.InnoSetup       # one-time
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\opm-flow.iss
```

Produces `dist\OPM-Flow-<version>-Setup.exe`, which installs to
`Program Files\OPM Flow`, creates Start-menu (and optional desktop)
shortcuts to the GUI, shows the GPL license page, and **silently installs
the VC++ and MS-MPI runtimes only when missing** (registry / `msmpi.dll`
checks). Override the version with `ISCC /DAppVersion=<v> ...`.

**Unsigned installers trip SmartScreen** on end-user machines — a dismissible
*reputation* warning at download ("isn't commonly downloaded" → Keep / Keep
anyway) and first run ("Windows protected your PC" → More info → Run anyway).
The packaged `README.txt` and the repo README document these click-throughs for
users.

For public distribution, **code-sign the setup exe** to remove the warnings.
Either sign the built exe directly:

```powershell
signtool sign /fd SHA256 /a /t <timestamp-url> dist\OPM-Flow-<ver>-Setup.exe
```

or have Inno sign it during compile — pass `/DSignExe` and a named SignTool
`opmsign` to ISCC (see the header of `installer\opm-flow.iss`):

```powershell
ISCC /DSignExe `
     /Sopmsign="\"<path-to>\signtool.exe\" sign /fd SHA256 /a /t http://timestamp.digicert.com `$f" `
     installer\opm-flow.iss
```

Use an **OV or EV** certificate from a CA: an **EV** cert earns SmartScreen
reputation immediately, an **OV** cert builds it up as downloads accumulate. A
**self-signed** cert does *not* help — SmartScreen reputation is independent of
the local trust store (self-signing only matters for the MSIX path below).

## Tier 3 — MSIX / Microsoft Store: `packaging\build-msix.ps1`

```powershell
.\packaging\build-msix.ps1            # unsigned .msix (what the Store wants)
.\packaging\build-msix.ps1 -Sign      # + self-signed cert for local sideload testing
```

Wraps `bin\` + generated logo assets + an `AppxManifest.xml`
(`runFullTrust` Win32 app, entry point `flow-gui-qt.exe`) with `makeappx`
from the Windows SDK into `dist\OPM-Flow-<x.x.x.x>.msix`.

**Store submission** (once you have a Partner Center account —
one-time individual registration fee):
1. Partner Center → Apps → *reserve the app name*. This assigns the real
   `Identity Name` / `Publisher` — put them into `build-msix.ps1`'s
   parameters and rebuild.
2. Upload the **unsigned** `.msix` (the Store signs packages itself).
3. Fill the listing (description, screenshots — the GUI window is the
   natural one), set price/markets, submit for certification.

**Caveats to decide up front:**
- **MPI:** an MSIX cannot run prerequisite installers, so MS-MPI must be
  installed separately (`winget install Microsoft.msmpi`) or the simulators
  won't start (they link `msmpi.dll`). State this in the Store listing, or
  ship a serial (non-MPI) simulator build inside the Store package.
- **GPLv3:** OPM Flow is GPL v3. Distributing through the Store is done by
  others (e.g. VLC) but you must ensure the listing/terms don't impose
  restrictions beyond the GPL and that complete corresponding source stays
  available (LICENSE.txt in the package points at the repos — keep the
  pushed branches in sync with what you ship).
- Sideloading a self-signed build on another machine requires importing the
  certificate into `LocalMachine\TrustedPeople` there (the script prints the
  exact commands).

## Licensing / redistribution notes

- **MSVC CRT** app-local copies and `vc_redist.x64.exe` are redistributable
  per Visual Studio's Distributable Code terms. `libomp140.x86_64.dll` is
  picked from the Build Tools redist tree (note: Build Tools places it under
  a `debug_nonredist` folder even for the release DLL — verify against
  Microsoft's current Distributable Code list before wide distribution, or
  have end users install it via the VS redist).
- **MS-MPI**: `msmpisetup.exe` is Microsoft's redistributable installer —
  bundling the installer (not loose `msmpi.dll`) is the sanctioned path.
- **Qt 6** is used under LGPLv3 via dynamic linking (users can replace the
  Qt DLLs), satisfying the relink requirement.
- **OPM itself is GPLv3+**: any binary distribution must offer the complete
  corresponding source. `LICENSE.txt` (generated by `package-flow.ps1`)
  names the exact repositories/branches — keep them pushed and current.

## Publishing the release notes

The text shown on each GitHub release lives in the repo at
`release-notes/v<version>.md` — **tracked**, so its history is versioned
alongside the code (unlike `dist/`, which is packaging output and git-ignored).
Edit that file, then publish it to the release page with the GitHub CLI:

```powershell
gh release edit v<version> -R GitPaean/opm_flow_windows --notes-file release-notes\v<version>.md
```

This updates only the notes text — it does **not** touch the uploaded assets.
Keep the file and the live release in sync so the two never drift.
