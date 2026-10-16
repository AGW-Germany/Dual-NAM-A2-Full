# vst3host — the GatewayStereoPlus verification harness

A real VST3 host, built against the VST3 SDK vendored in this repo at
`iPlug2/Dependencies/IPlug/VST3_SDK`. It loads and drives the **actual built binary** —
real bus arrangements, real `IParameterChanges` automation, real state chunks, real
`IEditController` — so the results are about the plugin you load in a DAW, not a
reimplementation of it.

This harness is the entire verification story for the fork. It previously lived in a
disposable scratchpad outside the repo; it is vendored here so it survives.

This harness comes from Gateway Stereo Plus; its reports (`TEST_REPORT.md`, `AUDIT_REPORT.md`,
`BUILD_NOTES.md`) are part of that project, not of this repository. Dual NAM A2 Full adds
`dualtests.cpp` (the plug-in test suite) and `guishot.cpp` (editor test); both are built and run
by `run_tests.ps1`, see `BUILD.md`.

## Building

Both script sets resolve the SDK from their own location, so a fresh checkout works with
no editing. Override `VST3_SDK` to point elsewhere.

**macOS** — one script per suite:

```bash
./build_all.sh      # every suite
./build.sh          # just the battery -> vst3tests
```

**Windows** — run from a shell where `vcvars64.bat` has been executed:

```bat
build_all_win.bat                      :: every suite
build_win.bat tests.cpp vst3tests      :: just one
```

`build_plugin_win.bat <source-root> <vst3-out-dir> [target]` builds the plugin itself.
Set `VCVARS` if your Build Tools are not at the default location.

## The suites

| Program | macOS script | Windows | Arguments |
|---|---|---|---|
| `vst3tests` | `build.sh` | `tests.cpp` | `<fork.vst3> <reference.vst3> <model.nam>` |
| `vst3deep` | `build_deep.sh` | `deep.cpp` | `<fork.vst3> <model.nam> <model2.nam> [soakSeconds]` |
| `vst3dry` | `build_dry.sh` | `dryfeatures.cpp` | `<fork.vst3> <model.nam>` |
| `vst3comb` | `build_comb.sh` | `combcheck.cpp` | `<fork.vst3> <model.nam>` |
| `vst3sym` | `build_sym.sh` | `symcheck.cpp` | `<fork.vst3> <model.nam> [model2.nam]` |
| `vst3fuzz` | `build_fuzz.sh` | `statefuzz.cpp` | `<fork.vst3> <model.nam>` |
| `vst3swap` | `build_swap.sh` | `swapprobe.cpp` | `<fork.vst3> <model.nam> <model2.nam>` |
| `vst3link` | `build_link.sh` | `linkstate.cpp` | `<fork.vst3> [model.nam]` |
| `vst3hunt`…`vst3hunt5` | `build_hunt*.sh` | `hunt*.cpp` | `<fork.vst3> <model.nam>`; hunt5 takes `<fork> <reference> <model>` |
| `vst3rtalloc` | `build_rtalloc.sh` | **macOS only** | `<fork.vst3> <model.nam>` |

`run_all_win.bat` runs everything on Windows and prints a pass/fail summary. Set `M1`
(and ideally `M2`) to `.nam` files first; `FORK`, `REF` and `LOGDIR` have defaults that
resolve to this repo.

## Traps that have each cost a session

- **`deep` takes the pristine-upstream reference from the `GW_REF` environment variable,
  not from argv.** With it unset, its three `PDC identical to upstream` assertions compare
  `-1/-1` against `-1/-1` and **pass vacuously**. Always set it.
- **`swapprobe` needs three arguments**, not two.
- **`DefaultParams()` exists twice**, in `tests.cpp` and `deep.cpp`. Both zero-fill a
  `vector<double>(kNumParams)` then name the parameters they know about. A parameter added
  to `host.h` but not to those two functions silently defaults to **0** — and `kMix = 0`
  means fully dry, so every measurement becomes a measurement of the bypass path. It
  presents as a dead-flat frequency response, zero THD, no tone-stack effect and an Output
  knob that moves nothing. **Adding a parameter means editing three places.**
- **`deep` needs its model argument twice.** With one, later sections run with no model
  loaded and report nonsense.
- **Sweep rates and block sizes when adding an assertion.** R10 (dual-mono symmetry)
  survived 83 assertions, pluginval at strictness 10, auval, ASan and UBSan because every
  symmetry test ran at 48 kHz — the one rate where the resampler is idle and the bug could
  not appear. It was 100% line-covered.
- **`rtalloc` is macOS-only**, because `mallocount.c` hooks libmalloc's `malloc_logger`
  and there is no Windows equivalent without a detouring library. `build_all_win.bat`
  omits it deliberately.
- **Arm allocation counting via `Plug::sOnProcessEnter/Leave`, never around
  `Plug::process()`.** The harness wrapper allocates on every call — a `ParameterChanges`
  with `kNumParams + 8` queues plus four vector temporaries, about **60 allocations and
  3.5 kB per block**. Bracketing the wrapper measures the rig, not the plugin, and reports
  FAIL against anything at all. This is why the hooks exist.
- **`rtalloc` asserts a positive control first.** Every one of its checks is "expected 0,
  got 0", so a broken counter would report a clean sweep while measuring nothing —
  the same vacuous-pass shape as the `GW_REF` bug above. `mallocount.c` self-tests at
  startup and `rtalloc` refuses to run if the counter is dead.

## Windows portability notes

`host.cpp`/`host.h` needed no changes. The platform-specific pieces are:

- the SDK's `module_win32.cpp` in place of `module_mac.mm`, and no `-framework` flags;
- `/D_USE_MATH_DEFINES` — MSVC does not define `M_PI` in `<cmath>`;
- `/Zc:__cplusplus` — without it MSVC reports `__cplusplus` as `199711L` and the SDK's
  C++17 feature checks silently take their pre-C++17 branches;
- `ssize_t` and the `<mach/mach.h>` RSS helper, both handled behind `#ifdef _WIN32` in
  `tests.cpp` and `deep.cpp` (`GetProcessMemoryInfo`, link `Psapi.lib`). Guard
  `windows.h` with `WIN32_LEAN_AND_MEAN` and `NOMINMAX` or it breaks `std::min/max`.

## pluginval

Use the **matrix** form, not the default invocation — the default passed 25 clean sections
while two real defects were present:

```bash
pluginval --strictness-level 10 \
  --sample-rates 44100,48000,88200,96000,176400,192000 \
  --block-sizes 32,64,127,512,1024,2048 \
  --repeat 3 --randomise --timeout-ms 900000 \
  --validate <path to GatewayStereoPlus.vst3>
```

On Windows over SSH add `--skip-gui-tests`: an SSH session has no interactive window
station, so pluginval dies mid-way through `Open editor whilst processing` with exit code
5 and no error text. Run the GUI sections from a logged-on desktop session instead.
