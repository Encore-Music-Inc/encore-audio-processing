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
    "sound-length": {"abs": 1, "rel": 0,
                     "why": "A sound's length in samples is the floor of the sample rate times a length in seconds; compilers that reorder arithmetic (MSVC /fp:fast, which the products build with) can round that product across a whole number, adding or losing one sample of the tail."},
    "sound-sum": {"abs": 0, "rel": 1e-4,
                  "why": "A sound's sum of |x|: synthesized with sin and exp, which maths libraries round differently in the last place, and one sample more or less of a tail (see sound-length)."},
    "sound-peak": {"abs": 0, "rel": 1e-5,
                   "why": "A sound's peak: rounding in sin and exp only (a tail sample is never the peak)."},
    "seconds": {"abs": 1e-6, "rel": 0,
                "why": "A time in seconds worked out from sample positions: rounding in the division only."},
    "ratio": {"abs": 1e-5, "rel": 0,
              "why": "A correlation or share between -1 and 1: sums of products in double, ordered differently by compilers."},
    "level": {"abs": 1e-6, "rel": 0,
              "why": "A linear level (DC offset, RMS, a difference, a waveform column): float or double sums, rounding only."},
    "spectrum": {"abs": 1e-4, "rel": 1e-5,
                 "why": "Spectrum values and their sums: a float FFT (Accelerate on Apple systems), windowing and smoothing; rounding differs by system."},
    "spectrum-db": {"abs": 0.001, "rel": 0,
                    "why": "Levels in dB read from averaged spectra (tonal balance, a learned body curve): the spectrum allowance through log10."},
    "music-math": {"abs": 0, "rel": 1e-12,
                   "why": "Hz and MIDI conversions in double: log2 and pow from the maths library, last-place differences only."},
    "proc-sum": {"abs": 0, "rel": 1e-3,
                 "why": "A processed signal's sum of |x|: filters, envelopes, oversampling and pitch shifting in float, which compilers order and fuse differently, over seconds of audio; 1e-3 is 0.009 dB, far below any audible or setting change."},
    "proc-peak": {"abs": 0, "rel": 1e-3,
                  "why": "A processed signal's peak: as proc-sum."},
    "vocoder-peak": {"abs": 0, "rel": 0.1,
                     "why": "A pitch-shifted signal's single loudest sample: the phase vocoder adds up phases over the whole stretch, and float rounding (which compilers order differently) moves where the waves line up, so one sample's height can move a few per cent while the signal's sum and loudness hold to proc-sum and proc-lufs."},
    "proc-lufs": {"abs": 0.005, "rel": 0,
                  "why": "A processed signal's integrated loudness, LUFS: as proc-sum."},
    "box": {"abs": 1e-6, "rel": 1e-6,
            "why": "A block averaged down (the tuner's): double sums of a few samples rounded to float, which compilers may order differently, so a value can move by a float step."},
    "law": {"abs": 1e-6, "rel": 1e-6,
            "why": "Gains and positions from the pan, fader, fade and automation laws and the drawing axes: float sin, cos, pow, exp and log, which maths libraries round differently in the last place."},
    "lpc": {"abs": 1e-7, "rel": 1e-7,
            "why": "Linear prediction's coefficients and errors: double-precision sums and a cosine window, which maths libraries and compilers round differently in the last places, and the recursion through the orders magnifies that on a block whose predictors are nearly ill-conditioned (MSVC's /fp:fast moves them by about 1e-8). An encoder quantizes the coefficients far more coarsely than this."},
    "dsd": {"abs": 1e-6, "rel": 1e-6,
            "why": "DSD to PCM: the low-pass is designed in double with sin and cos, which maths libraries round differently in the last place, so its float tables can differ in the last place, and a sample (a sum of up to 512 of them) by a few float steps."},
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
    "click-sounds": {"length": "sound-length", "sum": "sound-sum", "peak": "sound-peak"},
    "drum-sounds": {"length": "sound-length", "sum": "sound-sum", "peak": "sound-peak"},
    "drum-kits": {"sound": "exact", "slot": "exact", "note": "exact", "choke": "exact"},
}


EXACT_WORDS = {"count", "length", "channels", "maxAt", "first", "last", "outcome", "latency", "runs", "longest", "spots",
               "empty", "identical", "silentL", "silentR", "silent", "hasSpectrum", "hasPadded16", "padded16", "clipHasLevel",
               "dithering", "onGrid", "resampledLength", "hash", "tpOverCount", "tpOverZeroCount", "clipRuns", "clipEvents",
               "clipLongest", "oversFloat", "signature", "valid", "others"}


