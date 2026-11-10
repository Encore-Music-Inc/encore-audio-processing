# ENCORE. Audio Processing
# Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
#
# The reference set, in one place: small test inputs (recipes for
# dsp/ReferenceSignals.h, which makes them the same, bit for bit, everywhere),
# what each is run through, and how close each result must be, with why.
# Running this builds tests/ReferenceTests.cpp natively, runs every case and
# writes the expected results into
#   data/reference.json
# which tests/ReferenceTests.cpp (natively, on every system) and
# wasm/reference.mjs (through master.wasm) check. CI runs this script on Linux
# x64 and fails if the file differs from what it writes, so a result only
# moves when this script is run and the new file committed.
#
#   python3 tools/reference.py
#
# Needs a C++20 compiler (c++, or $CXX). The file is written from a Linux x64
# build with GCC, as CI's check is.

import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "data", "reference.json")

# ---- tolerances: each result names one. abs and rel add up: |got - expected| <= abs + rel * |expected|.
# 0 and 0 means exact. Each "why" says what the allowance is for; none is wide
# enough to hide a change to the processing.
TOLERANCES = {
    "exact": {"abs": 0, "rel": 0,
              "why": "Integer results (counts, lengths, notes, sample frames, kit maps) and sample peaks of the generated inputs: the same on every system."},
    "lufs": {"abs": 0.0005, "rel": 0,
             "why": "Loudness, LUFS: double-precision filters and sums; systems differ in float rounding and the maths library's log10 and pow."},
    "lu": {"abs": 0.001, "rel": 0,
           "why": "Loudness range, LU: a difference of two percentiles of short-term loudness, each within the lufs allowance."},
    "db": {"abs": 0.0005, "rel": 0,
           "why": "Levels in dB (peaks, RMS, balance): log10 of a float, rounding and the maths library."},
    "peak": {"abs": 0, "rel": 1e-5,
             "why": "True peak, linear: the 4x upsampler's sums in float, which compilers order and fuse differently (MSVC /fp:fast, FMA on arm64); 1e-5 is 0.0001 dB."},
    "hz": {"abs": 0, "rel": 1e-5,
           "why": "A detected pitch, Hz: YIN's sums of differences and the interpolated dip, which compilers order and fuse differently; 1e-5 is 0.017 cents."},
    "cents": {"abs": 0.02, "rel": 0,
              "why": "Cents from the nearest note: the hz allowance through log2."},
    "clarity": {"abs": 1e-5, "rel": 0,
                "why": "How periodic a voice is, 0 to 1: the depth of YIN's dip, rounding only."},
    "bpm": {"abs": 0.001, "rel": 0,
            "why": "Tempo, BPM: the best of a fixed grid of tempos, scored on an onset envelope made by an FFT (Accelerate on Apple systems), so only a near tie between two grid tempos could move it."},
    "frames": {"abs": 0.01, "rel": 0,
               "why": "An onset's position, sample frames: found to a fraction of a sample by a fitted predictor; rounding moves the fraction slightly."},
    "master-lufs": {"abs": 0.01, "rel": 0,
                    "why": "A master's loudness, LUFS: the whole chain (learning, oversampled saturation and limiting) runs in float over tens of seconds of audio, so rounding adds up; still a tenth of the 0.1 LU the driver aims within."},
    "master-db": {"abs": 0.02, "rel": 0,
                  "why": "What mastering learned (gains, dB): the same long float chain as master-lufs."},
    "master-peak": {"abs": 0, "rel": 0.002,
                    "why": "The master's true peak, linear: the same long float chain as master-lufs (0.002 is 0.02 dB)."},
    "sound-sum": {"abs": 0, "rel": 1e-5,
                  "why": "A sound's sum of |x|: synthesized with sin and exp, which maths libraries round differently in the last place."},
    "sound-peak": {"abs": 0, "rel": 1e-5,
                   "why": "A sound's peak: as sound-sum."},
}

