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
       -m qtcharts --archives qtbase qtcharts -O C:\Qt
```

Reboot after the MS-MPI SDK install, or the machine-level `MSMPI_INC` will not
be visible to a shell that was already running.

## 1. Build

```powershell
git clone https://github.com/GitPaean/opm_flow_windows C:\opm\opm_flow_windows
cd C:\opm\opm_flow_windows
.\build-all.ps1 -Mpi -OpenMP -SimTarget flow -Jobs 4 -OpmOrg GitPaean -OpmBranch windows
```

**`-Jobs 4` on a 32 GB machine.** OPM's template-heavy translation units are
RAM-hungry and more concurrent `cl.exe` processes than that can exhaust memory;
6-8 suits a machine with more. Never go above 9.

Then the GUI, from the same shell:

```powershell
. .\setup-env.ps1
cmake -S flow-gui -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Release `
      "-DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64"
cmake --build build-gui -- -j 4
& "C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe" --release --no-translations build-gui\flow-gui.exe
```

Watch the configure output for `flow-gui summary plotting: ON` and
`flow-gui 3D viewer: ON`. If either says OFF the GUI will build but lose a tab -
it means `install-mpi\` or Qt Charts was not found.

Note the quotes around `"-DCMAKE_PREFIX_PATH=..."`. PowerShell does not expand
`$` inside an unquoted argument that begins with `-D`, and the failure is a
confusing "Qt6 not found".

## 2. Package

```powershell
.\package-flow.ps1 -Zip
& "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\opm-flow.iss
.\packaging\build-msix.ps1
```

**Never repackage on top of stale build trees.** `LICENSE.txt` stamps the commit
each module was built from, and that is a correspondence claim under the GPL. If
the sources have moved since the binaries were built, rebuild first - otherwise
the package names commits its binaries did not come from. `package-flow.ps1`
warns if a source tree has uncommitted changes, because then no published commit
corresponds to the binaries at all.

## 3. Verify before uploading

```powershell
# Versions. flow-gui is a WIN32 app: PowerShell will not capture its stdout,
# so redirect through cmd or you will think it prints nothing.
cmd /c "dist\opm-flow-2026.10-pre\bin\flow-gui.exe --version"
dist\opm-flow-2026.10-pre\bin\flow.exe --version

# A real deck, not just --help.
dist\opm-flow-2026.10-pre\bin\flow.exe `
    src\opm-simulators\python\test_data\SPE1CASE1a\SPE1CASE1.DATA `
    --output-dir=spe1_check --threads-per-process=1

# The stamped commits must match what was actually built.
Select-String -Path dist\opm-flow-2026.10-pre\LICENSE.txt -Pattern '^\s+commit '

# The installer must be launchable without administrator rights.
$b=[IO.File]::ReadAllBytes('dist\OPM-Flow-2026.10-pre-Setup.exe')
[regex]::Match([Text.Encoding]::ASCII.GetString($b),'requestedExecutionLevel[^/]{0,60}').Value
# -> must contain level="asInvoker"
```

## 4. Upload

```powershell
gh release upload v2026.10-pre `
  dist\opm-flow-2026.10-pre-win64.zip `
  dist\OPM-Flow-2026.10-pre-Setup.exe `
  dist\OPM-Flow-2026.10.0.0.msix --clobber

# Only if the notes changed:
gh release edit v2026.10-pre --notes-file release-notes\v2026.10-pre.md
```

The auto-generated "Source code (zip/tar.gz)" assets are rendered from the tag,
not uploaded. To refresh them, move the tag:

```powershell
git tag -f v2026.10-pre <commit>; git push origin v2026.10-pre --force
```

Their displayed date stays at the release's original publish date - that label
is cosmetic, the contents do update.

## Known-good baselines

- `flow.exe` about 56 MB, `flow-gui.exe` about 1 MB.
- Staged package about 124 MB; zip about 70 MB.
- ctest, only with `-DBUILD_TESTING=ON -DBUILD_EXAMPLES=ON`: opm-common 226/229,
  opm-simulators 73/139. See VALIDATION.md for why, and for the baseline-free
  check that the failures are exactly the shell-driven tests.