def rule(op, name):
    """The tolerance of the newer ops' results, by what each result is."""
    parts = name.split(".")
    last = parts[-1]
    stem = last.rstrip("0123456789")
    if op == "lpc":
        return "exact" if last == "orders" else "lpc"
    if last in EXACT_WORDS or stem in ("pad", "midi", "trim"):
        return "exact"
    if op == "laws":
        return "exact" if parts[0] in ("tempo", "tuner") else "law"
    if op == "levels":
        return {"rms": "level", "gainDb": "db", "sum": "proc-sum", "peak": "proc-peak", "lufs": "proc-lufs"}.get(last, "exact")
    if op in ("processor", "host-mix", "fades", "channels", "resample", "live-resample", "fracdelay", "decimate", "devices", "vocal-eq", "ab", "effect", "vocoder", "instrument"):
        if op == "vocoder" and last == "peak":
            return "vocoder-peak"
        if last in ("sum", "peak", "lufs"):
            return {"sum": "proc-sum", "peak": "proc-peak", "lufs": "proc-lufs"}[last]
        if op == "processor" and parts[0] == "curve":
            return "spectrum-db"
        if stem in ("magnitude", "gainDb", "matchDb", "reachedLufs", "measuredLufs", "peakOf"):
            return "db" if stem != "peakOf" else "exact"
    if op == "processor" or op == "follow":
        return {"f0": "hz", "confidence": "clarity"}.get(parts[0], "proc-sum")
    if op == "quantize":
        return "exact"
    if op == "file-analysis":
        if parts[0] in ("timeline",):
            return "lufs"
        if parts[0] in ("tpOvers", "tpOverZero", "clipTimes", "lowOut") or last.endswith("At") or last.endswith("Sec") or last == "duration":
            return "seconds"
        if last in ("lufs", "shortTermMax"):
            return "lufs"
        if last == "lra":
            return "lu"
        if last in ("corr", "lowCorr"):
            return "ratio"
        if last in ("dcL", "dcR", "maxDiff"):
            return "level"
        if last == "cliffHz":
            return "hz"
        return "db"
    if op in ("spectrum", "welch"):
        return "spectrum"
    if op == "tonal-balance":
        return "spectrum-db"
    if op == "minmax":
        return "exact"
    if op in ("waveform", "block-peaks"):
        return "exact" if parts[0] == "shape" else "level"
    if op == "clip-runs":
        return "seconds"
    if op == "music":
        return "music-math" if stem in ("hzToMidi", "midiToHz") else "exact"
    if op == "meter":
        # Subdivision fractions are divisions (3/5, 5/6): MSVC's /fp:fast rounds them in the
        # last place; every other meter result is a whole number or a word.
        return "music-math" if parts[0] == "sub" and stem == "f" else "exact"
    if op in ("scale-library", "theory"):
        return "exact"
    if op == "downsample":
        return "box"
    if op == "dsd":
        return "exact" if last == "rate" else "lufs" if last == "lufs" else "dsd"
    if op == "limiter":
        return {"sum": "proc-sum", "peak": "proc-peak", "lufs": "proc-lufs", "truePeak": "proc-peak"}[last]
    raise SystemExit(f"tools/reference.py: no tolerance for {op} result {name}")


def tolerance(op, name):
    if op not in FIELDS:
        return rule(op, name)
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


# ---- measurement the browser also runs: file analysis (Last Call), spectra, waveforms, the clipping check

SONG = MIX
CLIPPED = signal(44100, 4, 2, voiced(110, 0.7, REED, decay=15, every=0.5), noise(111, 0.3, 0.3, lowpass=1),
                 dict(type="clip", l=0.95))
# A steady tone peaks between the same samples every cycle, so where its true peak is would be a tie;
# a short burst on top gives each of these one clear loudest peak.
BURST = dict(type="samples", values=[0.3, 0.6, 0.6, 0.3], **{"from": 3})
SPARSE = signal(48000, 6, 1, tone(440, 0.3, **{"from": 1, "to": 5}), tone(0, 0.01, phase=0.25), dict(BURST, l=1))  # silence each end, DC
OUT_OF_PHASE = signal(48000, 5, 2, tone(60, 0.4, 0), tone(60, 0, 0.4, phase=0.5), voiced(440, 0.1, SUNG), dict(BURST, l=0.8, r=0.8))
CLIFF = signal(44100, 6, 2, [tone(250, 0.012, mult=k) for k in range(1, 61)], noise(121, 0.001, 0.001))  # nothing above 15 kHz
TWIN = signal(48000, 3, 2, voiced(220, 0.3, PLUCKED, decay=14, every=0.75))                         # both channels the same