# which tolerance each result of an op uses (by its name's last part)
FIELDS = {
    "loudness-meter": {"integrated": "lufs"},
    "loudness-report": {"integrated": "lufs", "shortTermMax": "lufs", "momentaryMax": "lufs", "range": "lu",
                        "samplePeak": "db", "truePeak": "db", "rmsLeft": "db", "rmsRight": "db", "balance": "db", "frames": "exact"},
    "true-peak": {"truePeak": "peak", "samplePeak": "exact", "position": "exact", "truePeakLeft": "peak", "truePeakRight": "peak"},
    "tuner": {"hz": "hz", "note": "exact", "cents": "cents"},
    "voice": {"hz": "hz", "clarity": "clarity"},
    "tempo": {"valid": "exact", "bpm": "bpm", "others": "exact", "other": "bpm"},
    "onsets": {"count": "exact", "onset": "frames"},
    "mastering": {"lufs": "master-lufs", "preGain": "master-db", "drive": "master-db", "truePeak": "master-peak"},
    "click-sounds": {"length": "exact", "sum": "sound-sum", "peak": "sound-peak"},
    "drum-sounds": {"length": "exact", "sum": "sound-sum", "peak": "sound-peak"},
    "drum-kits": {"sound": "exact", "slot": "exact", "note": "exact", "choke": "exact"},
}


def tolerance(op, name):
    field = FIELDS[op]
    last = name.split(".")[-1]
    if last in field:
        return field[last]
    stem = last.rstrip("0123456789")
    if stem in field:
        return field[stem]
    raise SystemExit(f"tools/reference.py: no tolerance for {op} result {name}")


# ---- inputs (dsp/ReferenceSignals.h describes every field)

def tone(hz, l, r=None, **kw):
    return dict(type="tone", hz=hz, l=l, r=l if r is None else r, **kw)


def noise(seed, l, r=None, **kw):
    return dict(type="noise", seed=seed, l=l, r=l if r is None else r, **kw)


def voiced(hz, level, harmonics, **kw):
    """A tone with harmonics: harmonics[k] is the level of harmonic k + 1, relative to level."""
    return [tone(hz, level * h, mult=k + 1, **kw) for k, h in enumerate(harmonics) if h]


def signal(rate, seconds, channels, *parts):
    flat = []

    def add(p):
        if isinstance(p, list):
            for q in p:
                add(q)
        else:
            flat.append(p)
    add(list(parts))
    return {"rate": rate, "seconds": seconds, "channels": channels, "parts": flat}


SUNG = [1, 0.5, 0.35, 0.2, 0.12, 0.08, 0.05]          # a voice's harmonics, falling
PLUCKED = [1, 0.7, 0.45, 0.3, 0.2, 0.12, 0.08, 0.05]  # a plucked string's
REED = [1, 0, 0.5, 0, 0.3, 0, 0.18, 0, 0.1]           # odd harmonics (a clarinet, a square-ish tone)

cases = []


def case(id, op, input=None, **params):
    c = {"id": id, "op": op}
    c.update(params)
    if input is not None:
        c["input"] = input
    cases.append(c)


# ---- loudness: the meter (lm_*) and the report (ms_lr_*) on the same inputs

LOUDNESS = {
    "stereo-48k-sine": signal(48000, 5, 2, tone(997, 0.5)),
    "stereo-44k-noise-bass": signal(44100, 6, 2, noise(11, 0.25, 0, lowpass=3), noise(12, 0, 0.25, lowpass=3), tone(100, 0.2)),
    "mono-48k-voice": signal(48000, 4, 1, voiced(440, 0.4, SUNG, vibHz=5.5, vibDepth=0.01)),
    "stereo-96k-bright": signal(96000, 4, 2, tone(3000, 0.3, 0.2), noise(21, 0.05, 0.08, highpass=2)),
    "stereo-48k-quiet": signal(48000, 5, 2, noise(31, 0.003, 0.002, lowpass=2)),
    "stereo-44k-loud": signal(44100, 4, 2, voiced(55, 0.62, REED), noise(41, 0.1, 0.1)),
    "stereo-48k-silence": signal(48000, 3, 2),
    "mono-44k-short": signal(44100, 0.3, 1, tone(1000, 0.5)),
    "stereo-48k-half-second": signal(48000, 0.5, 2, tone(250, 0.4, 0.3)),
    "stereo-48k-dynamic": signal(48000, 12, 2,
                                 tone(220, 0.5, attack=0.01, decay=16, every=4),
                                 noise(51, 0.03, 0.03, lowpass=1, **{"from": 2})),
}
for name, sig in LOUDNESS.items():
    case(f"loudness/meter/{name}", "loudness-meter", sig, block=4096)
    case(f"loudness/report/{name}", "loudness-report", sig, block=4096)

# ---- true peak (ms_tp_*): peaks between the samples

