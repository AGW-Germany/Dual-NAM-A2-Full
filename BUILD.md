# Building Dual NAM A2 Full from source (Windows x64)

This describes how to build the Dual NAM A2 Full VST3 from this repository. Dual NAM A2 Full is
developed and supported for **Windows x64 only**. The source tree still contains the macOS/iOS/web
project files inherited from the Neural Amp Modeler plug-in and Gateway Stereo Plus; they are
not maintained here. You are welcome to use the code for your own ports, but we do not support
them.

Steps marked **(verified)** were tested from a fresh, independent checkout on Windows 11 Pro
(10.0.26200) with the tool versions listed below; steps marked **(not verified)** were not.

A build from this source at the same folder path produces the same DLL as a build from the
private development tree the release was made from (the two differ only in 26 bytes of
timestamps and debug identifiers, as do two builds of the same tree). Built in another folder,
the DLL is laid out differently (MSVC derives the names of anonymous namespaces from the source
file path), but it passes the same test suite with identical results.

## 1. Requirements

| Tool | Version used | Needed for |
|---|---|---|
| Visual Studio 2022 Build Tools with the "Desktop development with C++" workload (MSVC v143, component `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`) | 17.14, MSVC 14.44.35207 | building the plug-in |
| Windows SDK | 10.0.22621.0 | building the plug-in |
| Git for Windows, including Git Bash | 2.x | submodules and the VST3 SDK download script |
| Windows PowerShell | 5.1 (built into Windows) | `build.ps1`, `run_tests.ps1` |
| Python 3 | 3.x | test assets only (`run_tests.ps1`) |
| CMake | 4.x | Steinberg VST3 validator only (`run_tests.ps1`) |

The full Visual Studio IDE is not required. The Build Tools can be installed with
**(not verified: the tools were already installed on the test machine)**:

```
winget install Microsoft.VisualStudio.2022.BuildTools --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
```

## 2. Dependencies

All dependencies are public and pinned to the exact revisions used for the 1.2.0 release:

| Path | Source | Revision |
|---|---|---|
| `iPlug2` | https://github.com/sdatkinson/iPlug2 (NAM fork of iPlug2) | `66f9060b5287afdb2ddb83aab5a2a2a8e66a4d5e` |
| `NeuralAmpModelerCore` | https://github.com/sdatkinson/NeuralAmpModelerCore | `9c7b185de346fe0725dea537bcee4bc38b5bb6d6` (v0.5.3) |
| `AudioDSPTools` | https://github.com/sdatkinson/AudioDSPTools | `e19ef4b5b3bf2171c431847563acde29eedf85c0` (v0.1.2) |
| `eigen` | https://gitlab.com/libeigen/eigen | `ed8cda3ce4cb9d12640f9236fed968655a93b604` |
| `iPlug2/Dependencies/IPlug/VST3_SDK` | https://github.com/steinbergmedia/vst3sdk | tag `v3.8.0_build_66` (downloaded by `build.ps1`) |

The first four are git submodules of this repository. The Steinberg VST3 SDK (MIT licensed since
3.8) is not part of the repository; `build.ps1` downloads it with iPlug2's own script, pinned to
`v3.8.0_build_66`, and checks that it reports `VST 3.8.0`. None of the dependencies is modified.

## 3. Getting the source

**From git (recommended) (verified: clone and submodule checkout from the public sources):**

```
git clone --recurse-submodules https://github.com/AGW-Germany/Dual-NAM-A2-Full.git
cd Dual-NAM-A2-Full
```

If you cloned without `--recurse-submodules`, run `git submodule update --init` (top level only;
the nested submodules of NeuralAmpModelerCore and AudioDSPTools are not used).

**From the source archive `DualNAM_A2_Full-1.2.0-source.zip`** (attached to the v1.2.0 release):
the archive contains this repository's files but not the dependencies. Unzip it, open a
PowerShell in the unzipped folder and fetch the dependencies at the pinned revisions
**(verified)**:

```
git clone https://github.com/sdatkinson/iPlug2.git iPlug2
git -C iPlug2 checkout 66f9060b5287afdb2ddb83aab5a2a2a8e66a4d5e
git clone https://github.com/sdatkinson/NeuralAmpModelerCore.git NeuralAmpModelerCore
git -C NeuralAmpModelerCore checkout 9c7b185de346fe0725dea537bcee4bc38b5bb6d6
git clone https://github.com/sdatkinson/AudioDSPTools.git AudioDSPTools
git -C AudioDSPTools checkout e19ef4b5b3bf2171c431847563acde29eedf85c0
git clone https://gitlab.com/libeigen/eigen.git eigen
git -C eigen checkout ed8cda3ce4cb9d12640f9236fed968655a93b604
```