for name, sig, bits in (("song-24bit", SONG, 24), ("clipped-16bit", CLIPPED, 16), ("sparse-mono", SPARSE, 0),
                        ("out-of-phase-lows", OUT_OF_PHASE, 24), ("cliff", CLIFF, 16), ("twin-channels", TWIN, 0),
                        ("silence", signal(48000, 2, 2), 24)):
    case(f"analysis/file/{name}", "file-analysis", sig, bitDepth=bits, block=8192)

TONES = signal(48000, 1, 1, tone(1000, 0.4), tone(5000, 0.1), noise(131, 0.01))
case("analysis/spectrum/hann-update", "spectrum", TONES, block=1024, mode="update", size=4096, history=8192, window=0,
     bins=[0, 85, 427, 2047], levelAt=[100, 1000, 5000])
case("analysis/spectrum/periodic-analyse", "spectrum", TONES, block=2048, mode="analyse", size=4096, history=4096, window=1, bins=[85, 427])
case("analysis/spectrum/blackman-smoothed", "spectrum", TONES, block=512, mode="update", size=2048, history=4096, window=2,
     smoothing=0.5, bins=[43, 213])
case("analysis/spectrum/web-audio", "spectrum", TONES, block=1024, mode="analyse", webAudio=1, size=2048, smoothing=0.8, bins=[43, 213, 1023])
case("analysis/welch/song", "welch", SONG, size=4096, maxSegments=16, bins=[1, 10, 100, 1000])
case("analysis/welch/at", "welch", SONG, size=2048, starts=[0, 44100, 88200, 300000], bins=[1, 10, 100])
case("analysis/tonal-balance/song", "tonal-balance", SONG)
case("analysis/waveform/song", "waveform", SONG, block=4096, columns=16, rms=1, bands=1)
case("analysis/waveform/mono-stride", "waveform", SPARSE, block=4096, columns=12, stride=4)
case("analysis/block-peaks/song", "block-peaks", SONG, block=4096, blockSize=441, points=200, width=64)
case("analysis/clip-runs/clipped", "clip-runs", CLIPPED, bitDepth=16)
case("analysis/clip-runs/song", "clip-runs", SONG, bitDepth=24)

# ---- processing the browser also runs

VOX = signal(48000, 3, 1, voiced(201, 0.3, SUNG, vibHz=5, vibDepth=0.01, attack=0.05),
             noise(141, 0.35, highpass=1, decay=11, every=0.6, **{"from": 0.3}),   # esses
             noise(142, 0.004, lowpass=2))                                         # breath
HIGH = signal(96000, 1, 1, tone(1000, 0.3), tone(30000, 0.2), noise(151, 0.02))
case("processing/resample/48k-to-44k", "resample", VOX, block=4096, to=44100)
case("processing/resample/44k-to-48k", "resample", SONG, block=4096, to=48000)
case("processing/resample/96k-to-48k", "resample", HIGH, block=1000, to=48000)
case("processing/live-resample/44k-to-48k", "live-resample", SONG, step=44100 / 48000, block=512)
case("processing/live-resample/double-speed", "live-resample", SONG, step=2.0, block=256)
case("processing/levels/song", "levels", SONG)
case("analysis/minmax/song", "minmax", SONG, size=256, columns=40, block=1000)
case("processing/laws", "laws", pans=[-1, -0.5, 0, 0.3, 1], positions=[0, 0.05, 0.5, 1], dbs=[-100, -60, -12, 0, 6],
     ts=[0, 0.25, 0.5, 0.9, 1], curves=[0, 0.25, -0.4, 0.5, 0.75, -0.9, 1], xs=[0.1, 0.5, 0.8],
     bpms=[10, 20, 120, 400, 500], hzs=[20, 100, 1000, 20000], rates=[44100, 48000, 96000],
     playback=[-8, -0.5, -14, 1, -18, -3, -14, 1, -18, -3, -14, 0, -20, -1.5, -16, 1, -14, -1, -14, 1])
