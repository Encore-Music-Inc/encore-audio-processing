# ENCORE. Audio Processing
# Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
#
# The presets of ENCORE.'s audio chains (Vox, Verb, Equalize, Master, Strum):
# the starting points every product that offers a chain lists, in one place.
# Running this writes the two files made from it:
#   dsp/Presets.h        for C++
#   data/presets.json    for the browser
# CI runs it and fails if either file differs from what it writes.
#
#   python3 tools/presets.py
#
# Each preset: P(name, [(parameter id, value), ...]). A parameter a preset
# doesn't list is at its default; a choice is its position in the list
# (Master's style: 0 Balanced, 1 Heavy, 2 Bright, 3 Warm, MasterChain's
# order; its target: 0 Streaming, 1 Balanced, 2 Loud, MasterChain::targets).
# Values are written exactly as given, so every product reads the same number.

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

def P(name, values):
    return {"name": name, "values": list(values)}

# Each chain's presets, in the order a list shows them (the first is where the chain starts)
PRESETS = {
    "vox": [
        P("Clean Start", []),
        P("Folk Intimate", [("ride_target", "-20"), ("ride_range", "5"), ("ride_speed", "0.35"), ("hpf", "70"), ("deess", "0.35"), ("deess_freq", "6500"), ("drive", "0.1"), ("low", "1"), ("body", "-1"), ("presence", "1.5"), ("air", "2.5"), ("comp", "0.3"), ("comp_style", "0"), ("verb", "0.22"), ("verb_size", "0.55"), ("delay", "0")]),
        P("Punk Shout", [("ride_target", "-16"), ("ride_range", "8"), ("ride_speed", "0.7"), ("hpf", "110"), ("gate", "-55"), ("deess", "0.45"), ("deess_freq", "5500"), ("drive", "0.45"), ("low", "-1"), ("body", "-2"), ("presence", "3"), ("air", "1"), ("comp", "0.6"), ("comp_style", "1"), ("verb", "0.08"), ("verb_size", "0.3"), ("delay", "0.15"), ("delay_time", "0"), ("delay_fb", "0.1")]),
        P("Emo Lead", [("ride_target", "-17"), ("ride_range", "7"), ("ride_speed", "0.55"), ("hpf", "90"), ("gate", "-65"), ("deess", "0.45"), ("deess_freq", "6500"), ("drive", "0.25"), ("body", "-2"), ("presence", "2"), ("air", "3"), ("comp", "0.5"), ("comp_style", "1"), ("verb", "0.22"), ("verb_size", "0.6"), ("delay", "0.18"), ("delay_time", "3"), ("delay_fb", "0.3")]),
        P("Scream Control", [("ride_target", "-16"), ("ride_range", "10"), ("ride_speed", "0.85"), ("hpf", "120"), ("gate", "-50"), ("deess", "0.3"), ("deess_freq", "5000"), ("drive", "0.55"), ("low", "-2"), ("body", "-3"), ("presence", "1.5"), ("comp", "0.75"), ("comp_style", "2"), ("verb", "0.1"), ("verb_size", "0.35"), ("delay", "0.1"), ("delay_time", "0"), ("delay_fb", "0.05")]),
        P("Backing Vocals", [("ride_target", "-22"), ("ride_range", "6"), ("ride_speed", "0.4"), ("hpf", "150"), ("gate", "-60"), ("deess", "0.6"), ("deess_freq", "7000"), ("drive", "0.1"), ("low", "-3"), ("body", "-2"), ("presence", "-1"), ("air", "2"), ("comp", "0.5"), ("comp_style", "0"), ("verb", "0.3"), ("verb_size", "0.6"), ("delay", "0")]),
        P("Folk Harmony", [("ride_target", "-20"), ("ride_range", "5"), ("ride_speed", "0.35"), ("hpf", "70"), ("deess", "0.35"), ("deess_freq", "6500"), ("drive", "0.1"), ("low", "1"), ("body", "-1"), ("presence", "1.5"), ("air", "2.5"), ("comp", "0.3"), ("comp_style", "0"), ("verb", "0.25"), ("verb_size", "0.55"), ("delay", "0"), ("scale", "1"), ("tune_on", "1"), ("tune_speed", "120"), ("tune_amount", "0.5"), ("harm_on", "1"), ("h1_int", "7"), ("h1_level", "-9"), ("h1_pan", "-0.35"), ("h2_int", "0"), ("humanize", "0.6")]),
        P("Punk Gang Double", [("ride_target", "-16"), ("ride_range", "8"), ("ride_speed", "0.7"), ("hpf", "110"), ("gate", "-55"), ("deess", "0.45"), ("deess_freq", "5500"), ("drive", "0.45"), ("low", "-1"), ("body", "-2"), ("presence", "3"), ("air", "1"), ("comp", "0.6"), ("comp_style", "1"), ("verb", "0.1"), ("verb_size", "0.35"), ("delay", "0"), ("harm_on", "1"), ("h1_int", "1"), ("h1_level", "-5"), ("h1_pan", "-0.8"), ("h2_int", "1"), ("h2_level", "-5"), ("h2_pan", "0.8"), ("humanize", "0.8")]),
        P("Hard Tune", [("tune_on", "1"), ("tune_speed", "0"), ("tune_amount", "1"), ("drive", "0.2"), ("presence", "2"), ("air", "2"), ("comp", "0.5"), ("comp_style", "1"), ("verb", "0.15"), ("delay", "0.15"), ("delay_time", "2"), ("delay_fb", "0.25")]),
    ],
    "verb": [
        P("Clean Start", []),
        P("Vocal Plate", [("mode", "2"), ("mix", "0.25"), ("predelay", "30"), ("decay", "1.8"), ("size", "0.5"), ("tone", "9000"), ("lowcut", "250"), ("mod", "0.3"), ("duck", "0.3")]),
        P("Snare Room", [("mode", "0"), ("mix", "0.2"), ("predelay", "0"), ("decay", "0.8"), ("size", "0.35"), ("tone", "7000"), ("lowcut", "200"), ("width", "1.1"), ("mod", "0.15")]),
        P("Ballad Chamber", [("mode", "1"), ("mix", "0.25"), ("predelay", "15"), ("decay", "1.6"), ("size", "0.6"), ("tone", "7500"), ("lowcut", "200")]),
        P("Big Hall", [("mode", "3"), ("mix", "0.3"), ("predelay", "25"), ("decay", "3.2"), ("size", "0.75"), ("tone", "6500"), ("lowcut", "180"), ("width", "1.2"), ("mod", "0.35")]),
        P("Ducked Vocal Hall", [("mode", "3"), ("mix", "0.3"), ("predelay", "60"), ("decay", "2.8"), ("size", "0.65"), ("tone", "7000"), ("lowcut", "250"), ("duck", "0.7")]),
        P("Emo Ambient", [("mode", "4"), ("mix", "0.35"), ("predelay", "40"), ("decay", "8.0"), ("size", "0.9"), ("tone", "6000"), ("lowcut", "300"), ("width", "1.3"), ("mod", "0.6"), ("duck", "0.4"), ("shimmer", "0.35")]),
        P("Surf Spring", [("mode", "5"), ("mix", "0.35"), ("predelay", "0"), ("decay", "2.5"), ("size", "0.5"), ("tone", "4500"), ("lowcut", "200")]),
        P("Lo-fi Spring Vox", [("mode", "5"), ("mix", "0.2"), ("predelay", "10"), ("decay", "1.2"), ("size", "0.3"), ("tone", "3500"), ("lowcut", "400")]),
    ],
    "equalize": [
        P("Flat", []),
        P("Vocal Clean-Up", [("b1_on", "1"), ("b1_freq", "90"), ("b2_gain", "-1.5"), ("b3_gain", "-2"), ("b3_freq", "300"), ("b5_gain", "1.5"), ("b5_freq", "3200"), ("b7_gain", "2"), ("b7_freq", "11000")]),
        P("Acoustic Guitar", [("b1_on", "1"), ("b1_freq", "70"), ("b2_gain", "-2"), ("b2_freq", "150"), ("b3_gain", "-2.5"), ("b3_freq", "220"), ("b3_q", "1.4"), ("b6_gain", "1.5"), ("b6_freq", "5000"), ("b7_gain", "2.5"), ("b7_freq", "12000")]),
        P("Punk Guitar Wall", [("b1_on", "1"), ("b1_freq", "90"), ("b3_gain", "-3"), ("b3_freq", "400"), ("b3_q", "1.2"), ("b5_gain", "1.5"), ("b5_freq", "1800"), ("b8_on", "1"), ("b8_freq", "11000")]),
        P("Kick Punch", [("b1_on", "1"), ("b1_freq", "35"), ("b2_freq", "60"), ("b2_gain", "2.5"), ("b3_gain", "-4"), ("b3_freq", "350"), ("b3_q", "1.5"), ("b5_gain", "3"), ("b5_freq", "3500"), ("b5_q", "1.5")]),
        P("Snare Crack", [("b1_on", "1"), ("b1_freq", "80"), ("b3_freq", "200"), ("b3_gain", "2"), ("b4_freq", "900"), ("b4_gain", "-2.5"), ("b6_freq", "5500"), ("b6_gain", "2.5")]),
        P("Bass Tight", [("b1_on", "1"), ("b1_freq", "35"), ("b2_freq", "80"), ("b2_gain", "1.5"), ("b3_freq", "250"), ("b3_gain", "-2.5"), ("b5_freq", "1500"), ("b5_gain", "2"), ("b8_on", "1"), ("b8_freq", "9000")]),
        P("Guitars Carved by Vocal", [("b1_on", "1"), ("b1_freq", "90"), ("carve_on", "1"), ("carve_depth", "5"), ("carve_low", "250"), ("carve_high", "6000")]),
        P("Harsh Scream Tamer", [("s1_on", "1"), ("s1_freq", "2800"), ("s1_range", "6"), ("s1_thresh", "-14"), ("s2_on", "1"), ("s2_freq", "4500"), ("s2_range", "6"), ("s2_thresh", "-16"), ("b1_on", "1"), ("b1_freq", "100")]),
    ],
    "master": [
        P("Default (Balanced, Streaming)", []),
        P("Folk / Acoustic", [("style", "0"), ("intensity", "0.35"), ("target", "0")]),
        P("Punk / Loud", [("style", "1"), ("intensity", "0.6"), ("target", "2")]),
        P("Emo / Big Chorus", [("style", "1"), ("intensity", "0.45"), ("target", "1")]),
        P("Lo-fi Demo", [("style", "3"), ("intensity", "0.6"), ("target", "0"), ("tone_air", "-1.5")]),
        P("Clear Vocal Mix", [("style", "2"), ("intensity", "0.5"), ("target", "0")]),
    ],
    "strum": [
        P("Miked, gentle cleanup", []),
        P("Miked, boomy soundhole", [("boom", "0.75"), ("squeak", "0.4"), ("pick", "0.3")]),
        P("DI to mic (generic)", [("body_on", "1"), ("body_curve", "0"), ("body_amount", "1.0"), ("boom_on", "0"), ("squeak", "0.4"), ("pick", "0.35")]),
        P("DI to mic (learned)", [("body_on", "1"), ("body_curve", "1"), ("body_amount", "1.0"), ("boom_on", "0"), ("squeak", "0.4"), ("pick", "0.3")]),
        P("Strummed, wide double", [("boom", "0.4"), ("squeak", "0.4"), ("pick", "0.45"), ("double_on", "1"), ("width", "0.7")]),
        P("Fingerpicked, de-squeak", [("boom", "0.35"), ("squeak", "0.75"), ("pick_on", "0")]),
    ],
}

