# Cutting a Windows release from a clean machine

Everything needed to go from a bare Windows box to uploaded release assets.
Written after doing it, so the pitfalls listed are ones that actually bit.

Budget: a cold run is a few hours, nearly all of it unattended (vcpkg builds
the dependencies, then DUNE and the three OPM modules). A machine that has done
it before is far quicker, because `vcpkg\`, `build-mpi\` and `install-mpi\` are
kept and only what changed is recompiled.

## 0. Prerequisites

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools -e `
  --override "--quiet --wait --norestart `
    --add Microsoft.VisualStudio.Workload.VCTools `
    --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    --add Microsoft.VisualStudio.Component.Windows11SDK.22621 `
    --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended"
winget install -e --id Git.Git
winget install -e --id Microsoft.msmpi          # runtime
winget install -e --id Microsoft.msmpisdk       # SDK: provides MSMPI_INC / MSMPI_LIB64
winget install -e --id JRSoftware.InnoSetup     # only for the installer
winget install -e --id Python.Python.3.12       # only to fetch Qt below
```

Qt 6 (prebuilt; do not build it from source - Smart App Control blocks the
freshly built `moc`/`rcc` on machines where it is enforced):

```powershell
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 `
       -m qtcharts --archives qtbase qtcharts opengl32sw -O C:\Qt
```

Keep `opengl32sw`. It is Qt's software OpenGL renderer, the fallback that keeps
the GUI from opening as a blank window where the graphics driver lacks OpenGL
2.0, and `package-flow.ps1` refuses to package a GUI without it.

The Intel MPI package is built against the pip-installed Intel MPI SDK, which
needs no administrator rights:

```powershell
python -m pip install --user impi-rt impi-devel
```

Reboot after the MS-MPI SDK install, or the machine-level `MSMPI_INC` will not
be visible to a shell that was already running.

## 1. Bring the Windows series onto OPM master

A package is a snapshot of OPM master plus the Windows series, named by the date
of that snapshot - not by OPM's version, which master changes whenever a release
branches off. So start by rebasing the `windows` branch of opm-common, opm-grid
and opm-simulators onto the current upstream master, in whichever checkout you
keep the series, and push it: that is what makes the commits `LICENSE.txt` will
name public.

```powershell
git -C src\opm-common remote add upstream https://github.com/OPM/opm-common.git   # once
git -C src\opm-common fetch upstream
git -C src\opm-common rebase upstream/master windows
git -C src\opm-common push --force-with-lease origin windows
# ... and the same for opm-grid and opm-simulators
```

Build that in the same checkout (`build-all.ps1 -SkipClone -SkipDeps` with the
§2 options) and check it with §4's SPE1 and Norne runs. Then pin it, so that the
harness builds exactly this set from now on rather than whatever the branches
hold next:

```powershell
.\update-opm-pins.ps1         # writes the src\opm-* commits to opm-sources.json
```

It refuses a module with local changes, or a commit that is not on the fork's
branch on GitHub. The nightly workflow's pinned job builds the pins with the
runner's MSVC 19.51 from the next night on; to know before a release, start the
windows-weekly workflow by hand with `sources: pinned` once the harness commit
below is pushed.

The snapshot's date is that of the newest upstream commit of the three. Set it
as the version defaults in `package-flow.ps1` (`master-YYYY-MM-DD`),
`installer\opm-flow.iss` (the same) and `packaging\build-msix.ps1`
(`YYYY.M.D.0`, since an MSIX version has to be numeric) and commit them together
with `opm-sources.json` and README.md (whose quick start `update-opm-pins.ps1`
keeps naming the snapshot), so the harness commit the packages stamp names the
snapshot and the OPM commits too.

## 2. Build

```powershell
git clone https://github.com/GitPaean/opm_flow_windows C:\opm\opm_flow_windows
cd C:\opm\opm_flow_windows
.\build-all.ps1 -Mpi -OpenMP -SimTarget flow -Jobs 4 -OpmOrg GitPaean -OpmBranch windows
```

That clones the forks and checks each module out at the commit
`opm-sources.json` pins.

**`-Jobs 4` on a 32 GB machine.** OPM's template-heavy translation units are
RAM-hungry and more concurrent `cl.exe` processes than that can exhaust memory;
6-8 suits a machine with more. Never go above 9.

Then the same simulator against Intel MPI, for the no-administrator package. It
builds into its own `build-impi\` / `install-impi\` trees from the sources
`build-all.ps1` cloned, so it leaves the MS-MPI build alone:

```powershell
.\build-intelmpi.ps1 -SimTarget flow -Jobs 4
```

Then the GUI, from the same shell:

```powershell
. .\setup-env.ps1
cmake -S flow-gui -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Release `
      "-DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64"
cmake --build build-gui -- -j 4
& "C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe" --release --no-translations build-gui\flow-gui.exe
```