case("processing/lpc/song", "lpc", SONG, size=4096, order=12, blocks=3)
case("processing/fracdelay/song", "fracdelay", SONG, block=4096, delays=[0.5, 2.25])
case("processing/decimate/by-2", "decimate", HIGH, block=4096, factor=2)
case("processing/decimate/by-4", "decimate", HIGH, block=999, factor=4)
for bits, mode in ((16, 0), (16, 1), (16, 2), (24, 0)):
    case(f"processing/quantize/{bits}bit-{['auto', 'always', 'never'][mode]}", "quantize", SONG, block=4096, bits=bits, mode=mode, seed=7)
for curve in range(5):
    case(f"processing/fades/seconds-curve{curve}", "fades", VOX, kind="seconds", curve=curve, **{"in": 0.5, "out": 1.2})
for point in range(3):
    case(f"processing/fades/samples-point{point}", "fades", VOX, kind="samples", curve=point, point=point, **{"in": 4800, "out": 9600})
case("processing/fades/crossfade", "fades", SONG, kind="crossfade", curve=1, point=1, length=2000)
case("processing/channels/song", "channels", SONG, trim=[0.5, 2.0, 1.25, 0, 7.5, 9])
case("processing/channels/vox", "channels", VOX, trim=[0, 1])
case("processing/devices/song", "devices", SONG, block=4096, freqs=[100, 1000, 10000])
case("processing/vocal-eq/vox", "vocal-eq", VOX, block=512, lowCutHz=90, lowDb=-2, bodyDb=1.5, presenceDb=3, airDb=2,
     freqs=[60, 200, 3000, 12000])
case("processing/ab/vox", "ab", VOX, block=512)
case("processing/ab/song-latency", "ab", SONG, block=1024, latency=64)

# ---- the effects and instruments every product offers
for fx, params in (("chorus", dict(depthMs=4, speedHz=0.8, width=0.6, mix=0.5)), ("phaser", dict(depth=5, rate=0.5, feedback=0.6)),
                   ("filter", dict(type=4, hz=900, q=0.7071, slope=1)), ("comp", dict(threshold=-24, ratio=4, attackMs=5, releaseMs=120, makeup=3)),
                   ("delay", dict(amount=0.5, time=3, bpm=128, feedback=0.4)), ("reverb", dict(mode=3, decay=2.2, mix=0.35))):
    case(f"effects/{fx}/song", "effect", SONG, block=1024, fx=fx, **params)
case("effects/pitch/vox-up-octave", "vocoder", VOX, block=512, fx="pitch", semitones=12)
case("effects/pitch/vox-down-fifth", "vocoder", VOX, block=1024, fx="pitch", semitones=-7)
case("instruments/synth/phrase", "instrument", None, rate=48000, block=512, instrument="synth", seconds=2.5)
case("instruments/sampler/vox", "instrument", VOX, block=512, instrument="sampler", seconds=2.5)

# ---- the tuner's block averaging (tp_downsample)

case("pitch/downsample/voice", "downsample", signal(48000, 0.5, 1, voiced(220, 0.4, SUNG, vibHz=5, vibDepth=0.01), noise(201, 0.01)),
     strides=[1, 2, 3, 4, 6], at=[0, 100, 1000, 3999])

# ---- the beat grid's meter maths (rh_* meters, sd_clean_settings)

CLEAN_DEFAULT = [120, 4, 4, 1, 50, 0, 0, 3, 0, 4, 4, 0, 80, 4, 4, 120, 0, 5]


def clean(changes, **lists):
    s = list(CLEAN_DEFAULT)
    for k, v in changes.items():
        s[k] = v
    return dict(s=s, **lists)


case("rhythm/meter", "meter", tops=list(range(1, 18)), bottoms=[2, 4, 8, 16],
     parse=[["3+2+2", 7], ["2 2 3", 7], ["3+3", 7], ["0+7", 7], ["", 4], ["4", 4], ["3.5+3.5", 7], ["x3y4", 7], ["9" * 400, 9]],
     swings=[50, 60, 66.7, 75, 90, 0],
     bpms=[10, 24, 39.99, 50, 65.9, 66, 100, 119.99, 120, 175, 200, 399],
     clean=[clean({}),
            clean({1: 7, 2: 8}, groups=[3, 2, 2], accents=[3, 1, 1, 2, 1, 2, 1], sub="16"),
            clean({0: 500, 1: 20, 2: 3, 3: 2, 4: 80, 5: 150, 7: 0, 9: 0, 10: -2}, groups=[], accents=[], sub="x"),
            clean({0: 10, 1: 6, 2: 8, 3: 3, 6: 1, 7: 17, 8: 1, 11: 2}, groups=[3, 3], sub="8"),
            clean({1: 5, 2: 8, 3: 2}, groups=[2, 2], accents=[9, -1, 2, 2, 2], sub="3")])