def check():
    for chain, presets in PRESETS.items():
        names = [p["name"] for p in presets]
        assert len(names) == len(set(names)), f"{chain}: a name is used twice"
        for p in presets:
            ids = [k for k, _ in p["values"]]
            assert len(ids) == len(set(ids)), f"{chain} {p['name']}: a parameter is set twice"
            for _, v in p["values"]:
                float(v)

def cpp_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'

HEADER = """// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// Made by tools/presets.py (edit the list there and run it, not this file).
//
// The presets of ENCORE.'s audio chains: for each chain (vox, verb,
// equalize, master, strum) its presets in list order, each a name and the
// parameter values it sets (by parameter id; a parameter it doesn't list is
// at its default, a choice is its position in the list).
#pragma once

#include <string_view>
#include <vector>

namespace vs::presets
{
    struct Value { const char* id; double value; };
    struct Preset { const char* name; std::vector<Value> values; };
    struct Chain { const char* id; std::vector<Preset> presets; };

    inline const std::vector<Chain>& chains()
    {
        static const std::vector<Chain> c {
@CHAINS@
        };
        return c;
    }

    // A chain's presets by its id (empty for an unknown id)
    inline const std::vector<Preset>& forChain (std::string_view id)
    {
        static const std::vector<Preset> none;
        for (const auto& c : chains()) if (id == c.id) return c.presets;
        return none;
    }
}
"""

def write_cpp():
    rows = []
    for chain, presets in PRESETS.items():
        rows.append("            { %s, {" % cpp_str(chain))
        for p in presets:
            vals = ", ".join("{ %s, %s }" % (cpp_str(k), v) for k, v in p["values"])
            rows.append("                { %s, { %s } }," % (cpp_str(p["name"]), vals))
        rows.append("            } },")
    body = HEADER.replace("@CHAINS@", "\n".join(rows))
    open(os.path.join(ROOT, "dsp", "Presets.h"), "w", encoding="utf-8", newline="\n").write(body)

def write_json():
    data = {chain: [{"name": p["name"], "values": {k: float(v) if "." in v else int(v) for k, v in p["values"]}} for p in presets]
            for chain, presets in PRESETS.items()}
    text = json.dumps(data, ensure_ascii=False, separators=(",", ":")) + "\n"
    open(os.path.join(ROOT, "data", "presets.json"), "w", encoding="utf-8", newline="\n").write(text)

if __name__ == "__main__":
    check()
    write_cpp()
    write_json()
    print(f"{sum(len(p) for p in PRESETS.values())} presets in {len(PRESETS)} chains")
