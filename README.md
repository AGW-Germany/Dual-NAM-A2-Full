First public test release of **Dual NAM A2 Full**: a Windows x64 VST3 with two independent Neural Amp Modeler engines (A2 Full, also A1/LSTM), two IR loaders, true stereo output and a built-in noise gate.

## Download

- **Plug-in:** `DualNAM_A2_Full-1.2.0-win-x64.zip` (attached below)
  SHA-256: `4641CF28E5E80D28DD5D232C1FCC9F53E80E8251846B53D44186C7EB273A40DB`
- **Complete source code:** `DualNAM_A2_Full-1.2.0-source.zip` (attached below)
  SHA-256: `FD77A85F40E72A863DBE9DAB1DF0352A4B2F33A5F094BA1D1209C58AF90F2912`
  The exact source state the plug-in was built from; how to build it is described in `BUILD.md`.

**Note:** the "Source code (zip)" and "Source code (tar.gz)" archives that GitHub generates automatically for this tag contain only the documentation that was in the repository when the tag was created, **not** the source code. Use `DualNAM_A2_Full-1.2.0-source.zip` or clone the repository.

## Highlights

- Two independent NAM engines; A2 files always play their full size (A2 Full). Inference uses the official, unmodified NeuralAmpModelerCore.
- Two mono IR loaders, 3-band EQ, input/output gain, constant-power pan, mute and bypasses per channel; blend and master.
- Noise gate with threshold, attack (0.1–50 ms), release (10–1000 ms) and a Pre NAM / Post NAM switch; one shared envelope for both channels, no latency, click-free switching.
- Mono bus: both channels at full level. Stereo bus: left input only.
- Automatic resampling and exact latency alignment for models and projects at different sample rates.

## Installation

Unzip, then run `Install.ps1` in an administrator PowerShell:

```
powershell -ExecutionPolicy Bypass -File .\Install.ps1
```

It checks the package, installs `Dual NAM A2 Full 1.2.0.vst3` into `C:\Program Files\Common Files\VST3\` and replaces older copies of this plug-in. Installing by hand and the full user guide are described in the `README.md` inside the ZIP.

## Requirements

Windows 10/11 x64, a VST3 host (tested in Studio One 8 Pro), NAM models (`.nam`) and mono IRs (`.wav`).

## Known limitations

- Windows x64 VST3 only; the source is available for own ports, but only Windows x64 is developed and supported.
- Mono IRs only, max. 8192 samples.
- Resampling (e.g. 44.1 kHz project with 48 kHz models) limits accuracy; 48 kHz projects are not affected.
- Tested by the author in Studio One 8 Pro; other hosts only with the Steinberg VST3 validator.

## Credits, disclaimer and license

Developed by Manuel Adler, ADLER GUITAR WORKS. Based on Gateway Stereo Plus (© 2026 Tunc M Sengiz) and the Neural Amp Modeler plug-in and core by Steven Atkinson and contributors.

Development of Dual NAM A2 Full was assisted by AI-based coding tools under human direction, testing, and review.

Not affiliated with or endorsed by Steven Atkinson, the Neural Amp Modeler project, or the authors of Gateway Stereo Plus.

MIT License. Third-party notices and license texts are in the ZIP (`LICENSES/`), inside the installed plug-in (`Contents/Resources`) and in the repository (`LICENSES/`). Eigen (MPL-2.0) source: https://gitlab.com/libeigen/eigen, commit `ed8cda3ce4cb9d12640f9236fed968655a93b604`.

VST is a trademark of Steinberg Media Technologies GmbH.