# ---- DSD to PCM (pr_dsd_*): a sine through a one-bit modulator, and random bytes, at each output rate

case("processing/dsd/dsd64-sine", "dsd", signal(2822400, 0.25, 2, tone(1000, 0.5, 0.25)), kind="sine", block=4096,
     at=[0, 500, 10000, 22000])
case("processing/dsd/dsd128-sine", "dsd", signal(5644800, 0.1, 1, tone(10000, 0.4)), kind="sine", block=1000, at=[300, 8000])
case("processing/dsd/dsd256-sine", "dsd", signal(11289600, 0.1, 2, tone(997, 0.3), tone(15000, 0.1, 0.2)), kind="sine", block=8192,
     at=[300, 17000])
case("processing/dsd/dsd64-random", "dsd", signal(2822400, 0.05, 2, noise(211, 1, 1)), kind="sign", block=777, at=[200, 1000])

# ---- the mastering chain's true-peak limiter on its own (pr_limiter_*): hot mixes held to the ceiling

HOT = [voiced(110, 0.9, REED, decay=14, every=0.5), noise(221, 0.5, 0.4, lowpass=1), tone(5000, 0.3, 0.35, phase=0.1)]
for rate in (44100, 48000, 96000):
    case(f"processing/limiter/hot-{rate // 1000}k", "limiter", signal(rate, 3, 2, *HOT), ceiling=-1, block=1024)
case("processing/limiter/mono-slow-release", "limiter", signal(48000, 3, 1, *HOT), ceiling=-0.3, release=150, block=512)

# ---- music: note names, keys, spelling (the scale library is data/scales.json in the browser, so natively only)

case("music/words", "music", parse=["F#", "Bb", "Cbb", "Gx", "H", "F# minor", "Bbm", "C#/Db major", "e"],
     hz=[27.5, 100, 440, 1000, 4186], midi=[21, 60, 69, 108])
case("music/scale-library", "scale-library", native=True)

# ---- the chord and scale theory: requests to dsp/TheoryCalls.h (th_call in the browser), each
# answer as JSON text with its numbers rounded to nine decimals, so every build writes the same text

GUITAR_COURSES = [{"midi": [m]} for m in (40, 45, 50, 55, 59, 64)]
UKULELE_COURSES = [{"midi": [m]} for m in (67, 60, 64, 69)]
BANJO_COURSES = [{"midi": [67], "start": 5}] + [{"midi": [m]} for m in (50, 55, 59, 62)]
# a saved vocal range, and random numbers for the exercises that draw them (a fixed stream)
VOICE_RANGE = {"low": 47, "high": 69, "tessLo": 52, "tessHi": 63}
RAND = [round((k * 0.6180339887498949) % 1, 9) for k in range(1, 251)]