TRUE_PEAK = {
    "quarter-rate-45deg-48k": (signal(48000, 1, 2, tone(12000, 1.0, phase=0.125)), False),
    "third-rate-48k": (signal(48000, 1, 1, tone(16000, 0.95, phase=0.1)), False),
    "near-nyquist-44k": (signal(44100, 1, 2, tone(19000, 0.8, 0.6, phase=0.3)), False),
    "sine-1k-mono-44k": (signal(44100, 1, 1, tone(1000, 0.5)), False),
    "high-96k": (signal(96000, 0.5, 2, tone(12000, 0.9, 0.7, phase=0.1)), False),
    "silence-48k": (signal(48000, 0.5, 2), False),
    "one-burst-48k": (signal(48000, 0.5, 2,
                             dict(type="samples", l=1, r=0.5, values=[0.5, 1, 1, 0.5, -0.5, -1, -1, -0.5, 0.25], **{"from": 0.25}),
                             noise(61, 0.01, 0.01)), True),
}
for name, (sig, position) in TRUE_PEAK.items():
    case(f"true-peak/{name}", "true-peak", sig, block=1024, **({"position": True} if position else {}))

# ---- pitch: the tuner (tp_*), across instruments' ranges, and a singing voice (pi_voice)

TUNER = [
    ("bass-5-low-B", 30.87, PLUCKED, 44100, 16384),
    ("bass-low-E", 41.20, PLUCKED, 48000, 16384),
    ("guitar-low-E", 82.41, PLUCKED, 48000, 8192),
    ("guitar-A", 110.0, PLUCKED, 44100, 8192),
    ("guitar-G", 196.0, PLUCKED, 48000, 8192),
    ("guitar-high-E", 329.63, PLUCKED, 48000, 4096),
    ("violin-A", 440.0, SUNG, 48000, 4096),
    ("a-442", 442.0, SUNG, 44100, 4096),
    ("clarinet-low", 146.83, REED, 48000, 8192),
    ("violin-E", 659.26, SUNG, 96000, 8192),
    ("flute-C6", 1046.5, [1, 0.3, 0.1], 48000, 4096),
    ("piccolo-C7", 2093.0, [1, 0.2], 48000, 4096),
    ("ukulele-flat", 261.0, PLUCKED, 44100, 4096),
]
for name, hz, harm, rate, length in TUNER:
    sig = signal(rate, length / rate + 0.05, 1, voiced(hz, 0.5, harm, decay=16))
    case(f"pitch/tuner/{name}", "tuner", sig, length=length)
case("pitch/tuner/silence", "tuner", signal(48000, 0.2, 1), length=4096)

VOICE = [
    ("bass-E2", 82.41, 48000), ("baritone-A2", 110.0, 44100), ("tenor-C3", 130.81, 48000), ("tenor-A3", 220.0, 48000),
    ("alto-E4", 329.63, 44100), ("soprano-A4", 440.0, 48000), ("soprano-C5", 523.25, 48000), ("soprano-G5", 783.99, 96000),
]
for name, hz, rate in VOICE:
    sig = signal(rate, 0.6, 1, voiced(hz, 0.3, SUNG, vibHz=5.5, vibDepth=0.015, attack=0.05),
                 noise(71, 0.002, lowpass=2))
    case(f"pitch/voice/{name}", "voice", sig, **{"from": 0.3}, length=2048 if rate < 90000 else 4096)
case("pitch/voice/breath", "voice", signal(48000, 0.2, 1, noise(72, 0.05, lowpass=1)), length=2048)

# ---- rhythm: the tempo finder (rh_tempo_*) and the onset detector (rh_onsets_*)


def beat(bpm, rate, seconds, seed):
    every = 60.0 / bpm
    return signal(rate, seconds, 1,
                  tone(60, 0.6, decay=11, every=every),
                  noise(seed, 0.25, highpass=1, decay=9, every=every / 2),
                  noise(seed + 1, 0.3, lowpass=1, decay=10, every=every * 2, **{"from": every}))


for bpm, rate in ((120, 48000), (95, 44100), (140, 48000), (87.5, 44100)):
    case(f"rhythm/tempo/{bpm:g}bpm-{rate // 1000}k", "tempo", beat(bpm, rate, 10, 80 + int(bpm)), block=4096)

case("rhythm/onsets/clicks-48k", "onsets",
     signal(48000, 3, 1, tone(2000, 0.5, decay=8, every=0.37, **{"from": 0.2})), block=512)
case("rhythm/onsets/drums-44k", "onsets", beat(110, 44100, 4, 90), block=441)
case("rhythm/onsets/legato-48k", "onsets",
     signal(48000, 3, 1,
            [tone(f, 0.3, mult=k + 1, **{"from": t, "to": t + 0.5})
             for t, f in ((0.2, 196.0), (0.7, 246.94), (1.2, 293.66), (1.7, 392.0), (2.2, 329.63)) for k in range(3)]),
     block=256)