Watch the configure output for `flow-gui summary plotting: ON`,
`flow-gui 3D viewer: ON` and `flow-gui deck model (group tree / network): ON`.
If any says OFF the GUI will build but lose a tab. The first two mean
`install-mpi\` or Qt Charts was not found; the third, which decides the Well
Hierarchy tab, means the test link against opm-common's parser failed.

Note the quotes around `"-DCMAKE_PREFIX_PATH=..."`. PowerShell does not expand
`$` inside an unquoted argument that begins with `-D`, and the failure is a
confusing "Qt6 not found".

On a machine that has built before, keep its trees and run the same command with
`-SkipClone -SkipDeps`, with `src\` at the pinned commits:
`.\update-opm-pins.ps1 -Check` says whether it is, and `build-all.ps1` warns
when it is not. `package-flow.ps1` warns too.

## 3. Package

```powershell
.\package-flow.ps1 -Zip
.\package-flow.ps1 -IntelMpi -Zip
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\opm-flow.iss
.\packaging\build-msix.ps1
```

**Never repackage on top of stale build trees.** `LICENSE.txt` stamps the commit
each module was built from, and that is a correspondence claim under the GPL. If
the sources have moved since the binaries were built, rebuild first - otherwise
the package names commits its binaries did not come from. `package-flow.ps1`
warns if a source tree has uncommitted changes, because then no published commit
corresponds to the binaries at all.

## 4. Verify before uploading

The examples use the snapshot `master-2026-10-12`.

```powershell
# Versions. flow-gui is a WIN32 app: PowerShell will not capture its stdout,
# so redirect through cmd or you will think it prints nothing. flow reports
# the development version OPM master carried, not the snapshot's name.
cmd /c "dist\opm-flow-master-2026-10-12\bin\flow-gui.exe --version"
dist\opm-flow-master-2026-10-12\bin\flow.exe --version
cmd /c "dist\opm-flow-master-2026-10-12-impi\bin\flow-impi.cmd --version"

# A real deck, not just --help.
dist\opm-flow-master-2026-10-12\bin\flow.exe `
    src\opm-simulators\python\test_data\SPE1CASE1a\SPE1CASE1.DATA `
    --output-dir=spe1_check --threads-per-process=1

# The stamped commits must match what was actually built - and the pins.
Select-String -Path dist\opm-flow-master-2026-10-12\LICENSE.txt -Pattern '^\s+commit '
.\update-opm-pins.ps1 -Check

# The installer must be launchable without administrator rights.
$b=[IO.File]::ReadAllBytes('dist\OPM-Flow-master-2026-10-12-Setup.exe')
[regex]::Match([Text.Encoding]::ASCII.GetString($b),'requestedExecutionLevel[^/]{0,60}').Value
# -> must contain level="asInvoker"

# Its simulator-only type must leave every GUI file out. Per-user and silent,
# so no administrator is needed. Run it where OPM Flow is not installed: it
# would install over an existing install, and the uninstall would remove it.
$d = "$env:TEMP\opm-sim-check"
Start-Process -Wait dist\OPM-Flow-master-2026-10-12-Setup.exe `
    -ArgumentList '/VERYSILENT', '/CURRENTUSER', '/TYPE=simulator', "/DIR=$d"
Get-ChildItem "$d\bin" -Recurse -Include flow-gui.exe, Qt6*.dll, opengl32sw.dll, qwindows.dll
# -> nothing
Start-Process -Wait "$d\unins000.exe" -ArgumentList '/VERYSILENT'
```

## 5. Publish

Each snapshot is a pre-release of its own, tagged with its name. Push the
harness first: `--target` creates the tag at the commit the packages stamp, so
the release's "Source code" archives are exactly that commit.

```powershell
git push origin master
gh release create master-2026-10-12 --target (git rev-parse HEAD) --prerelease `
  --title "OPM Flow for Windows - OPM master of 2026-10-12" `
  --notes-file release-notes\master-2026-10-12.md `
  dist\opm-flow-master-2026-10-12-win64.zip `
  dist\opm-flow-master-2026-10-12-impi-win64.zip `
  dist\OPM-Flow-master-2026-10-12-Setup.exe `
  dist\OPM-Flow-2026.10.12.0.msix

# Later changes to the notes:
gh release edit master-2026-10-12 --notes-file release-notes\master-2026-10-12.md
```

Earlier snapshots stay downloadable as their own releases.

## Known-good baselines

- `flow.exe` about 59 MB. `flow-gui.exe` about 8 MB since 0.8.0, which links
  opm-common's parser for the Well Hierarchy tab; about 1 MB before that.
- Staged package about 154 MB, 20 MB of it `opengl32sw.dll`; zip about 77 MB,
  installer about 56 MB, MSIX about 49 MB.
- ctest, only with `-DBUILD_TESTING=ON -DBUILD_EXAMPLES=ON`: opm-common 226/229,
  opm-simulators 73/139. See VALIDATION.md for why, and for the baseline-free
  check that the failures are exactly the shell-driven tests.