THEORY = [
    {"fn": "degree", "d": "#11"}, {"fn": "above", "root": "Eb", "d": "b7"},
    {"fn": "intervalBetween", "a": "C", "b": "F#"}, {"fn": "moveBy", "note": "Bb", "steps": 2, "semis": 4},
    {"fn": "chordTones", "chord": "F#m7b5/C"}, {"fn": "chordSymbol", "chord": {"root": {"L": 6, "a": -1}, "type": "maj9#11", "bass": None}},
    {"fn": "parseChord", "text": "C\u00b07"}, {"fn": "parseChord", "text": "H7"}, {"fn": "chordPcs", "chord": "E7#9"},
    {"fn": "spellRoot", "pc": 6, "type": "min", "side": -1}, {"fn": "spellBass", "pc": 8, "root": "E", "keyTonic": "A", "side": 0},
    {"fn": "nameNotes", "midis": [48, 52, 55, 58]}, {"fn": "nameNotes", "midis": [40, 55, 60, 64]},
    {"fn": "nameNotes", "midis": [50, 54, 57, 60, 64], "keyTonic": "G"}, {"fn": "nameNotes", "midis": [48, 51]},
    {"fn": "nameNotes", "midis": [61, 65, 68], "side": 1}, {"fn": "nameNotes", "midis": [60]},
    {"fn": "numeralChord", "key": "C major", "rn": "vii\u00b07/V"}, {"fn": "numeralChord", "key": "A minor", "rn": "bVI"},
    {"fn": "numeralChord", "key": "G major", "rn": "I/3"}, {"fn": "plainNumeral", "key": "C major", "chord": "F#m7b5"},
    {"fn": "inversionPart", "chord": "C/Bb"}, {"fn": "numeralOf", "key": "C major", "chord": "C7", "next": "F"},
    {"fn": "numeralOf", "key": "C major", "chord": "C7", "next": "G"},
    {"fn": "keyPalette", "key": "C major"}, {"fn": "keyPalette", "key": "F# minor"}, {"fn": "keyPalette", "key": "Db major"},
    {"fn": "modulations", "key": "Eb major"}, {"fn": "modulations", "key": "E minor"},
    {"fn": "relate", "key": "C major", "from": "G7", "to": "C"}, {"fn": "relate", "key": "C major", "from": "G7", "to": "Am"},
    {"fn": "relate", "key": "C major", "from": "F", "to": "Fm"}, {"fn": "relate", "key": "C major", "from": "Db7", "to": "C"},
    {"fn": "relate", "key": "A minor", "from": "E7", "to": "Am"}, {"fn": "relate", "key": "C major", "from": "Bb7", "to": "C"},
    {"fn": "nextMoves", "key": "C major", "from": "G7"}, {"fn": "nextMoves", "key": "B minor", "from": "Bm", "limit": 12},
    {"fn": "keyInterval", "from": "C major", "to": "Ab major"}, {"fn": "transposeChord", "chord": "D7/F#", "steps": 2, "semis": 3},
    {"fn": "keyBySemis", "key": "Eb minor", "semis": 5},
    {"fn": "frettable", "name": "Guitar (6-string)", "part": "string", "tunings": [[40, 45, 50, 55, 59, 64]]},
    {"fn": "frettable", "name": "Erhu", "part": "string", "tunings": [[62, 69]]},
    {"fn": "coursesOf", "midi": [67, 50, 55, 59, 62], "name": "Banjo (5-string)"},
    {"fn": "coursesOf", "midi": [55, 67, 62, 74, 69, 81, 76, 88], "courses": [2, 2, 2, 2], "name": "Mandolin"},
    {"fn": "courseNotes", "courses": BANJO_COURSES, "course": 0, "fret": 3, "capo": 0},
    {"fn": "shapeNotes", "courses": GUITAR_COURSES, "frets": [-1, 3, 2, 0, 1, 0], "capo": 2},
    {"fn": "ascending", "courses": UKULELE_COURSES}, {"fn": "fingering", "frets": [1, 3, 3, 2, 1, 1]},
    {"fn": "fretVoicings", "chord": "C", "courses": GUITAR_COURSES}, {"fn": "fretVoicings", "chord": "F#m7b5", "courses": GUITAR_COURSES, "limit": 8},
    {"fn": "fretVoicings", "chord": "G/B", "courses": GUITAR_COURSES, "capo": 2, "limit": 6},
    {"fn": "fretVoicings", "chord": "A7", "courses": UKULELE_COURSES}, {"fn": "fretVoicings", "chord": "D", "courses": BANJO_COURSES, "limit": 6},
    {"fn": "shapeText", "frets": [-1, 10, 12, 12, 11, 10]}, {"fn": "pianoVoicing", "chord": "Cmaj7"},
    {"fn": "pianoVoicing", "chord": "G7/B", "prev": [48, 60, 64, 67, 71]}, {"fn": "shapeFor", "chord": "Eb", "capo": 3},
    {"fn": "capoOptions", "chords": ["Eb", "Ab", "Bb", "Cm"], "courses": GUITAR_COURSES},
    {"fn": "spellScale", "scale": "harmonic_minor", "root": "G#"}, {"fn": "spellScale", "scale": "blues", "root": "Eb"},
    {"fn": "spellScale", "scale": "whole_tone", "root": "Db"}, {"fn": "readCost", "scale": "ionian", "root": "C#"},
    {"fn": "easyRoot", "scale": "aeolian", "pc": 3}, {"fn": "simplerRoot", "scale": "ionian", "root": "D#"},
    {"fn": "stepSizes", "scale": "hungarian_minor"}, {"fn": "stepText", "scale": "hungarian_minor"},
    {"fn": "compareScales", "a": "dorian", "b": "aeolian"}, {"fn": "compareScales", "a": "blues", "b": "minor_pent"},
    {"fn": "scalesWithSemis", "semis": [0, 2, 3, 5, 7, 9, 10]}, {"fn": "familyOf", "scale": "altered"},
    {"fn": "rotations", "scale": "major_pent", "root": "G"}, {"fn": "parentOf", "scale": "lydian_dominant", "root": "F"},
    {"fn": "familyModes", "scale": "phrygian", "root": "E"}, {"fn": "familyModes", "scale": "blues", "root": "E"},
    {"fn": "typeForSemis", "semis": [0, 3, 6, 9]}, {"fn": "scaleChords", "scale": "harmonic_minor", "root": "A"},
    {"fn": "intervalDeg", "a": "C", "b": "A#"}, {"fn": "chordsInside", "scale": "minor_pent", "root": "A"},
    {"fn": "readChord", "text": "Bb7alt"}, {"fn": "readChord", "text": "Am7/G"},
    {"fn": "readProgression", "text": "Am7, D7 | Gmaj7 Xyz"},
    {"fn": "fitOver", "scale": "ionian", "root": "C", "chord": "Cmaj7"}, {"fn": "fitOver", "scale": "altered", "root": "G", "chord": "G7alt"},
    {"fn": "scalesForChord", "chord": "G7"}, {"fn": "scalesForChord", "chord": "Cm6", "limit": 4},
    {"fn": "scalesForChord", "chord": "D7", "limit": 4, "context": [7, 9, 11, 0, 2, 4, 6]},
    {"fn": "scalesForProgression", "chords": ["Am7", "D7", "Gmaj7"]}, {"fn": "scalesForProgression", "chords": ["C7", "F7", "C7", "G7"]},
    {"fn": "scalesForProgression", "chords": ["Am", "G", "F", "E"]}, {"fn": "scalesForProgression", "chords": ["Bm7b5", "E7b9", "Am7"]},
    {"fn": "neckNotes", "scale": "minor_pent", "root": "A", "courses": GUITAR_COURSES, "frets": 12},
    {"fn": "boxPositions", "scale": "minor_pent", "root": "A", "courses": GUITAR_COURSES, "standard": True},
    {"fn": "boxPositions", "scale": "dorian", "root": "D", "courses": GUITAR_COURSES, "capo": 2, "standard": True},
    {"fn": "boxPositions", "scale": "ionian", "root": "C", "courses": UKULELE_COURSES},
    {"fn": "perStringPatterns", "scale": "ionian", "root": "G", "courses": GUITAR_COURSES},
    {"fn": "perStringPatterns", "scale": "major_pent", "root": "E", "courses": GUITAR_COURSES, "frets": 19},
    {"fn": "isStandardSix", "courses": GUITAR_COURSES}, {"fn": "upAndDown", "scale": "ionian", "root": "C", "around": 52},
    {"fn": "nope"}, {"fn": "chordSymbol", "chord": "H"},
    # a song's key, a capo for a shift and the voice exercises (random numbers given in order)
    {"fn": "guessKey", "chords": ["Am", "F", "C", "G", "Am"]}, {"fn": "guessKey", "chords": ["G", "D/F#", "Em", "C", "D7", "G"]},
    {"fn": "guessKey", "chords": ["B#", "C"]}, {"fn": "guessKey", "chords": []},
    {"fn": "capoForShift", "semis": 5}, {"fn": "capoForShift", "semis": -3},
    {"fn": "keyShiftsForRange", "songLow": 60, "songHigh": 72, "range": VOICE_RANGE, "originalPc": 0},
    {"fn": "keyShiftsForRange", "songLow": 55.5, "songHigh": 81, "range": VOICE_RANGE},
    {"fn": "nextMatchNote", "level": 4, "range": VOICE_RANGE, "prev": 55, "rand": RAND},
    {"fn": "buildSingBack", "level": 5, "range": VOICE_RANGE, "rand": RAND},
    {"fn": "buildWarmup", "range": VOICE_RANGE, "full": True}, {"fn": "buildWarmup", "range": {"low": 50, "high": 56, "tessLo": 52, "tessHi": 54}},
]
case("music/theory", "theory", requests=[json.dumps(r, separators=(",", ":")) for r in THEORY])