Note: GitHub's automatically generated "Source code" archives of the tag `v1.2.0` do **not**
contain the source code (that tag marks the first, documentation-only commit of this
repository). Use git or `DualNAM_A2_Full-1.2.0-source.zip`.

## 4. Building

From the repository root, in a normal (not elevated) PowerShell **(verified: full `-Clean`
build from the source archive, including the VST3 SDK download)**:

```
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

`build.ps1`:

1. finds MSBuild with `vswhere`,
2. runs `git submodule update --init` (skipped automatically if the folder is not a git
   checkout, e.g. when using the source archive),
3. downloads the VST3 SDK if it is missing (needs Git Bash),
4. builds the project `NeuralAmpModeler-vst3`, configuration `Release|x64`, through the solution
   `NeuralAmpModeler\NeuralAmpModeler.sln` (always build through the `.sln`; the property sheets
   need `$(SolutionDir)`),
5. assembles `dist\` and the installation ZIP.

Options: `-Clean` (full rebuild), `-Install` (installs into
`C:\Program Files\Common Files\VST3\` via `Install.ps1`; needs an elevated PowerShell;
**not verified** from source, `Install.ps1` itself was tested with the release package).
Without `-Install` nothing is written outside the repository.

## 5. Output (verified)

| Path | Content |
|---|---|
| `NeuralAmpModeler\build-win\DualNAM_A2_Full.vst3\` | the VST3 bundle as built (internal name) |
| `dist\Dual NAM A2 Full 1.2.0.vst3\` | the bundle as shipped: `Contents\x86_64-win\Dual NAM A2 Full 1.2.0.vst3` (the DLL), `Contents\Resources\ThirdPartyNotices.txt`, `Contents\Resources\Licenses\` |
| `dist\` | additionally `Install.ps1`, the user guide as `README.md`, `docs\screenshot.png`, `LICENSES\` |
| `release\DualNAM_A2_Full-1.2.0-win-x64.zip` | the installation ZIP, plus `.sha256` |

Only the shipped bundle carries the version in its name; the VST3 class ID
(`F2AEE70D00DE4F4E444E616D446E4132`), the plug-in name "Dual NAM A2 Full" and the vendor
"ADLER GUITAR WORKS" are fixed in `NeuralAmpModeler\config.h`.

## 6. Tests (optional)

```
powershell -ExecutionPolicy Bypass -File .\run_tests.ps1
```

Builds and runs the NAM core test (`tools\coretest\slimtest`), the gate threshold migration proof
(`tools\coretest\thresholdproof`), the plug-in test suite (`tools\vst3host\dualtests`) and the
editor test (`tools\vst3host\guishot`) against the built plug-in, then the Steinberg VST3
validator (normal and `-e`). The validator is built from a separate full checkout of the SDK tag
in `_reference\vst3sdk` (needs CMake); skip it with `-SkipValidator`. Use `-Quick` for shorter
stress and CPU runs. Logs go to `test-output\`. The editor test opens a real plug-in window; do
not use the mouse while it runs.

**Partly verified:** the plug-in test suite (169/169), the editor test and the validator (47/47,
extended 537/537) were run against the plug-in built from the source archive, using test programs
built from the same `tools\` sources; `run_tests.ps1` itself was not run in the fresh checkout.

## 7. Build configuration

* Release|x64, `/O2`, whole program optimisation, static CRT, C++20, SSE2 (x64 baseline; an AVX2
  build was measured and was slower for A2 models).
* Preprocessor: `IGRAPHICS_NANOVG;IGRAPHICS_GL2;NAM_ENABLE_A2_FAST`.
* The linker embeds only the PDB file name, not the build path (`/PDBALTPATH:%_PDB%`).

## 8. Known limitations

* Windows x64 VST3 only. 32-bit, AAX, AU, macOS, iOS, web and Linux builds are not supported;
  the corresponding project files are upstream leftovers.
* Builds are not bit-identical between machines or runs (the compiler and linker embed
  timestamps and a PDB identifier). A build from this source therefore does not have the same
  SHA-256 as the released DLL, but contains the same code.
* The VST3 SDK must be the pinned tag; other SDK versions have not been tested.
* `run_tests.ps1` needs an interactive desktop session (editor test) and network access (VST3 SDK
  checkout for the validator).
