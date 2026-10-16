"""Generate the test models and IRs used by tools/vst3host/dualtests.cpp and
tools/coretest/slimtest.cpp.

Everything is derived from tests/models/ (official NeuralAmpModelerCore example
models, MIT) or synthesised here, so no third-party captures are needed.

    python tests/make_test_assets.py            -> tests/generated/
"""

import json
import math
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODELS = HERE / "models"
OUT = HERE / "generated"


def write_json(path, obj):
    path.write_text(json.dumps(obj), encoding="utf-8")


def write_wav_float(path, samples, sample_rate, channels=1):
    """32-bit IEEE float WAV. samples: list (mono) or list of frames (multi)."""
    if channels == 1:
        data = b"".join(struct.pack("<f", s) for s in samples)
        n = len(samples)
    else:
        data = b"".join(struct.pack("<" + "f" * channels, *frame) for frame in samples)
        n = len(samples)
    block_align = 4 * channels
    fmt = struct.pack("<HHIIHH", 3, channels, sample_rate, sample_rate * block_align, block_align, 32)
    riff = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    path.write_bytes(b"RIFF" + struct.pack("<I", len(riff)) + riff)
    assert n > 0


def write_wav_pcm24(path, samples, sample_rate):
    frames = bytearray()
    for s in samples:
        v = max(-8388608, min(8388607, int(round(s * 8388607.0))))
        frames += struct.pack("<i", v)[:3]
    fmt = struct.pack("<HHIIHH", 1, 1, sample_rate, sample_rate * 3, 3, 24)
    riff = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(frames)) + bytes(frames)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(riff)) + riff)


def main():
    OUT.mkdir(exist_ok=True)

    # ── A2: split the official container into its two sizes ──────────────────
    a2 = json.loads((MODELS / "A2.nam").read_text(encoding="utf-8"))
    assert a2["architecture"] == "SlimmableContainer", a2["architecture"]
    subs = sorted(a2["config"]["submodels"], key=lambda s: s["max_value"])
    lite, full = subs[0]["model"], subs[-1]["model"]
    ch = lambda m: [la["channels"] for la in m["config"]["layers"]]
    assert ch(full)[0] == 8 and ch(lite)[0] == 3, (ch(full), ch(lite))
    for m in (lite, full):
        m.setdefault("metadata", a2.get("metadata", {}))
    write_json(OUT / "A2_full_only.nam", full)
    write_json(OUT / "A2_lite_only.nam", lite)

    # A second, genuinely different A2 Full model: same architecture, every
    # weight perturbed deterministically by up to 5 %.
    variant = json.loads(json.dumps(full))
    variant["weights"] = [w * (1.0 + 0.05 * math.sin(1.7 * i)) for i, w in enumerate(variant["weights"])]
    write_json(OUT / "A2_full_variant.nam", variant)

    # The same A2 Full model declared at 44.1 kHz, to exercise resampling with a
    # real A2 network.
    full44 = json.loads(json.dumps(full))
    full44["sample_rate"] = 44100
    write_json(OUT / "A2_full_44k.nam", full44)

    # ── Identity models (Linear, one tap of 1.0) at two native rates ─────────
    for sr, name in ((48000, "identity_48k.nam"), (44100, "identity_44k.nam")):
        write_json(OUT / name, {"version": "0.5.4", "architecture": "Linear",
                                "config": {"receptive_field": 1, "bias": False},
                                "weights": [1.0], "sample_rate": sr})

    # ── Broken models ─────────────────────────────────────────────────────────
    text = (MODELS / "A2.nam").read_text(encoding="utf-8")
    (OUT / "broken_truncated.nam").write_text(text[: len(text) // 3], encoding="utf-8")
    (OUT / "broken_garbage.nam").write_bytes(bytes(range(256)) * 8)
    stereo_lin = {"version": "0.5.4", "architecture": "Linear", "config": {"receptive_field": 1, "bias": False},
                  "weights": [1.0, 1.0], "sample_rate": 48000}
    write_json(OUT / "broken_wrong_weights.nam", stereo_lin)

    # ── IRs ───────────────────────────────────────────────────────────────────
    impulse = [1.0] + [0.0] * 63
    write_wav_float(OUT / "ir_impulse_48k.wav", impulse, 48000)
    write_wav_float(OUT / "ir_impulse_44k.wav", impulse, 44100)
    # One-pole lowpass impulse response, 512 taps.
    a = 0.85
    lp = [(1 - a) * a ** n for n in range(512)]
    write_wav_float(OUT / "ir_lowpass_48k.wav", lp, 48000)
    # A decaying, alternating "bright" IR in 24-bit PCM.
    hp = [((-1) ** n) * 0.5 * 0.9 ** n for n in range(256)]
    write_wav_pcm24(OUT / "ir_bright_48k_pcm24.wav", hp, 48000)
    write_wav_float(OUT / "ir_stereo_48k.wav", [(1.0, 0.5)] + [(0.0, 0.0)] * 63, 48000, channels=2)
    (OUT / "broken.wav").write_bytes(b"RIFF\x10\x00\x00\x00WAVEjunkjunkjunk")

    print("assets written to", OUT)


if __name__ == "__main__":
    sys.exit(main())