# ---- processors the browser build has no functions for: natively only ("native"; wasm/reference.mjs
# skips them and says how many)

CLICKY = signal(48000, 2.5, 2, tone(1500, 0.5, decay=8, every=0.6), noise(161, 0.3, 0.2, decay=9, every=1.1, **{"from": 0.3}))
GUITAR = signal(48000, 6, 1, voiced(110, 0.35, PLUCKED, decay=14, every=0.5),
                noise(171, 0.2, highpass=1, decay=7, every=0.5),                     # pick attacks
                noise(172, 0.05, highpass=2, decay=11, every=1.5, **{"from": 0.25}))  # string squeaks


def native(id, input, kind, **params):
    case(id, "processor", input, kind=kind, native=True, **params)


for style in range(3):
    native(f"native/compressor/{['smooth', 'punchy', 'slam'][style]}", SONG, "compressor", style=style, amount=0.6, block=512)
native("native/gate/clicks", CLICKY, "gate", threshold=-35, block=512)
native("native/rider/vox", VOX, "rider", target=-20, range=6, speed=0.5, block=512)
native("native/dynamic-bell/vox", VOX, "dynamic-bell", freq=3500, q=2, cut=6, block=256)
for t, (freq, gain, q) in enumerate(((1000, 4, 1.0), (120, 3, 0.7), (6000, -3, 0.7), (80, 0, 0.7), (12000, 0, 0.7), (2000, 0, 2))):
    native(f"native/eq-band/{['bell', 'low-shelf', 'high-shelf', 'low-cut', 'high-cut', 'notch'][t]}", SONG, "eq-band",
           type=t, freq=freq, gain=gain, q=q, block=512)
