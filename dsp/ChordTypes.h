// ENCORE. Audio Processing
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
        inline constexpr Degree tones_maj[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 } };
        inline constexpr Degree tones_min[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 } };
        inline constexpr Degree tones_dim[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "b5", 5, 4, -1, 6 } };
        inline constexpr Degree tones_aug[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "#5", 5, 4, 1, 8 } };
        inline constexpr Degree tones_sus4[] { { "1", 1, 0, 0, 0 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 } };
        inline constexpr Degree tones_sus2[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "5", 5, 4, 0, 7 } };
        inline constexpr Degree tones_5[] { { "1", 1, 0, 0, 0 }, { "5", 5, 4, 0, 7 } };
        inline constexpr Degree tones_b5[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "b5", 5, 4, -1, 6 } };
        inline constexpr Degree tones_6[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 } };
        inline constexpr const char* opt_6[] { "5" };
        inline constexpr Degree tones_m6[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 } };
        inline constexpr const char* opt_m6[] { "5" };
        inline constexpr Degree tones_69[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "9", 9, 1, 0, 14 } };
        inline constexpr const char* opt_69[] { "5" };
        inline constexpr Degree tones_m69[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "9", 9, 1, 0, 14 } };
        inline constexpr const char* opt_m69[] { "5" };
        inline constexpr Degree tones_maj7[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "7", 7, 6, 0, 11 } };
        inline constexpr const char* opt_maj7[] { "5" };
        inline constexpr Degree tones_7[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr const char* opt_7[] { "5" };
        inline constexpr Degree tones_m7[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr const char* opt_m7[] { "5" };
        inline constexpr Degree tones_mMaj7[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "7", 7, 6, 0, 11 } };
        inline constexpr const char* opt_mMaj7[] { "5" };
        inline constexpr Degree tones_dim7[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "b5", 5, 4, -1, 6 }, { "bb7", 7, 6, -2, 9 } };
        inline constexpr Degree tones_m7b5[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "b5", 5, 4, -1, 6 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr Degree tones_7S5[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "#5", 5, 4, 1, 8 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr Degree tones_maj7S5[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "#5", 5, 4, 1, 8 }, { "7", 7, 6, 0, 11 } };
        inline constexpr Degree tones_7b5[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "b5", 5, 4, -1, 6 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr Degree tones_7sus4[] { { "1", 1, 0, 0, 0 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr const char* opt_7sus4[] { "5" };
        inline constexpr Degree tones_add9[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "9", 9, 1, 0, 14 } };
        inline constexpr const char* opt_add9[] { "5" };
        inline constexpr Degree tones_madd9[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "9", 9, 1, 0, 14 } };
        inline constexpr const char* opt_madd9[] { "5" };
        inline constexpr Degree tones_add11[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "11", 11, 3, 0, 17 } };
        inline constexpr const char* opt_add11[] { "5" };
        inline constexpr Degree tones_madd11[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "11", 11, 3, 0, 17 } };
        inline constexpr const char* opt_madd11[] { "5" };
        inline constexpr Degree tones_addS11[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "#11", 11, 3, 1, 18 } };
        inline constexpr Degree tones_sus4add9[] { { "1", 1, 0, 0, 0 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "9", 9, 1, 0, 14 } };
        inline constexpr Degree tones_9[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "9", 9, 1, 0, 14 } };
        inline constexpr const char* opt_9[] { "5" };
        inline constexpr Degree tones_maj9[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "7", 7, 6, 0, 11 }, { "9", 9, 1, 0, 14 } };
        inline constexpr const char* opt_maj9[] { "5" };
        inline constexpr Degree tones_m9[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "9", 9, 1, 0, 14 } };
        inline constexpr const char* opt_m9[] { "5" };
        inline constexpr Degree tones_mMaj9[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "7", 7, 6, 0, 11 }, { "9", 9, 1, 0, 14 } };
        inline constexpr const char* opt_mMaj9[] { "5" };
        inline constexpr Degree tones_m9b5[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "b5", 5, 4, -1, 6 }, { "b7", 7, 6, -1, 10 }, { "9", 9, 1, 0, 14 } };
        inline constexpr Degree tones_9sus4[] { { "1", 1, 0, 0, 0 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "9", 9, 1, 0, 14 } };
        inline constexpr const char* opt_9sus4[] { "5" };
        inline constexpr Degree tones_7b9[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "b9", 9, 1, -1, 13 } };
        inline constexpr const char* opt_7b9[] { "5" };
        inline constexpr Degree tones_7S9[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "#9", 9, 1, 1, 15 } };
        inline constexpr const char* opt_7S9[] { "5" };
        inline constexpr Degree tones_7S11[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "#11", 11, 3, 1, 18 } };
        inline constexpr const char* opt_7S11[] { "5" };
        inline constexpr Degree tones_7b13[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "b13", 13, 5, -1, 20 } };
        inline constexpr const char* opt_7b13[] { "5" };
        inline constexpr Degree tones_7b9b13[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "b9", 9, 1, -1, 13 }, { "b13", 13, 5, -1, 20 } };
        inline constexpr const char* opt_7b9b13[] { "5" };
        inline constexpr Degree tones_7S9b13[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "#9", 9, 1, 1, 15 }, { "b13", 13, 5, -1, 20 } };
        inline constexpr const char* opt_7S9b13[] { "5" };
        inline constexpr Degree tones_7b9S11[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "b9", 9, 1, -1, 13 }, { "#11", 11, 3, 1, 18 } };
        inline constexpr const char* opt_7b9S11[] { "5" };
        inline constexpr Degree tones_9S11[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "9", 9, 1, 0, 14 }, { "#11", 11, 3, 1, 18 } };
        inline constexpr const char* opt_9S11[] { "5" };
        inline constexpr Degree tones_maj7S11[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "7", 7, 6, 0, 11 }, { "#11", 11, 3, 1, 18 } };
        inline constexpr const char* opt_maj7S11[] { "5" };
        inline constexpr Degree tones_maj9S11[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "7", 7, 6, 0, 11 }, { "9", 9, 1, 0, 14 }, { "#11", 11, 3, 1, 18 } };
        inline constexpr const char* opt_maj9S11[] { "5" };
        inline constexpr Degree tones_11[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "9", 9, 1, 0, 14 }, { "11", 11, 3, 0, 17 } };
        inline constexpr const char* opt_11[] { "3", "5" };
        inline constexpr Degree tones_m11[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "9", 9, 1, 0, 14 }, { "11", 11, 3, 0, 17 } };
        inline constexpr const char* opt_m11[] { "5", "9" };
        inline constexpr Degree tones_13[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "9", 9, 1, 0, 14 }, { "13", 13, 5, 0, 21 } };
        inline constexpr const char* opt_13[] { "5", "9" };
        inline constexpr Degree tones_13b9[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "b9", 9, 1, -1, 13 }, { "13", 13, 5, 0, 21 } };
        inline constexpr const char* opt_13b9[] { "5" };
        inline constexpr Degree tones_13S11[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "9", 9, 1, 0, 14 }, { "#11", 11, 3, 1, 18 }, { "13", 13, 5, 0, 21 } };
        inline constexpr const char* opt_13S11[] { "5", "9" };
        inline constexpr Degree tones_13sus4[] { { "1", 1, 0, 0, 0 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "9", 9, 1, 0, 14 }, { "13", 13, 5, 0, 21 } };
        inline constexpr const char* opt_13sus4[] { "5", "9" };
        inline constexpr Degree tones_maj13[] { { "1", 1, 0, 0, 0 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "7", 7, 6, 0, 11 }, { "9", 9, 1, 0, 14 }, { "13", 13, 5, 0, 21 } };
        inline constexpr const char* opt_maj13[] { "5", "9" };
        inline constexpr Degree tones_m13[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 }, { "9", 9, 1, 0, 14 }, { "11", 11, 3, 0, 17 }, { "13", 13, 5, 0, 21 } };
        inline constexpr const char* opt_m13[] { "5", "9", "11" };
        inline constexpr const char* group_0[] { "maj", "min", "dim", "aug", "sus2", "sus4", "5" };
        inline constexpr const char* group_1[] { "6", "m6", "69", "m69", "add9", "madd9", "add11", "madd11", "sus4add9", "addS11" };
        inline constexpr const char* group_2[] { "maj7", "7", "m7", "mMaj7", "m7b5", "dim7", "7sus4", "7#5", "7b5", "maj7#5" };
        inline constexpr const char* group_3[] { "9", "maj9", "m9", "mMaj9", "9sus4", "m9b5", "11", "m11", "13", "maj13", "m13", "13sus4" };
        inline constexpr const char* group_4[] { "7b9", "7#9", "7#11", "7b13", "7b9b13", "7#9b13", "7b9#11", "9#11", "13b9", "13#11", "maj7#11", "maj9#11", "b5" };
    }

    inline constexpr std::array<ChordType, 52> types { {
        { "maj", "", "major", detail::tones_maj, 0.0, {}, Family::maj, "" },
        { "min", "m", "minor", detail::tones_min, 0.0, {}, Family::min, "" },
        { "dim", "dim", "diminished", detail::tones_dim, 0.8, {}, Family::dim, "\302\260" },
        { "aug", "aug", "augmented", detail::tones_aug, 1.0, {}, Family::aug, "+" },
        { "sus4", "sus4", "suspended 4th", detail::tones_sus4, 1.0, {}, Family::sus, "sus4" },
        { "sus2", "sus2", "suspended 2nd", detail::tones_sus2, 1.2, {}, Family::sus, "sus2" },
        { "5", "5", "power chord", detail::tones_5, 0.6, {}, Family::power, "5" },
        { "b5", "(b5)", "major flat five", detail::tones_b5, 3.5, {}, Family::maj, "(b5)" },
        { "6", "6", "major sixth", detail::tones_6, 1.8, detail::opt_6, Family::maj, "6" },
        { "m6", "m6", "minor sixth", detail::tones_m6, 1.8, detail::opt_m6, Family::min, "6" },
        { "69", "6/9", "six nine", detail::tones_69, 2.2, detail::opt_69, Family::maj, "6/9" },
        { "m69", "m6/9", "minor six nine", detail::tones_m69, 2.6, detail::opt_m69, Family::min, "6/9" },
        { "maj7", "maj7", "major seventh", detail::tones_maj7, 1.2, detail::opt_maj7, Family::maj, "maj7" },
        { "7", "7", "dominant seventh", detail::tones_7, 0.9, detail::opt_7, Family::dom, "7" },
        { "m7", "m7", "minor seventh", detail::tones_m7, 1.0, detail::opt_m7, Family::min, "7" },
        { "mMaj7", "m(maj7)", "minor major seventh", detail::tones_mMaj7, 2.2, detail::opt_mMaj7, Family::min, "(maj7)" },
        { "dim7", "dim7", "diminished seventh", detail::tones_dim7, 1.4, {}, Family::dim, "\302\2607" },
        { "m7b5", "m7b5", "half-diminished (minor seven flat five)", detail::tones_m7b5, 1.3, {}, Family::hdim, "\303\2707" },
        { "7#5", "7#5", "augmented seventh", detail::tones_7S5, 2.2, {}, Family::dom, "+7" },
        { "maj7#5", "maj7#5", "augmented major seventh", detail::tones_maj7S5, 2.8, {}, Family::aug, "+maj7" },
        { "7b5", "7b5", "seven flat five", detail::tones_7b5, 2.6, {}, Family::dom, "7b5" },
        { "7sus4", "7sus4", "seven suspended 4th", detail::tones_7sus4, 1.6, detail::opt_7sus4, Family::dom, "7sus4" },
        { "add9", "add9", "added ninth", detail::tones_add9, 1.5, detail::opt_add9, Family::maj, "add9" },
        { "madd9", "m(add9)", "minor added ninth", detail::tones_madd9, 1.8, detail::opt_madd9, Family::min, "(add9)" },
        { "add11", "add11", "added eleventh", detail::tones_add11, 2.4, detail::opt_add11, Family::maj, "add11" },
        { "madd11", "m(add11)", "minor added eleventh", detail::tones_madd11, 2.5, detail::opt_madd11, Family::min, "(add11)" },
        { "addS11", "add#11", "added sharp eleventh", detail::tones_addS11, 3.2, {}, Family::maj, "add#11" },
        { "sus4add9", "sus4(add9)", "suspended 4th, added ninth", detail::tones_sus4add9, 2.8, {}, Family::sus, "sus4(add9)" },
        { "9", "9", "dominant ninth", detail::tones_9, 1.6, detail::opt_9, Family::dom, "9" },
        { "maj9", "maj9", "major ninth", detail::tones_maj9, 1.9, detail::opt_maj9, Family::maj, "maj9" },
        { "m9", "m9", "minor ninth", detail::tones_m9, 1.8, detail::opt_m9, Family::min, "9" },
        { "mMaj9", "m(maj9)", "minor major ninth", detail::tones_mMaj9, 3.0, detail::opt_mMaj9, Family::min, "(maj9)" },
        { "m9b5", "m9b5", "half-diminished ninth", detail::tones_m9b5, 3.2, {}, Family::hdim, "\303\2709" },
        { "9sus4", "9sus4", "nine suspended 4th", detail::tones_9sus4, 2.4, detail::opt_9sus4, Family::dom, "9sus4" },
        { "7b9", "7b9", "seven flat nine", detail::tones_7b9, 2.2, detail::opt_7b9, Family::dom, "7b9" },
        { "7#9", "7#9", "seven sharp nine", detail::tones_7S9, 2.2, detail::opt_7S9, Family::dom, "7#9" },
        { "7#11", "7#11", "seven sharp eleven", detail::tones_7S11, 2.8, detail::opt_7S11, Family::dom, "7#11" },
        { "7b13", "7b13", "seven flat thirteen", detail::tones_7b13, 3.0, detail::opt_7b13, Family::dom, "7b13" },
        { "7b9b13", "7b9b13", "seven flat nine flat thirteen", detail::tones_7b9b13, 3.4, detail::opt_7b9b13, Family::dom, "7b9b13" },
        { "7#9b13", "7#9b13", "seven sharp nine flat thirteen", detail::tones_7S9b13, 3.4, detail::opt_7S9b13, Family::dom, "7#9b13" },
        { "7b9#11", "7b9#11", "seven flat nine sharp eleven", detail::tones_7b9S11, 3.6, detail::opt_7b9S11, Family::dom, "7b9#11" },
        { "9#11", "9#11", "nine sharp eleven", detail::tones_9S11, 3.0, detail::opt_9S11, Family::dom, "9#11" },
        { "maj7#11", "maj7#11", "major seven sharp eleven", detail::tones_maj7S11, 2.6, detail::opt_maj7S11, Family::maj, "maj7#11" },
        { "maj9#11", "maj9#11", "major nine sharp eleven", detail::tones_maj9S11, 3.2, detail::opt_maj9S11, Family::maj, "maj9#11" },
        { "11", "11", "dominant eleventh", detail::tones_11, 2.8, detail::opt_11, Family::dom, "11" },
        { "m11", "m11", "minor eleventh", detail::tones_m11, 2.4, detail::opt_m11, Family::min, "11" },
        { "13", "13", "dominant thirteenth", detail::tones_13, 2.2, detail::opt_13, Family::dom, "13" },
        { "13b9", "13b9", "thirteen flat nine", detail::tones_13b9, 3.2, detail::opt_13b9, Family::dom, "13b9" },
        { "13#11", "13#11", "thirteen sharp eleven", detail::tones_13S11, 3.4, detail::opt_13S11, Family::dom, "13#11" },
        { "13sus4", "13sus4", "thirteen suspended 4th", detail::tones_13sus4, 3.0, detail::opt_13sus4, Family::dom, "13sus4" },
        { "maj13", "maj13", "major thirteenth", detail::tones_maj13, 2.6, detail::opt_maj13, Family::maj, "maj13" },
        { "m13", "m13", "minor thirteenth", detail::tones_m13, 2.8, detail::opt_m13, Family::min, "13" },
    } };

    // The types grouped for a picker, in the order a player looks for them
    inline constexpr std::array<Group, 5> groups { {
        { "Triads", detail::group_0 },
        { "Sixths and added tones", detail::group_1 },
        { "Sevenths", detail::group_2 },
        { "Ninths and up", detail::group_3 },
        { "Altered", detail::group_4 },
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