case("rhythm/onsets/quiet-96k", "onsets",
     signal(96000, 2, 1, noise(95, 0.02, highpass=2, decay=11, every=0.5, **{"from": 0.1})), block=1024, sensitivity=80)

# ---- mastering (mt_*): a short mix through every style at the default target, and two other targets

MIX = signal(44100, 8, 2,
             tone(55, 0.45, decay=12, every=0.5),                            # kick
             voiced(55, 0.18, [1, 0.6, 0.3, 0.15], decay=15, every=1),       # bass
             [voiced(hz, 0.05, SUNG) for hz in (261.63, 329.63, 392.0)],      # a chord
             noise(101, 0.12, 0.08, highpass=1, decay=9, every=0.25),        # hats
             noise(102, 0.2, 0.25, lowpass=1, decay=11, every=1, **{"from": 0.5}),  # snare
             tone(880, 0.04, 0.07, vibHz=4, vibDepth=0.01))                  # a lead
STYLES = ["balanced", "heavy", "bright", "warm", "gentle", "wide", "arena", "drive"]
for i, name in enumerate(STYLES):
    case(f"mastering/{name}", "mastering", MIX, style=i, block=4096)
case("mastering/balanced-at-9", "mastering", MIX, style=0, target=-9, block=4096)
case("mastering/gentle-at-16", "mastering", MIX, style=4, target=-16, block=4096)

# ---- every sound: each metronome set and role, each drum sound, each kit's pads

for rate in (44100, 48000):
    case(f"sounds/clicks-{rate // 1000}k", "click-sounds", rate=rate)
    case(f"sounds/drums-{rate // 1000}k", "drum-sounds", rate=rate)
case("sounds/kits", "drum-kits")


# ---- writing


def build(work):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++")
    if not cxx:
        raise SystemExit("tools/reference.py needs a C++20 compiler (c++ or $CXX)")
    exe = os.path.join(work, "reference")
    subprocess.run([cxx, "-std=c++20", "-O2", "-DVS_NO_JUCE=1", "-D_USE_MATH_DEFINES", "-I", ROOT,
                    os.path.join(ROOT, "tests", "ReferenceTests.cpp"), "-o", exe], check=True)
    return exe


def write(results):
    lines = ["{",
             '  "about": "ENCORE. Audio Processing\'s reference set: inputs (recipes for dsp/ReferenceSignals.h) and the results every build must give, each within its tolerance. Written by tools/reference.py; checked by tests/ReferenceTests.cpp and wasm/reference.mjs.",',
             '  "tolerances": {']
    names = list(TOLERANCES)
    for i, n in enumerate(names):
        lines.append(f"    {json.dumps(n)}: {json.dumps(TOLERANCES[n])}" + ("," if i < len(names) - 1 else ""))
    lines.append("  },")
    lines.append('  "cases": [')
    by_id = {r["id"]: r for r in results}
    for i, c in enumerate(cases):
        r = by_id[c["id"]]
        head = dict(c)
        if "inputHash" in r:
            head["inputHash"] = r["inputHash"]
        text = json.dumps(head)[:-1] + ', "results": {'
        lines.append("    " + text)
        res = r["results"]
        for k, (name, value) in enumerate(res):
            if value is None:
                raise SystemExit(f"tools/reference.py: {c['id']} {name} is not a finite number")
            lines.append(f"      {json.dumps(name)}: [{json.dumps(value)}, {json.dumps(tolerance(c['op'], name))}]"
                         + ("," if k < len(res) - 1 else ""))
        lines.append("    }}" + ("," if i < len(cases) - 1 else ""))
    lines.append("  ]")
    lines.append("}")
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ids = [c["id"] for c in cases]
    if len(set(ids)) != len(ids):
        raise SystemExit("tools/reference.py: two cases have the same id")
    with tempfile.TemporaryDirectory() as work:
        exe = build(work)
        cases_path = os.path.join(work, "cases.json")
        out_path = os.path.join(work, "results.json")
        with open(cases_path, "w") as f:
            json.dump({"cases": cases}, f)
        subprocess.run([exe, "--compute", cases_path, out_path], check=True)
        with open(out_path) as f:
            results = json.load(f)
    write(results)
    print(f"wrote {os.path.relpath(OUT, ROOT)}: {len(cases)} cases, {sum(len(r['results']) for r in results)} results")


if __name__ == "__main__":
    sys.exit(main())