native("native/eq-band/low-cut-48dB", SONG, "eq-band", type=3, freq=100, slope=2, block=512)
native("native/dynamic-band/vox", VOX, "dynamic-band", freq=6000, range=9, threshold=-40, block=512)
native("native/carve/song", SONG, "carve", depth=6, block=512)
native("native/saturator/song", SONG, "saturator", amount=0.5, block=512)
native("native/doubler/guitar", GUITAR, "doubler", amount=0.7, block=512)
for mode in range(6):
    native(f"native/reverb/{['room', 'chamber', 'plate', 'hall', 'huge', 'spring'][mode]}", CLICKY, "reverb", mode=mode, block=512)
native("native/reverb/space-setting", CLICKY, "reverb", space=1, size=0.3, block=512)
native("native/space/vox", VOX, "space", amount=0.6, size=0.4, delay=0.3, delaySeconds=0.25, feedback=0.3, block=512)
for kind in ("squeak", "pick", "boom"):
    native(f"native/guitar-fixes/{kind}", GUITAR, kind, amount=0.8, block=512)
for i, name in enumerate(STYLES):
    native(f"native/master-chain/{name}", SONG, "master-chain", style=i, block=1024)
case("native/follow/vox", "follow", VOX, hop=4800, native=True)
native("native/de-esser/vox", VOX, "de-esser", amount=1.0, freq=6500, block=512)
native("native/pitch-engine/correction", VOX, "pitch-engine", key=0, scale=0, speedMs=20, block=512)
native("native/pitch-engine/harmony", VOX, "pitch-engine", key=0, scale=0, speedMs=20, harmony=1, interval=3, block=512)
native("native/body/guitar", GUITAR, "body", block=512)
case("native/host-mix/song", "host-mix", SONG, block=512, native=True)


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


def compute(exe, cases_path, work):
    out_path = os.path.join(work, "results.json")
    subprocess.run([exe, "--compute", cases_path, out_path], check=True)
    with open(out_path) as f:
        return json.load(f)


def main():
    ids = [c["id"] for c in cases]
    if len(set(ids)) != len(ids):
        raise SystemExit("tools/reference.py: two cases have the same id")
    with tempfile.TemporaryDirectory() as work:
        exe = build(work)
        cases_path = os.path.join(work, "cases.json")
        with open(cases_path, "w") as f:
            json.dump({"cases": cases}, f)
        results = compute(exe, cases_path, work)
    write(results)
    print(f"wrote {os.path.relpath(OUT, ROOT)}: {len(cases)} cases, {sum(len(r['results']) for r in results)} results")


if __name__ == "__main__":
    sys.exit(main())
