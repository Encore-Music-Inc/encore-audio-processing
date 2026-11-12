# ENCORE. Audio Processing
# Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
#
# The chord types: every chord ENCORE.'s products name, with its symbol,
# formula and how it reads in a roman numeral, and the order a player looks
# for them in. Running this writes the two files made from it:
#   dsp/ChordTypes.h     for C++
#   data/chords.json     for the browser
# CI runs it and fails if either file differs from what it writes.
#
#   python3 tools/chords.py
#
import json
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# id, symbol (what follows the root: "m7b5"), name, formula (degrees), cost (how rarely the
# name is the right one: lower wins when a set of notes has several names), family (decides a
# numeral's case and the chord's function), numeral suffix (what follows a roman numeral, whose
# case already says major or minor), optional degrees (can be left out and it's still the chord)
TYPES = [
    ("maj", "", "major", ["1", "3", "5"], 0, "maj", "", []),
    ("min", "m", "minor", ["1", "b3", "5"], 0, "min", "", []),
    ("dim", "dim", "diminished", ["1", "b3", "b5"], 0.8, "dim", "°", []),
    ("aug", "aug", "augmented", ["1", "3", "#5"], 1.0, "aug", "+", []),
    ("sus4", "sus4", "suspended 4th", ["1", "4", "5"], 1.0, "sus", "sus4", []),
    ("sus2", "sus2", "suspended 2nd", ["1", "2", "5"], 1.2, "sus", "sus2", []),
    ("5", "5", "power chord", ["1", "5"], 0.6, "power", "5", []),
    ("b5", "(b5)", "major flat five", ["1", "3", "b5"], 3.5, "maj", "(b5)", []),
    ("6", "6", "major sixth", ["1", "3", "5", "6"], 1.8, "maj", "6", ["5"]),
    ("m6", "m6", "minor sixth", ["1", "b3", "5", "6"], 1.8, "min", "6", ["5"]),
    ("69", "6/9", "six nine", ["1", "3", "5", "6", "9"], 2.2, "maj", "6/9", ["5"]),
    ("m69", "m6/9", "minor six nine", ["1", "b3", "5", "6", "9"], 2.6, "min", "6/9", ["5"]),
    ("maj7", "maj7", "major seventh", ["1", "3", "5", "7"], 1.2, "maj", "maj7", ["5"]),
    ("7", "7", "dominant seventh", ["1", "3", "5", "b7"], 0.9, "dom", "7", ["5"]),
    ("m7", "m7", "minor seventh", ["1", "b3", "5", "b7"], 1.0, "min", "7", ["5"]),
    ("mMaj7", "m(maj7)", "minor major seventh", ["1", "b3", "5", "7"], 2.2, "min", "(maj7)", ["5"]),
    ("dim7", "dim7", "diminished seventh", ["1", "b3", "b5", "bb7"], 1.4, "dim", "°7", []),
    ("m7b5", "m7b5", "half-diminished (minor seven flat five)", ["1", "b3", "b5", "b7"], 1.3, "hdim", "ø7", []),
    ("7#5", "7#5", "augmented seventh", ["1", "3", "#5", "b7"], 2.2, "dom", "+7", []),
    ("maj7#5", "maj7#5", "augmented major seventh", ["1", "3", "#5", "7"], 2.8, "aug", "+maj7", []),
    ("7b5", "7b5", "seven flat five", ["1", "3", "b5", "b7"], 2.6, "dom", "7b5", []),
    ("7sus4", "7sus4", "seven suspended 4th", ["1", "4", "5", "b7"], 1.6, "dom", "7sus4", ["5"]),
    ("add9", "add9", "added ninth", ["1", "3", "5", "9"], 1.5, "maj", "add9", ["5"]),
    ("madd9", "m(add9)", "minor added ninth", ["1", "b3", "5", "9"], 1.8, "min", "(add9)", ["5"]),
    ("add11", "add11", "added eleventh", ["1", "3", "5", "11"], 2.4, "maj", "add11", ["5"]),
    ("madd11", "m(add11)", "minor added eleventh", ["1", "b3", "5", "11"], 2.5, "min", "(add11)", ["5"]),
    ("addS11", "add#11", "added sharp eleventh", ["1", "3", "5", "#11"], 3.2, "maj", "add#11", []),
    ("sus4add9", "sus4(add9)", "suspended 4th, added ninth", ["1", "4", "5", "9"], 2.8, "sus", "sus4(add9)", []),
    ("9", "9", "dominant ninth", ["1", "3", "5", "b7", "9"], 1.6, "dom", "9", ["5"]),
    ("maj9", "maj9", "major ninth", ["1", "3", "5", "7", "9"], 1.9, "maj", "maj9", ["5"]),
    ("m9", "m9", "minor ninth", ["1", "b3", "5", "b7", "9"], 1.8, "min", "9", ["5"]),
    ("mMaj9", "m(maj9)", "minor major ninth", ["1", "b3", "5", "7", "9"], 3.0, "min", "(maj9)", ["5"]),
    ("m9b5", "m9b5", "half-diminished ninth", ["1", "b3", "b5", "b7", "9"], 3.2, "hdim", "ø9", []),
    ("9sus4", "9sus4", "nine suspended 4th", ["1", "4", "5", "b7", "9"], 2.4, "dom", "9sus4", ["5"]),
    ("7b9", "7b9", "seven flat nine", ["1", "3", "5", "b7", "b9"], 2.2, "dom", "7b9", ["5"]),
    ("7#9", "7#9", "seven sharp nine", ["1", "3", "5", "b7", "#9"], 2.2, "dom", "7#9", ["5"]),
    ("7#11", "7#11", "seven sharp eleven", ["1", "3", "5", "b7", "#11"], 2.8, "dom", "7#11", ["5"]),
    ("7b13", "7b13", "seven flat thirteen", ["1", "3", "5", "b7", "b13"], 3.0, "dom", "7b13", ["5"]),
    ("7b9b13", "7b9b13", "seven flat nine flat thirteen", ["1", "3", "5", "b7", "b9", "b13"], 3.4, "dom", "7b9b13", ["5"]),
    ("7#9b13", "7#9b13", "seven sharp nine flat thirteen", ["1", "3", "5", "b7", "#9", "b13"], 3.4, "dom", "7#9b13", ["5"]),
    ("7b9#11", "7b9#11", "seven flat nine sharp eleven", ["1", "3", "5", "b7", "b9", "#11"], 3.6, "dom", "7b9#11", ["5"]),
    ("9#11", "9#11", "nine sharp eleven", ["1", "3", "5", "b7", "9", "#11"], 3.0, "dom", "9#11", ["5"]),
    ("maj7#11", "maj7#11", "major seven sharp eleven", ["1", "3", "5", "7", "#11"], 2.6, "maj", "maj7#11", ["5"]),
    ("maj9#11", "maj9#11", "major nine sharp eleven", ["1", "3", "5", "7", "9", "#11"], 3.2, "maj", "maj9#11", ["5"]),
    ("11", "11", "dominant eleventh", ["1", "3", "5", "b7", "9", "11"], 2.8, "dom", "11", ["3", "5"]),
    ("m11", "m11", "minor eleventh", ["1", "b3", "5", "b7", "9", "11"], 2.4, "min", "11", ["5", "9"]),
    ("13", "13", "dominant thirteenth", ["1", "3", "5", "b7", "9", "13"], 2.2, "dom", "13", ["5", "9"]),
    ("13b9", "13b9", "thirteen flat nine", ["1", "3", "5", "b7", "b9", "13"], 3.2, "dom", "13b9", ["5"]),
    ("13#11", "13#11", "thirteen sharp eleven", ["1", "3", "5", "b7", "9", "#11", "13"], 3.4, "dom", "13#11", ["5", "9"]),
    ("13sus4", "13sus4", "thirteen suspended 4th", ["1", "4", "5", "b7", "9", "13"], 3.0, "dom", "13sus4", ["5", "9"]),
    ("maj13", "maj13", "major thirteenth", ["1", "3", "5", "7", "9", "13"], 2.6, "maj", "maj13", ["5", "9"]),
    ("m13", "m13", "minor thirteenth", ["1", "b3", "5", "b7", "9", "11", "13"], 2.8, "min", "13", ["5", "9", "11"]),
]

