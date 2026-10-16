# Dual NAM A2 Full

A lean Windows x64 VST3 with **two independent Neural Amp Modeler engines** (A2 Full, also
A1/LSTM) and **two IR loaders**. A mono guitar signal feeds both channels; each channel has its
own model, its own IR, its own levels and its own pan position, and the output is true stereo.

It is based on [Gateway Stereo Plus](https://github.com/Naaxas/GatewayStereoPlus) (MIT), which
builds on the [Neural Amp Modeler plug-in](https://github.com/sdatkinson/NeuralAmpModelerPlugin)
(MIT). Inference runs on the official, unmodified
[NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore): no custom NAM engine,
no model conversion.

![Dual NAM A2 Full](docs/screenshot.png)

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

---

## Requirements

* Windows 10 or 11, 64-bit, x64 CPU (SSE2 is enough, no AVX required)
* A VST3 host, e.g. Studio One 8 Pro
* NAM models (`.nam`) and mono IRs (`.wav`)

## Installation

1. Unzip `DualNAM_A2_Full-1.2.0-win-x64.zip`.
2. Open **PowerShell as administrator**, change into the unzipped folder and run
   `powershell -ExecutionPolicy Bypass -File .\Install.ps1`. The script
   * first checks that the package is complete (bundle folder and plug-in DLL) and stops without
     changing anything if it is not,
   * copies the folder `Dual NAM A2 Full 1.2.0.vst3` to `C:\Program Files\Common Files\VST3\`, and
   * replaces older copies of this plug-in in that folder and in the user's VST3 folder: only
     folders named `DualNAM_A2_Full.vst3` or `Dual NAM A2 Full <version>.vst3` whose DLL
     identifies itself as Dual NAM A2 Full. If an old copy is in use (e.g. the DAW is still
     running), nothing is changed; close the DAW and run the script again.

   **By hand:** first delete every old `DualNAM_A2_Full.vst3` or `Dual NAM A2 Full ….vst3`
   folder from the VST3 folder, then copy the **folder** `Dual NAM A2 Full 1.2.0.vst3` (the whole
   folder, not just the file inside it) to `C:\Program Files\Common Files\VST3\`.
3. Start your host. If the plug-in does not show up in Studio One: check in the options that
   `C:\Program Files\Common Files\VST3` is listed as a VST plug-in location, trigger a new
   plug-in scan (reset the blocklist if necessary) and restart.
4. The plug-in appears as **Dual NAM A2 Full** (vendor *ADLER GUITAR WORKS*, category *Fx*).
   Up to and including the first 1.2.0 test build the vendor was shown as *DualNAM*; see
   "Vendor name change" below.

**Why only one copy:** the version number is only part of the folder and file name. Every
version has the same VST3 class ID and the same display name; that is how the host identifies
the plug-in and maps it in projects. Two copies side by side would be the same plug-in twice for
the host, and which one gets loaded would be undefined. Running two versions in parallel for
comparison is therefore not supported.

The plug-in has its own IDs and does not clash with the original NAM plug-in or with Gateway
Stereo Plus; all of them can be installed side by side.

**Vendor name change:** the vendor shown by the host is now *ADLER GUITAR WORKS* (previously
*DualNAM*). The VST3 class ID (`F2AEE70D00DE4F4E444E616D446E4132`), the plug-in ID and the
plug-in name are unchanged, so existing projects keep loading the plug-in with all settings. The
vendor name only affects how hosts sort and file the plug-in:

* The plug-in browser lists it under the new vendor; after a rescan, favourites or custom
  folders that referred to the old entry may need to be set again.
* Hosts that store presets in vendor folders keep older presets under the old name, for example
  `Documents\VST3 Presets\DualNAM\Dual NAM A2 Full\` (VST3 standard location) or the
  corresponding vendor folder of the host's own preset library. Presets saved from now on go to
  the new vendor folder. Older presets are not moved automatically; move them by hand into the
  new vendor folder if the host no longer lists them.

**Uninstall:** delete the folder `C:\Program Files\Common Files\VST3\Dual NAM A2 Full 1.2.0.vst3`
(or `DualNAM_A2_Full.vst3` for older installations).

## Controls

### Channel A / Channel B

Both channels are identical (A blue, B yellow) and fully independent.

| Control | Function |
|---|---|
| **Model browser** (top row) | Folder icon: choose a `.nam` file. `<` `>`: previous/next model in the same folder. Click on the name: list of all models in the folder. `X`: remove the model. |
| **IR browser** (second row) | The same for `.wav` impulse responses. |
| **Input** | Level **before** the model (±20 dB): sets how hard the model is driven. |
| **Bass / Middle / Treble** | 3-band EQ after the model (as in the NAM plug-in). |
| **Output** | Level after model and IR (±40 dB). |
| **Pan** | Channel pan (L100 … C … R100), constant power (sine/cosine): hard left 0 dB / silent, centre −3.01 dB on both sides, hard right silent / 0 dB. |
| **EQ** | EQ on/off. |
| **NAM Bypass** | Bypass the model (the DI signal goes straight to the IR). Saves the model's CPU time. |
| **IR Bypass** | Bypass the IR. Saves the CPU time of the convolution. |
| **Mute** | Mute the channel. A muted channel is **not processed at all**. |

### Input

The routing is fixed; there is nothing to set:

| Input bus | A and B receive |
|---|---|
| **Mono** | the input, unchanged and at full level |
| **Stereo** | the **left** input, unchanged and at full level; the right input is ignored |

There is no averaging and no level adjustment. If you need two different signals on A and B,
use two instances.

### Noise gate (bottom left)

| Control | Function |
|---|---|
| **Gate** | Gate on/off. When off, the other gate controls are greyed out (they can still be preset). |
| **Position** (vertical switch: Pre NAM on top, Post NAM below; the active position is labelled in white, a click on the switch track toggles it) | **Pre NAM:** the gate sits before the split, both models get the gated signal. **Post NAM** (default): the gate sits right after both models, before EQ and IR, so it also removes the models' own noise. |
| **Threshold** | Opening threshold (−100 … 0 dB), relative to the **guitar signal at the plug-in input**, i.e. independent of Input A/B. |
| **Attack** | Fade-in time when opening, 0.1 … 50 ms (default 3 ms): time to reach 99 % level. |
| **Release** | Fade-out time when closing, 10 … 1000 ms (default 150 ms): time to reach −40 dB. |

* One detector on the mono guitar signal, **before** the input gains, and **one** smoothed
  envelope for both channels: A and B always open and close at exactly the same time.
* Built in: level detection with 2 ms smoothing, **3 dB hysteresis** (the gate only closes 3 dB
  below the threshold) and **10 ms hold** (it stays open for another 10 ms after the level drops).
  This prevents chattering on decaying notes.
* No latency, no lookahead. With 0.1 ms attack even a short pick attack is preserved.
* Switching between Pre and Post, gate on/off and control moves are click-free while audio is
  running: the gate's effect fades out over 10 ms, the position changes, and the effect fades
  back in over 10 ms at the new position. NAM is never computed twice.

### Output (bottom right)

* **Blend A | B**: balance of the channels. Fully left: A only. Centre: **A and B both at full
  level** (display "A100 B100"). Fully right: B only. In between, the other channel is faded out
  (e.g. "A100 B50"). In the centre the sum is louder than a single channel (about +3 … +6 dB);
  compensate with **Master**. A channel that is faded out completely is not processed.
* **Master**: overall level (−40 … +12 dB).
* **Meter**: output left/right.

Blend and pan are independent: blend sets *how loud* each channel is, pan sets *where* it sits.

### Typical settings

* **Two amps blended, centred:** Pan A = C, Pan B = C, Blend in the centre. Each amp then sits at
  −3 dB on both sides.
* **Two amps left/right:** Pan A = L100, Pan B = R100. Each amp is at 0 dB on its side.
* **One amp, two cabs:** the same model on A and B, different IRs, pan to taste.

### A2 Full

A2 files often contain two sizes (A2 Lite and A2 Full) in one container. This plug-in **always
selects the full size (A2 Full)**; there is deliberately no "Slim" control. (In the original NAM
plug-in and in Gateway Stereo Plus, "Slim" defaults to 0, which plays A2 *Lite* for such files.)
A2 models use the NAM core's optimised A2 code path; model quality is not reduced.

### Sample rate and latency

NAM models are usually trained at 48 kHz. If the project runs at another rate (e.g. 44.1 kHz),
the plug-in resamples the signal for the model (the Lanczos resampler from the NAM project).
This costs about 29 samples of latency, which is reported to the host and compensated
automatically. If the two models have different rates, the faster channel is delayed so that
both line up exactly.
**Recommendation:** run the project at 48 kHz; latency is then 0 and nothing is resampled.

### Mono guitar track

The plug-in offers the bus layouts *mono → stereo* and *stereo → stereo*, but not
*mono → mono*; the output is always stereo. Either way the guitar reaches both models without any
level loss: on a mono bus the input, on a stereo bus the left channel.

### Projects from version 1.1.0

Older projects load without loss; models, IRs and all other settings are kept. Deliberate
changes:

* **Gate sound:** the new gate works differently from the old one (attack, release, hold,
  hysteresis, smooth envelope). An old project gets attack 3 ms, release 150 ms and position
  **Post NAM** (the defaults); choose **Pre NAM** for the old place in front of the models.
* **Threshold:** the old gate measured *after* the input gains, the new one *before* them. So that
  it opens for the same playing, the threshold is converted once on loading:
  **new = old − highest input gain of the audible channels** (limited to −100 … 0 dB). Example:
  threshold −70 dB, Input A +6 dB, Input B −3 dB → −76 dB. For all mono routings this is provably
  the exact same opening threshold (verified against the old gate's own code). Only for projects
  saved before 1.2.0; a project saved in 1.2.0 is never converted again.
* **Input Source:** the selector is gone. Projects on a mono bus or with **Mono Left** keep their
  routing. On a stereo bus, old projects with **Mono Right** (previously the right input),
  **Mono Sum** (previously the average of L and R) or **Stereo Independent** (previously L → A,
  R → B) now play the left input on both channels. For "Stereo Independent" only channel A is
  taken into account in the threshold conversion (an approximation).

### Projects from version 1.0.0

The same applies as for 1.1.0 (gate, threshold conversion, fixed routing; on a stereo bus,
"Mono Split" from 1.0.0 corresponds to "Mono Sum", "Stereo" to "Stereo Independent").
**In addition, the pan law changed in 1.1.0:** in 1.0.0 a centred channel was at 0 dB and a
hard-panned one at +3 dB; since 1.1.0 they are at −3 dB and 0 dB. Old projects are therefore
**3 dB quieter**; compensate with Master +3 dB if needed.

### Projects, presets, missing files

A project stores both model paths, both IR paths and all parameters. The files are reloaded when
the project opens. If a file is missing, nothing crashes: the channel stays without a model or
IR, the browser shows **"(MISSING) file name"**, and the tooltip shows the full path. The path is
kept in the project; as soon as the file is back in place (or chosen again in the browser),
everything is as before. A channel without a model passes the (gated, EQ'd) input signal through
unchanged, as in the original NAM plug-in.

Invalid files (not a NAM file, damaged, stereo IR, …) are rejected with a message; the
previously loaded model or IR keeps playing.

### IRs

**Mono** WAV files (PCM 16/24/32-bit, 32-bit float) at any sample rate are supported; they are
resampled to the project rate on loading. Stereo IRs are rejected with the message "File is not
mono". As in the NAM plug-in, IRs are limited to 8192 samples and lowered by 18 dB (the NAM
project's level matching).

### CPU

Measured on an Intel Core Ultra 7 155H (laptop), 48 kHz, on a thread set up like a DAW audio
thread: 2 × A2 Full + 2 × IR (2048 samples) + EQ + gate ≈ **12–20 % of one core** at buffer sizes
of 64 to 512 samples (range across measurement runs); one A2 Full model alone ≈ 6–8 %.

## Parameters (automation)

`Input A/B`, `Output A/B`, `Pan A/B`, `Bass A/B`, `Middle A/B`, `Treble A/B`, `EQ A/B`,
`NAM Bypass A/B`, `IR Bypass A/B`, `Mute A/B`, `Blend`, `Master`, `Gate`, `Gate Threshold`,
`Gate Attack`, `Gate Release`, `Gate Position`.

Parameter IDs 0–23 are unchanged since 1.0.0. New or changed in 1.2.0:

| ID | Parameter | |
|---|---|---|
| 24 | `Input Source (unused)` | reserved, no effect, not automatable (was "Input Source" in 1.1.0) |
| 25 | `Gate Attack` | 0.1 … 50 ms, default 3 ms |
| 26 | `Gate Release` | 10 … 1000 ms, default 150 ms |
| 27 | `Gate Position` | 0 = Pre NAM, 1 = Post NAM (default) |

An update is a fresh installation with `Install.ps1` (see Installation): old copies are removed
and the new one takes their place. Class ID and plug-in name stay the same, so existing projects
find the plug-in again.

## Known limitations

* Windows x64 VST3 only (no AU/AAX/macOS build in this package).
* Mono IRs only; direct IR convolution (no FFT convolution), max. 8192 samples, as in the NAM
  plug-in.
* When resampling, the NAM project's resampler limits the accuracy (about −39 dB residual at
  1 kHz for 44.1 ↔ 48 kHz, level exact). This does not occur in 48 kHz projects.
* Tested by the author in Studio One 8 Pro; other hosts have only been checked with the Steinberg
  VST3 validator and the project's own VST3 test host.

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

MIT – © 2022 Steven Atkinson, © 2026 Tunc M Sengiz, © 2026 Manuel Adler, ADLER GUITAR WORKS.
Third-party licenses (NeuralAmpModelerCore, AudioDSPTools, iPlug2/WDL, Steinberg VST3 SDK, Eigen,
NanoVG, NanoSVG, glad, nlohmann/json, Roboto, Michroma, …) are listed in `ThirdPartyNotices.txt`.
In the release package it is in the `LICENSES/` folder together with the full license texts; the
installed plug-in carries the same notices in `Contents/Resources/ThirdPartyNotices.txt` and the
license texts they refer to in `Contents/Resources/Licenses/`.
Eigen is used under the MPL-2.0; its source is available from https://gitlab.com/libeigen/eigen
(commit `ed8cda3ce4cb9d12640f9236fed968655a93b604`).
VST is a trademark of Steinberg Media Technologies GmbH.
