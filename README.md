# Dual NAM A2 Full

A lean Windows x64 VST3 with **two independent Neural Amp Modeler engines** (A2 Full, also
A1/LSTM) and **two IR loaders**. A mono guitar signal feeds both channels; each channel has its
own model, its own IR, its own levels and its own pan position, and the output is true stereo.
A built-in noise gate can sit before or after the models.

![Dual NAM A2 Full](docs/screenshot.png)

## Download

**Version 1.2.0 (Windows x64, VST3):**
[DualNAM_A2_Full-1.2.0-win-x64.zip](https://github.com/AGW-Germany/Dual-NAM-A2-Full/releases/download/v1.2.0/DualNAM_A2_Full-1.2.0-win-x64.zip)

SHA-256: `4641CF28E5E80D28DD5D232C1FCC9F53E80E8251846B53D44186C7EB273A40DB`

All releases: [Releases](https://github.com/AGW-Germany/Dual-NAM-A2-Full/releases)

This is a public test release. Feedback and bug reports are welcome via
[Issues](https://github.com/AGW-Germany/Dual-NAM-A2-Full/issues).

## Signal flow

```
                  MONO INPUT          (stereo bus: left input only)
                      |
                      +------------> gate detector (one envelope)
                      |
                [Gate, Pre NAM]
                      |
            +---------+---------+
            |                   |
        CHANNEL A           CHANNEL B
        Input Gain          Input Gain
        NAM (A2 Full)       NAM (A2 Full)
      [Gate, Post NAM]    [Gate, Post NAM]   (same envelope)
        EQ (B/M/T)          EQ (B/M/T)
        IR Loader A         IR Loader B
        Output Gain         Output Gain
        Mute / Blend        Mute / Blend
        Pan                 Pan
            +---------+---------+
                      |
                Master Output
                      |
                STEREO OUTPUT
```

## Requirements

* Windows 10 or 11, 64-bit, x64 CPU (SSE2 is enough, no AVX required)
* A VST3 host, e.g. Studio One 8 Pro
* NAM models (`.nam`) and mono IRs (`.wav`)

## Installation

1. Download and unzip `DualNAM_A2_Full-1.2.0-win-x64.zip`.
2. Open **PowerShell as administrator**, change into the unzipped folder and run
   `powershell -ExecutionPolicy Bypass -File .\Install.ps1`. The script
   * first checks that the package is complete and stops without changing anything if it is not,
   * copies the folder `Dual NAM A2 Full 1.2.0.vst3` to `C:\Program Files\Common Files\VST3\`, and
   * replaces older copies of this plug-in (only folders named `DualNAM_A2_Full.vst3` or
     `Dual NAM A2 Full <version>.vst3` whose DLL identifies itself as Dual NAM A2 Full). If an
     old copy is in use (e.g. the DAW is still running), nothing is changed; close the DAW and
     run the script again.

   **By hand:** delete any older `DualNAM_A2_Full.vst3` or `Dual NAM A2 Full ….vst3` folder from
   the VST3 folder, then copy the **folder** `Dual NAM A2 Full 1.2.0.vst3` (the whole folder,
   not just the file inside it) to `C:\Program Files\Common Files\VST3\`.
3. Start your host and rescan plug-ins if necessary.
4. The plug-in appears as **Dual NAM A2 Full** (vendor *ADLER GUITAR WORKS*, category *Fx*).

Keep only one copy installed: every version has the same VST3 class ID, so two copies side by
side would be the same plug-in twice for the host. The plug-in has its own IDs and does not clash
with the original NAM plug-in or with Gateway Stereo Plus.

**Uninstall:** delete the folder `C:\Program Files\Common Files\VST3\Dual NAM A2 Full 1.2.0.vst3`.

The full user guide (controls, gate, routing, sample rate, presets, limitations) is the
`README.md` inside the download; the same text is in [docs/USER_GUIDE.md](docs/USER_GUIDE.md).

## Features

* Two independent NAM engines; A2 files always play their **full size (A2 Full)**, no "Slim"
  control. A1 and LSTM models are supported too. Inference uses the official, unmodified
  NeuralAmpModelerCore.
* Two mono IR loaders, 3-band EQ, input/output gain, constant-power pan, mute and bypasses per
  channel; blend and master.
* Noise gate with threshold, attack (0.1–50 ms), release (10–1000 ms) and a **Pre NAM / Post NAM**
  position switch; one detector on the guitar signal and one shared envelope for both channels;
  no latency, click-free switching.
* Fixed input routing: mono bus → both channels at full level; stereo bus → left input only.
* Automatic resampling and exact latency alignment when models and project rates differ.

## Known limitations

* Windows x64 VST3 only (no AU/AAX/macOS build).
* Mono IRs only; direct IR convolution, max. 8192 samples, as in the NAM plug-in.
* When resampling (e.g. 44.1 kHz projects with 48 kHz models), the NAM project's resampler limits
  accuracy (about −39 dB residual at 1 kHz). 48 kHz projects are not affected.
* Tested by the author in Studio One 8 Pro; other hosts have been checked with the Steinberg
  VST3 validator only.

## Source code and building

This repository contains the complete source code of Dual NAM A2 Full 1.2.0, the exact state the
released binary was built from. See **[BUILD.md](BUILD.md)** for how to build it on Windows x64.
The plug-in is developed and supported for Windows x64 only; you are welcome to use the code for
your own ports, but other platforms are not supported.

**Source archive:** the complete source is also attached to the v1.2.0 release as
[DualNAM_A2_Full-1.2.0-source.zip](https://github.com/AGW-Germany/Dual-NAM-A2-Full/releases/download/v1.2.0/DualNAM_A2_Full-1.2.0-source.zip)
(dependencies are fetched separately, see [BUILD.md](BUILD.md)). The "Source code (zip/tar.gz)"
archives that GitHub generates automatically for the tag `v1.2.0` contain only the documentation
that was in the repository when the tag was created, **not** the source code.

| Path | Content |
|---|---|
| `NeuralAmpModeler/` | the plug-in: DSP, state, GUI (`NeuralAmpModeler.cpp/.h`, `NeuralAmpModelerControls.h`), identity (`config.h`), resources, Visual Studio projects |
| `iPlug2/`, `NeuralAmpModelerCore/`, `AudioDSPTools/`, `eigen/` | dependencies (git submodules, unmodified) |
| `build.ps1`, `Install.ps1` | release build and installer |
| `run_tests.ps1`, `tools/`, `tests/` | automated tests (VST3 test host, NAM core tests, test models) |
| `docs/` | user guide and screenshot |
| `LICENSES/`, `LICENSE` | license texts and third-party notices |

The plug-in is built on the following open-source projects (exact licenses in
[LICENSES/ThirdPartyNotices.txt](LICENSES/ThirdPartyNotices.txt)):

* [Gateway Stereo Plus](https://github.com/Naaxas/GatewayStereoPlus) (MIT), a fork of the
  [Neural Amp Modeler plug-in](https://github.com/sdatkinson/NeuralAmpModelerPlugin) (MIT)
* [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) v0.5.3 (MIT),
  unmodified
* [AudioDSPTools](https://github.com/sdatkinson/AudioDSPTools) v0.1.2 (MIT)
* [iPlug2](https://github.com/iPlug2/iPlug2) (zlib-style license; the
  [NAM fork](https://github.com/sdatkinson/iPlug2) is used), Steinberg VST3 SDK 3.8 (MIT)
* [Eigen](https://gitlab.com/libeigen/eigen) (MPL-2.0), unmodified. The exact Eigen source used
  is available from https://gitlab.com/libeigen/eigen at commit
  `ed8cda3ce4cb9d12640f9236fed968655a93b604`.

## Credits and disclaimer

Dual NAM A2 Full is developed by **Manuel Adler, ADLER GUITAR WORKS**
(manuel.adler@ais-moers.de).

Neural Amp Modeler is by **[Steven Atkinson](https://github.com/sdatkinson)**, with
[Oli Larkin and contributors](https://github.com/sdatkinson/NeuralAmpModelerPlugin/graphs/contributors):
the models, the training code, the inference engine and the plug-in this is built on are their
work. Gateway Stereo Plus (© 2026 Tunc M Sengiz) is the direct basis of this project.

Development of Dual NAM A2 Full was assisted by AI-based coding tools under human direction, testing, and review.

This is an independent project. It is **not affiliated with or endorsed by** Steven Atkinson,
the Neural Amp Modeler project, or the authors of Gateway Stereo Plus. "NAM" and "Neural Amp
Modeler" are used only to describe compatibility with NAM model files.

## License

The plug-in is licensed under the MIT License, see [LICENSE](LICENSE):
© 2022 Steven Atkinson, © 2026 Tunc M Sengiz, © 2026 Manuel Adler, ADLER GUITAR WORKS.

Third-party notices and the full license texts are in [LICENSES/](LICENSES/)
(start with [ThirdPartyNotices.txt](LICENSES/ThirdPartyNotices.txt)); the same files are included
in the download and inside the installed plug-in (`Contents/Resources`). The build uses
`NeuralAmpModeler/installer/ThirdPartyNotices.txt` as the source of the notices; the dependencies
in the submodules carry their own license files.

VST is a trademark of Steinberg Media Technologies GmbH.