FAMILIES = ["maj", "min", "dim", "aug", "sus", "power", "dom", "hdim"]

# The types grouped for a picker, in the order a player looks for them
GROUPS = [
    ("Triads", ["maj", "min", "dim", "aug", "sus2", "sus4", "5"]),
    ("Sixths and added tones", ["6", "m6", "69", "m69", "add9", "madd9", "add11", "madd11", "sus4add9", "addS11"]),
    ("Sevenths", ["maj7", "7", "m7", "mMaj7", "m7b5", "dim7", "7sus4", "7#5", "7b5", "maj7#5"]),
    ("Ninths and up", ["9", "maj9", "m9", "mMaj9", "9sus4", "m9b5", "11", "m11", "13", "maj13", "m13", "13sus4"]),
    ("Altered", ["7b9", "7#9", "7#11", "7b13", "7b9b13", "7#9b13", "7b9#11", "9#11", "13b9", "13#11", "maj7#11", "maj9#11", "b5"]),
]

MAJOR = [0, 2, 4, 5, 7, 9, 11]

def degree(d):
    """A degree like "b7" or "#11": its number, scale step (0..6), alteration and size in semitones"""
    m = re.fullmatch(r"(bb|b|#)?(\d+)", d)
    assert m, "not a degree: " + d
    acc = {"bb": -2, "b": -1, "#": 1}.get(m.group(1), 0)
    n = int(m.group(2))
    step = (n - 1) % 7
    return {"d": d, "n": n, "step": step, "acc": acc, "semis": MAJOR[step] + acc + 12 * ((n - 1) // 7)}

def check():
    ids = [t[0] for t in TYPES]
    assert len(set(ids)) == len(ids), "an id is used twice"
    syms = [t[1] for t in TYPES]
    assert len(set(syms)) == len(syms), "a symbol is used twice"
    for i, sym, name, f, cost, fam, num, opt in TYPES:
        assert fam in FAMILIES, i
        assert f[0] == "1", i
        for d in f: degree(d)
        for d in opt: assert d in f, i + ": " + d
    grouped = [i for _, g in GROUPS for i in g]
    assert sorted(grouped) == sorted(ids), "every type is in one group"

def number(v):
    return int(v) if float(v).is_integer() else v

def write_json():
    data = {
        "types": [{"id": i, "sym": sym, "name": name, "f": f, "cost": number(cost), "opt": opt, "fam": fam, "num": num,
                   "tones": [degree(d) for d in f]} for i, sym, name, f, cost, fam, num, opt in TYPES],
        "groups": [{"name": n, "types": g} for n, g in GROUPS],
    }
    text = json.dumps(data, ensure_ascii=False, separators=(",", ":")) + "\n"
    open(os.path.join(ROOT, "data", "chords.json"), "w", encoding="utf-8", newline="\n").write(text)

def cpp_str(s):
    out = '"'
    for b in s.encode("utf-8"):
        c = chr(b)
        if b >= 128: out += "\\%03o" % b
        elif c in ('"', "\\"): out += "\\" + c
        else: out += c
    return out + '"'

def cname(i):
    return re.sub(r"[^A-Za-z0-9]", lambda m: {"#": "S"}.get(m.group(0), "_"), i)

HEADER = """// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// The chord types: every chord ENCORE.'s products name, with its symbol,
// formula, family and roman-numeral suffix, and the groups a picker shows
// them in. Made by tools/chords.py (edit the list there and run it, not this
// file). Text is UTF-8. Everything is constant data, so reading it never
// allocates.
#pragma once

#include <array>
#include <span>
#include <string_view>

namespace vs::chords
{
    // A degree of a formula: its text ("b7"), number, scale step (0..6),
    // alteration (-2..1) and semitones above the root
    struct Degree { const char* d; int n; int step; int acc; int semis; };

    enum class Family { maj, min, dim, aug, sus, power, dom, hdim };

    struct ChordType
    {
        const char* id;
        const char* sym;                      // what follows the root in a chord symbol ("m7b5")
        const char* name;
        std::span<const Degree> tones;        // the formula
        double cost;                          // how rarely the name is the right one (lower wins)
        std::span<const char* const> opt;     // degrees that can be left out and it's still this chord
        Family family;
        const char* num;                      // what follows a roman numeral ("ø7")
    };

    struct Group { const char* name; std::span<const char* const> types; };

    namespace detail
    {
@DATA@
    }

    inline constexpr std::array<ChordType, @NTYPES@> types { {
@TYPES@
    } };

    // The types grouped for a picker, in the order a player looks for them
    inline constexpr std::array<Group, @NGROUPS@> groups { {
@GROUPS@
    } };

    // The type with this id, or nullptr
    constexpr const ChordType* find (std::string_view id)
    {
        for (auto& t : types) if (id == t.id) return &t;
        return nullptr;
    }

    // The type with this symbol, or nullptr
    constexpr const ChordType* bySymbol (std::string_view sym)
    {
        for (auto& t : types) if (sym == t.sym) return &t;
        return nullptr;
    }

    // Its index in types
    constexpr int indexOf (const ChordType& t) { return (int) (&t - types.data()); }
}
"""

def write_header():
    data, types, groups = [], [], []
    for i, sym, name, f, cost, fam, num, opt in TYPES:
        c = cname(i)
        tones = ", ".join("{ %s, %d, %d, %d, %d }" % (cpp_str(g["d"]), g["n"], g["step"], g["acc"], g["semis"]) for g in map(degree, f))
        data.append("        inline constexpr Degree tones_%s[] { %s };" % (c, tones))
        opt_ref = "{}"
        if opt:
            data.append("        inline constexpr const char* opt_%s[] { %s };" % (c, ", ".join(cpp_str(d) for d in opt)))
            opt_ref = "detail::opt_" + c
        types.append("        { %s, %s, %s, detail::tones_%s, %r, %s, Family::%s, %s }," % (cpp_str(i), cpp_str(sym), cpp_str(name), c, float(cost), opt_ref, fam, cpp_str(num)))
    for k, (n, g) in enumerate(GROUPS):
        data.append("        inline constexpr const char* group_%d[] { %s };" % (k, ", ".join(cpp_str(x) for x in g)))
        groups.append("        { %s, detail::group_%d }," % (cpp_str(n), k))
    text = (HEADER.replace("@DATA@", "\n".join(data)).replace("@NTYPES@", str(len(TYPES))).replace("@TYPES@", "\n".join(types))
            .replace("@NGROUPS@", str(len(GROUPS))).replace("@GROUPS@", "\n".join(groups)))
    open(os.path.join(ROOT, "dsp", "ChordTypes.h"), "w", encoding="utf-8", newline="\n").write(text)

if __name__ == "__main__":
    check()
    write_json()
    write_header()
    print("wrote dsp/ChordTypes.h and data/chords.json: %d types" % len(TYPES))
