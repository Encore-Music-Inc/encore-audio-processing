// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// The scale library: every scale ENCORE.'s products name, with its formula,
// semitones, family and the words shown with it. Made by tools/scales.py (edit
// the list there and run it, not this file). Text is UTF-8. Everything is
// constant data, so reading it never allocates (safe on an audio thread).
#pragma once

#include <array>
#include <span>
#include <string_view>

namespace vs::scales
{
    // A degree of a formula: its text ("b7"), number, scale step (0..6),
    // alteration (-2..1) and semitones above the root
    struct Degree { const char* d; int n; int step; int acc; int semis; };

    struct LibraryScale
    {
        const char* id;
        const char* name;
        const char* shortName;                  // a shorter name for tight spaces ("" when the name is short enough)
        const char* group;                      // the id of one of groups
        std::span<const Degree> tones;          // the formula, counted up the major scale from the root
        std::span<const int> semis;             // semitones of each degree above the root (0..11)
        bool hepta;                             // seven notes on seven different letters
        bool sym;                               // symmetric: spelled the plainest way, not one letter per degree
        std::span<const char* const> aka;       // other names it goes by
        std::span<const char* const> character; // the degree(s) that give it its flavour
        const char* nearId;                     // its nearest relative's id, or nullptr (for "how it differs")
        double prior;                           // how often it's the everyday answer (lower is more common)
        std::span<const char* const> jazz;      // the formula as jazz players read it over a chord (may be empty)
        const char* approx;                     // why it is only a 12-tone version of a scale tuned between our notes, or ""
        const char* differs;                    // a hand-written "how it differs", or ""
        const char* note;                       // a note about the name, or ""
        const char* mood;
        const char* genres;
        const char* why;
    };

    struct Group { const char* id; const char* name; };
    struct Family { const char* id; const char* name; std::array<const char*, 7> modes; };

    namespace detail
    {
        inline constexpr Degree tones_ionian[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "3", 3, 2, 0, 4 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_ionian[] { 0, 2, 4, 5, 7, 9, 11 };
        inline constexpr const char* aka_ionian[] { "Major", "Ionian" };
        inline constexpr const char* char_ionian[] { "7" };
        inline constexpr Degree tones_dorian[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_dorian[] { 0, 2, 3, 5, 7, 9, 10 };
        inline constexpr const char* char_dorian[] { "6" };
        inline constexpr Degree tones_phrygian[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_phrygian[] { 0, 1, 3, 5, 7, 8, 10 };
        inline constexpr const char* char_phrygian[] { "b2" };
        inline constexpr Degree tones_lydian[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "3", 3, 2, 0, 4 }, { "#4", 4, 3, 1, 6 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_lydian[] { 0, 2, 4, 6, 7, 9, 11 };
        inline constexpr const char* char_lydian[] { "#4" };
        inline constexpr Degree tones_mixolydian[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "3", 3, 2, 0, 4 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_mixolydian[] { 0, 2, 4, 5, 7, 9, 10 };
        inline constexpr const char* char_mixolydian[] { "b7" };
        inline constexpr Degree tones_aeolian[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_aeolian[] { 0, 2, 3, 5, 7, 8, 10 };
        inline constexpr const char* aka_aeolian[] { "Natural minor", "Aeolian", "Minor" };
        inline constexpr const char* char_aeolian[] { "b6" };
        inline constexpr Degree tones_locrian[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "b5", 5, 4, -1, 6 }, { "b6", 6, 5, -1, 8 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_locrian[] { 0, 1, 3, 5, 6, 8, 10 };
        inline constexpr const char* char_locrian[] { "b5" };
        inline constexpr Degree tones_harmonic_minor[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_harmonic_minor[] { 0, 2, 3, 5, 7, 8, 11 };
        inline constexpr const char* char_harmonic_minor[] { "7" };
        inline constexpr Degree tones_melodic_minor[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_melodic_minor[] { 0, 2, 3, 5, 7, 9, 11 };
        inline constexpr const char* aka_melodic_minor[] { "Jazz minor", "Ascending melodic minor" };
        inline constexpr const char* char_melodic_minor[] { "6", "7" };
        inline constexpr Degree tones_harmonic_major[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "3", 3, 2, 0, 4 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_harmonic_major[] { 0, 2, 4, 5, 7, 8, 11 };
        inline constexpr const char* char_harmonic_major[] { "b6" };
        inline constexpr Degree tones_locrian_n6[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "b5", 5, 4, -1, 6 }, { "6", 6, 5, 0, 9 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_locrian_n6[] { 0, 1, 3, 5, 6, 9, 10 };
        inline constexpr const char* aka_locrian_n6[] { "Locrian #6" };
        inline constexpr const char* char_locrian_n6[] { "6" };
        inline constexpr Degree tones_ionian_s5[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "3", 3, 2, 0, 4 }, { "4", 4, 3, 0, 5 }, { "#5", 5, 4, 1, 8 }, { "6", 6, 5, 0, 9 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_ionian_s5[] { 0, 2, 4, 5, 8, 9, 11 };
        inline constexpr const char* char_ionian_s5[] { "#5" };
        inline constexpr Degree tones_ukrainian_dorian[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "#4", 4, 3, 1, 6 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_ukrainian_dorian[] { 0, 2, 3, 6, 7, 9, 10 };
        inline constexpr const char* aka_ukrainian_dorian[] { "Dorian #4", "Romanian minor", "Misheberakh (klezmer)", "Altered Dorian", "Maqam Nikriz (Arabic and Turkish, 12-tone approximation)" };
        inline constexpr const char* char_ukrainian_dorian[] { "#4" };
        inline constexpr Degree tones_phrygian_dominant[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "3", 3, 2, 0, 4 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_phrygian_dominant[] { 0, 1, 4, 5, 7, 8, 10 };
        inline constexpr const char* aka_phrygian_dominant[] { "Freygish (klezmer)", "Ahava Rabbah (Jewish prayer mode)", "Spanish Phrygian", "Maqam Hijaz (Arabic, 12-tone approximation)" };
        inline constexpr const char* char_phrygian_dominant[] { "b2", "3" };
        inline constexpr Degree tones_lydian_s2[] { { "1", 1, 0, 0, 0 }, { "#2", 2, 1, 1, 3 }, { "3", 3, 2, 0, 4 }, { "#4", 4, 3, 1, 6 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_lydian_s2[] { 0, 3, 4, 6, 7, 9, 11 };
        inline constexpr const char* char_lydian_s2[] { "#2" };
        inline constexpr Degree tones_ultralocrian[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "b3", 3, 2, -1, 3 }, { "b4", 4, 3, -1, 4 }, { "b5", 5, 4, -1, 6 }, { "b6", 6, 5, -1, 8 }, { "bb7", 7, 6, -2, 9 } };
        inline constexpr int semis_ultralocrian[] { 0, 1, 3, 4, 6, 8, 9 };
        inline constexpr const char* aka_ultralocrian[] { "Ultralocrian", "Super-Locrian bb7" };
        inline constexpr const char* char_ultralocrian[] { "bb7" };
        inline constexpr Degree tones_dorian_b2[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_dorian_b2[] { 0, 1, 3, 5, 7, 9, 10 };
        inline constexpr const char* aka_dorian_b2[] { "Phrygian #6" };
        inline constexpr const char* char_dorian_b2[] { "b2" };
        inline constexpr Degree tones_lydian_aug[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "3", 3, 2, 0, 4 }, { "#4", 4, 3, 1, 6 }, { "#5", 5, 4, 1, 8 }, { "6", 6, 5, 0, 9 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_lydian_aug[] { 0, 2, 4, 6, 8, 9, 11 };
        inline constexpr const char* char_lydian_aug[] { "#5" };
        inline constexpr Degree tones_lydian_dominant[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "3", 3, 2, 0, 4 }, { "#4", 4, 3, 1, 6 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_lydian_dominant[] { 0, 2, 4, 6, 7, 9, 10 };
        inline constexpr const char* aka_lydian_dominant[] { "Overtone scale", "Acoustic scale", "Lydian b7", "Mixolydian #4" };
        inline constexpr const char* char_lydian_dominant[] { "#4", "b7" };
        inline constexpr Degree tones_mixolydian_b6[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "3", 3, 2, 0, 4 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_mixolydian_b6[] { 0, 2, 4, 5, 7, 8, 10 };
        inline constexpr const char* aka_mixolydian_b6[] { "Aeolian dominant", "Melodic major" };
        inline constexpr const char* char_mixolydian_b6[] { "b6" };
        inline constexpr Degree tones_locrian_s2[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "b5", 5, 4, -1, 6 }, { "b6", 6, 5, -1, 8 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_locrian_s2[] { 0, 2, 3, 5, 6, 8, 10 };
        inline constexpr const char* aka_locrian_s2[] { "Locrian natural 2", "Aeolian b5", "Half-diminished scale" };
        inline constexpr const char* char_locrian_s2[] { "2" };
        inline constexpr Degree tones_altered[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "b3", 3, 2, -1, 3 }, { "b4", 4, 3, -1, 4 }, { "b5", 5, 4, -1, 6 }, { "b6", 6, 5, -1, 8 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_altered[] { 0, 1, 3, 4, 6, 8, 10 };
        inline constexpr const char* aka_altered[] { "Super-Locrian", "Diminished whole tone" };
        inline constexpr const char* char_altered[] { "b2", "b3", "b5", "b6" };
        inline constexpr const char* jazz_altered[] { "1", "b9", "#9", "3", "#11", "b13", "b7" };
        inline constexpr Degree tones_major_pent[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 } };
        inline constexpr int semis_major_pent[] { 0, 2, 4, 7, 9 };
        inline constexpr Degree tones_minor_pent[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_minor_pent[] { 0, 3, 5, 7, 10 };
        inline constexpr Degree tones_blues[] { { "1", 1, 0, 0, 0 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "b5", 5, 4, -1, 6 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_blues[] { 0, 3, 5, 6, 7, 10 };
        inline constexpr const char* aka_blues[] { "Minor blues", "Blues" };
        inline constexpr const char* char_blues[] { "b5" };
        inline constexpr Degree tones_major_blues[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "3", 3, 2, 0, 4 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 } };
        inline constexpr int semis_major_blues[] { 0, 2, 3, 4, 7, 9 };
        inline constexpr const char* char_major_blues[] { "b3" };
        inline constexpr Degree tones_bebop_dominant[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "3", 3, 2, 0, 4 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "b7", 7, 6, -1, 10 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_bebop_dominant[] { 0, 2, 4, 5, 7, 9, 10, 11 };
        inline constexpr const char* char_bebop_dominant[] { "7" };
        inline constexpr Degree tones_bebop_major[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "3", 3, 2, 0, 4 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "#5", 5, 4, 1, 8 }, { "6", 6, 5, 0, 9 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_bebop_major[] { 0, 2, 4, 5, 7, 8, 9, 11 };
        inline constexpr const char* char_bebop_major[] { "#5" };
        inline constexpr Degree tones_bebop_dorian[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "3", 3, 2, 0, 4 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_bebop_dorian[] { 0, 2, 3, 4, 5, 7, 9, 10 };
        inline constexpr const char* aka_bebop_dorian[] { "Dorian bebop" };
        inline constexpr const char* char_bebop_dorian[] { "3" };
        inline constexpr Degree tones_whole_tone[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "3", 3, 2, 0, 4 }, { "#4", 4, 3, 1, 6 }, { "#5", 5, 4, 1, 8 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_whole_tone[] { 0, 2, 4, 6, 8, 10 };
        inline constexpr Degree tones_dim_hw[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "b3", 3, 2, -1, 3 }, { "3", 3, 2, 0, 4 }, { "#4", 4, 3, 1, 6 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_dim_hw[] { 0, 1, 3, 4, 6, 7, 9, 10 };
        inline constexpr const char* aka_dim_hw[] { "Dominant diminished", "Octatonic (half-whole)" };
        inline constexpr const char* char_dim_hw[] { "b2", "b3", "#4" };
        inline constexpr const char* jazz_dim_hw[] { "1", "b9", "#9", "3", "#11", "5", "13", "b7" };
        inline constexpr Degree tones_dim_wh[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "b5", 5, 4, -1, 6 }, { "b6", 6, 5, -1, 8 }, { "6", 6, 5, 0, 9 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_dim_wh[] { 0, 2, 3, 5, 6, 8, 9, 11 };
        inline constexpr const char* aka_dim_wh[] { "Octatonic (whole-half)" };
        inline constexpr const char* char_dim_wh[] { "2", "4", "b6", "7" };
        inline constexpr Degree tones_chromatic[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "3", 3, 2, 0, 4 }, { "4", 4, 3, 0, 5 }, { "#4", 4, 3, 1, 6 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 }, { "6", 6, 5, 0, 9 }, { "b7", 7, 6, -1, 10 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_chromatic[] { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        inline constexpr Degree tones_hungarian_minor[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "#4", 4, 3, 1, 6 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_hungarian_minor[] { 0, 2, 3, 6, 7, 8, 11 };
        inline constexpr const char* aka_hungarian_minor[] { "Double harmonic minor" };
        inline constexpr const char* char_hungarian_minor[] { "#4" };
        inline constexpr Degree tones_hungarian_major[] { { "1", 1, 0, 0, 0 }, { "#2", 2, 1, 1, 3 }, { "3", 3, 2, 0, 4 }, { "#4", 4, 3, 1, 6 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_hungarian_major[] { 0, 3, 4, 6, 7, 9, 10 };
        inline constexpr const char* char_hungarian_major[] { "#2", "#4" };
        inline constexpr Degree tones_double_harmonic[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "3", 3, 2, 0, 4 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_double_harmonic[] { 0, 1, 4, 5, 7, 8, 11 };
        inline constexpr const char* aka_double_harmonic[] { "Byzantine", "Arabic", "Maqam Hijaz Kar (Arabic, 12-tone approximation)", "Raga Bhairav (North Indian, 12-tone approximation)" };
        inline constexpr const char* char_double_harmonic[] { "b2", "b6" };
        inline constexpr Degree tones_neapolitan_minor[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_neapolitan_minor[] { 0, 1, 3, 5, 7, 8, 11 };
        inline constexpr const char* char_neapolitan_minor[] { "b2" };
        inline constexpr Degree tones_neapolitan_major[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "b3", 3, 2, -1, 3 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_neapolitan_major[] { 0, 1, 3, 5, 7, 9, 11 };
        inline constexpr const char* char_neapolitan_major[] { "b2" };
        inline constexpr Degree tones_persian[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "3", 3, 2, 0, 4 }, { "4", 4, 3, 0, 5 }, { "b5", 5, 4, -1, 6 }, { "b6", 6, 5, -1, 8 }, { "7", 7, 6, 0, 11 } };
        inline constexpr int semis_persian[] { 0, 1, 4, 5, 6, 8, 11 };
        inline constexpr const char* char_persian[] { "b5" };
        inline constexpr Degree tones_hirajoshi[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 } };
        inline constexpr int semis_hirajoshi[] { 0, 2, 3, 7, 8 };
        inline constexpr Degree tones_in[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 } };
        inline constexpr int semis_in[] { 0, 1, 5, 7, 8 };
        inline constexpr const char* aka_in[] { "Miyako-bushi", "Sakura scale" };
        inline constexpr const char* char_in[] { "b2" };
        inline constexpr Degree tones_insen[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_insen[] { 0, 1, 5, 7, 10 };
        inline constexpr const char* char_insen[] { "b2" };
        inline constexpr Degree tones_iwato[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "4", 4, 3, 0, 5 }, { "b5", 5, 4, -1, 6 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_iwato[] { 0, 1, 5, 6, 10 };
        inline constexpr const char* char_iwato[] { "b5" };
        inline constexpr Degree tones_yo[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 } };
        inline constexpr int semis_yo[] { 0, 2, 5, 7, 9 };
        inline constexpr Degree tones_kumoi[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "6", 6, 5, 0, 9 } };
        inline constexpr int semis_kumoi[] { 0, 2, 3, 7, 9 };
        inline constexpr Degree tones_pelog[] { { "1", 1, 0, 0, 0 }, { "b2", 2, 1, -1, 1 }, { "b3", 3, 2, -1, 3 }, { "5", 5, 4, 0, 7 }, { "b6", 6, 5, -1, 8 } };
        inline constexpr int semis_pelog[] { 0, 1, 3, 7, 8 };
        inline constexpr Degree tones_slendro[] { { "1", 1, 0, 0, 0 }, { "2", 2, 1, 0, 2 }, { "4", 4, 3, 0, 5 }, { "5", 5, 4, 0, 7 }, { "b7", 7, 6, -1, 10 } };
        inline constexpr int semis_slendro[] { 0, 2, 5, 7, 10 };
    }

    // The library's sections, in the order they're listed
    inline constexpr std::array<Group, 8> groups { {
        { "major", "Major and its modes" },
        { "minor", "Minor scales" },
        { "hminor", "Modes of harmonic minor" },
        { "mminor", "Modes of melodic minor" },
        { "penta", "Pentatonic and blues" },
        { "bebop", "Bebop" },
        { "sym", "Symmetric" },
        { "world", "World and folk" },
    } };

    inline constexpr std::array<LibraryScale, 47> library { {
        { "ionian", "Major (Ionian)", "Major", "major", detail::tones_ionian, detail::semis_ionian, true, false,
          detail::aka_ionian, detail::char_ionian, "mixolydian", 0.0, {},
          "",
          "",
          "",
          "Bright, settled, happy, finished.",
          "Pop, folk, country, hymns, classical, children's songs: most of the music you know.",
          "Its 7th sits a half step under the root and pulls up to it (the leading tone), so melodies feel finished when they land home." },
        { "dorian", "Dorian", "", "major", detail::tones_dorian, detail::semis_dorian, true, false,
          {}, detail::char_dorian, "aeolian", 0.0, {},
          "",
          "",
          "",
          "Minor but hopeful: cool, soulful, a little sly.",
          "Funk, soul, modal jazz (Miles Davis' So What), Santana (Oye Como Va), Celtic and English folk (Scarborough Fair).",
          "A minor scale with a major 6th. That one raised note brightens the minor sound and gives a major IV chord (Am to D), the groove of funk and Santana." },
        { "phrygian", "Phrygian", "", "major", detail::tones_phrygian, detail::semis_phrygian, true, false,
          {}, detail::char_phrygian, "aeolian", 0.7, {},
          "",
          "",
          "",
          "Dark, tense, brooding, Spanish-tinged.",
          "Flamenco, metal (thrash, death, djent), film scores.",
          "Its 2nd is only a half step above the root, so melodies fall heavily onto home. A b2 over a minor chord is the darkest common sound in Western music." },
        { "lydian", "Lydian", "", "major", detail::tones_lydian, detail::semis_lydian, true, false,
          {}, detail::char_lydian, "ionian", 0.6, {},
          "",
          "",
          "",
          "Bright, dreamy, floating, full of wonder.",
          "Film and TV scores (flying, magic, wonder), Joe Satriani, Steve Vai, prog rock, jazz over maj7#11 chords.",
          "The raised 4th removes the major scale's one clash (the 4th rubbing a half step above the 3rd), so nothing pulls down. It hovers." },
        { "mixolydian", "Mixolydian", "", "major", detail::tones_mixolydian, detail::semis_mixolydian, true, false,
          {}, detail::char_mixolydian, "ionian", 0.3, {},
          "",
          "",
          "",
          "Bright but bluesy: laid-back, rocking, earthy.",
          "Classic rock (Sweet Home Alabama), blues, country, bluegrass, Celtic and bagpipe tunes, funk, jam bands.",
          "Major with a flat 7th. The home chord becomes a dominant 7th, and the bVII chord (G to F in G) gives rock and folk their swagger without the pull of a leading tone." },
        { "aeolian", "Natural minor (Aeolian)", "Natural minor", "major", detail::tones_aeolian, detail::semis_aeolian, true, false,
          detail::aka_aeolian, detail::char_aeolian, "dorian", 0.15, {},
          "",
          "",
          "",
          "Sad, serious, reflective.",
          "Rock, pop ballads, metal, folk: the minor key of most songs.",
          "The b3, b6 and b7 all sit lower than in major, which our ears hear as sad. With no leading tone it settles softly instead of pulling home." },
        { "locrian", "Locrian", "", "major", detail::tones_locrian, detail::semis_locrian, true, false,
          {}, detail::char_locrian, "phrygian", 0.4, {},
          "",
          "",
          "",
          "Unstable, eerie, unresolved.",
          "Rare as a song's home key. Heard in metal riffs, and in jazz over half-diminished (m7b5) chords.",
          "Its 5th is flat, so its own home chord is diminished. Home itself sounds unsettled, which is why it is mostly used over one chord at a time." },
        { "harmonic_minor", "Harmonic minor", "", "minor", detail::tones_harmonic_minor, detail::semis_harmonic_minor, true, false,
          {}, detail::char_harmonic_minor, "aeolian", 0.5, {},
          "",
          "",
          "",
          "Dramatic, intense, classical, exotic.",
          "Classical, neoclassical metal (Yngwie Malmsteen), tango, East European and Middle Eastern flavored music.",
          "Natural minor with its 7th raised, so the V chord turns major and pulls home hard. The gap from b6 up to 7 (a step and a half) gives its exotic edge." },
        { "melodic_minor", "Melodic minor (jazz minor)", "", "minor", detail::tones_melodic_minor, detail::semis_melodic_minor, true, false,
          detail::aka_melodic_minor, detail::char_melodic_minor, "dorian", 0.5, {},
          "",
          "",
          "",
          "Minor but smooth and sophisticated, bittersweet.",
          "Jazz (over m(maj7) and m6 chords), classical, film noir.",
          "Major with a minor 3rd: the top half is pure major, so it is minor with a bright, rising pull. Classical players use it going up and natural minor coming down; jazz uses it both ways." },
        { "harmonic_major", "Harmonic major", "", "minor", detail::tones_harmonic_major, detail::semis_harmonic_major, true, false,
          {}, detail::char_harmonic_major, "ionian", 1.2, {},
          "",
          "",
          "",
          "Major with a shadow: noble, bittersweet.",
          "Classical (Rimsky-Korsakov), film scores, jazz.",
          "Major with a b6. The iv chord turns minor (the \"sad IV\") while the leading tone keeps its pull home." },
        { "locrian_n6", "Locrian natural 6", "", "hminor", detail::tones_locrian_n6, detail::semis_locrian_n6, true, false,
          detail::aka_locrian_n6, detail::char_locrian_n6, "locrian", 0.9, {},
          "",
          "",
          "",
          "Dark and tense, with an odd bright glint.",
          "Jazz over the ii\303\2707 chord of a minor ii, V, i; metal.",
          "Locrian with a major 6th: the same diminished home chord, but the raised 6th (the harmonic minor's leading tone) adds a strange light." },
        { "ionian_s5", "Ionian #5 (Ionian augmented)", "", "hminor", detail::tones_ionian_s5, detail::semis_ionian_s5, true, false,
          {}, detail::char_ionian_s5, "ionian", 1.0, {},
          "",
          "",
          "",
          "Bright and restless, dreamy-strange.",
          "Jazz over maj7#5 chords, film music.",
          "Major with a raised 5th, so the home chord is augmented and never quite settles." },
        { "ukrainian_dorian", "Ukrainian Dorian (Dorian #4)", "", "hminor", detail::tones_ukrainian_dorian, detail::semis_ukrainian_dorian, true, false,
          detail::aka_ukrainian_dorian, detail::char_ukrainian_dorian, "dorian", 1.0, {},
          "",
          "",
          "",
          "Plaintive and yearning, with a sob in it.",
          "Klezmer (the Misheberakh mode), Ukrainian, Romanian and other East European folk, Jewish prayer chant, Greek and Turkish music.",
          "Dorian with a raised 4th. The #4 leans up into the 5th, and the step and a half from b3 up to #4 is the crying \"augmented 2nd\" of East European melody." },
        { "phrygian_dominant", "Phrygian dominant (Freygish)", "", "hminor", detail::tones_phrygian_dominant, detail::semis_phrygian_dominant, true, false,
          detail::aka_phrygian_dominant, detail::char_phrygian_dominant, "phrygian", 0.7, {},
          "",
          "",
          "",
          "Fiery, exotic, intense.",
          "Flamenco, klezmer (Freygish), Jewish cantorial music, Middle Eastern and Balkan music, metal.",
          "Phrygian with a major 3rd. The step and a half from b2 up to 3 is the sound people call \"exotic\". It is the scale of the major V chord in a minor key (E7 in A minor)." },
        { "lydian_s2", "Lydian #2", "", "hminor", detail::tones_lydian_s2, detail::semis_lydian_s2, true, false,
          {}, detail::char_lydian_s2, "lydian", 1.4, {},
          "",
          "",
          "",
          "Bright, strange, shimmering.",
          "Jazz, fusion and film music; rare.",
          "Lydian with a raised 2nd: the #2 crowds up against the major 3rd for a bright but uneasy color." },
        { "ultralocrian", "Altered diminished (Ultralocrian)", "", "hminor", detail::tones_ultralocrian, detail::semis_ultralocrian, true, false,
          detail::aka_ultralocrian, detail::char_ultralocrian, "altered", 1.2, {},
          "",
          "",
          "",
          "Very dark, very tense.",
          "Jazz over diminished 7th chords; metal.",
          "Its 4th is flat (it sounds like a major 3rd) and its 7th double flat (it sounds like a 6th), so it holds a diminished 7th chord on its root." },
        { "dorian_b2", "Dorian b2 (Phrygian natural 6)", "", "mminor", detail::tones_dorian_b2, detail::semis_dorian_b2, true, false,
          detail::aka_dorian_b2, detail::char_dorian_b2, "dorian", 0.9, {},
          "",
          "",
          "",
          "Dark and mysterious, lighter than Phrygian.",
          "Jazz over sus(b9) chords, modal jazz, metal.",
          "Dorian with a flat 2nd: the b2 darkens it like Phrygian while the major 6th keeps a little light." },
        { "lydian_aug", "Lydian augmented", "", "mminor", detail::tones_lydian_aug, detail::semis_lydian_aug, true, false,
          {}, detail::char_lydian_aug, "lydian", 0.8, {},
          "",
          "",
          "",
          "Brighter than bright, mysterious, futuristic.",
          "Jazz over maj7#5 chords, film scores.",
          "Lydian with a raised 5th as well: the floating #4 plus an augmented home chord that never settles." },
        { "lydian_dominant", "Lydian dominant", "", "mminor", detail::tones_lydian_dominant, detail::semis_lydian_dominant, true, false,
          detail::aka_lydian_dominant, detail::char_lydian_dominant, "mixolydian", 0.7, {},
          "",
          "",
          "",
          "Bright, bluesy, playful, a little whimsical.",
          "Jazz over 7#11 chords, fusion, Brazilian music, Bartok, cartoon and film themes.",
          "It is close to the overtones a single note naturally rings with, so it sounds \"right\" over a dominant chord: Mixolydian with its clashing 4th raised out of the way." },
        { "mixolydian_b6", "Mixolydian b6", "", "mminor", detail::tones_mixolydian_b6, detail::semis_mixolydian_b6, true, false,
          detail::aka_mixolydian_b6, detail::char_mixolydian_b6, "mixolydian", 0.8, {},
          "",
          "",
          "",
          "Major but melancholy, wistful.",
          "Film music, rock and pop ballads, jazz over 7b13 chords.",
          "Its lower half is major and its upper half minor: it starts bright and turns sad." },
        { "locrian_s2", "Locrian #2 (half-diminished)", "", "mminor", detail::tones_locrian_s2, detail::semis_locrian_s2, true, false,
          detail::aka_locrian_s2, detail::char_locrian_s2, "locrian", 0.3, {},
          "",
          "",
          "",
          "Dark and unstable, but smoother than Locrian.",
          "Jazz over m7b5 (half-diminished) chords.",
          "Locrian with a natural 2nd: the same half-diminished home chord without the b2 grinding against the root." },
        { "altered", "Altered (super-Locrian)", "", "mminor", detail::tones_altered, detail::semis_altered, true, false,
          detail::aka_altered, detail::char_altered, "locrian", 0.6, detail::jazz_altered,
          "",
          "",
          "",
          "Maximum tension that melts into the next chord.",
          "Jazz and fusion over altered dominant (7alt) chords just before they resolve.",
          "Over a dominant 7th it keeps the root, 3rd (spelled b4) and 7th, and every other note is an altered tension (b9, #9, #11, b13). Pure tension, so it resolves beautifully." },
        { "major_pent", "Major pentatonic", "", "penta", detail::tones_major_pent, detail::semis_major_pent, false, false,
          {}, {}, "ionian", 0.6, {},
          "",
          "",
          "",
          "Open, sunny, simple, can't-go-wrong.",
          "Country, folk, rock, gospel, children's songs, and music all over the world (Scotland, China, West Africa).",
          "It leaves out the major scale's two half steps (the 4th and 7th), so no note clashes with any other. Every note sounds good over the home chord." },
        { "minor_pent", "Minor pentatonic", "", "penta", detail::tones_minor_pent, detail::semis_minor_pent, false, false,
          {}, {}, "aeolian", 0.5, {},
          "",
          "",
          "",
          "Bluesy, tough, soulful.",
          "Blues, rock and metal solos, R&B, funk, hip hop.",
          "Natural minor without its 2nd and b6, so it has no half steps at all. Easy to bend and wail on: the first scale most guitarists learn." },
        { "blues", "Blues scale (minor blues)", "", "penta", detail::tones_blues, detail::semis_blues, false, false,
          detail::aka_blues, detail::char_blues, "minor_pent", 0.8, {},
          "",
          "",
          "",
          "Gritty, bluesy, a little sly.",
          "Blues, rock, jazz, funk, soul.",
          "Minor pentatonic plus the b5, the \"blue note\". It is a passing note: it rubs for a moment, then slides to the 4th or the 5th." },
        { "major_blues", "Major blues", "", "penta", detail::tones_major_blues, detail::semis_major_blues, false, false,
          {}, detail::char_major_blues, "major_pent", 1.0, {},
          "",
          "",
          "",
          "Sweet, sunny and bluesy.",
          "Country, Western swing, bluegrass, blues, Southern rock.",
          "Major pentatonic plus the b3, which you slide or hammer up to the major 3rd: the country and blues \"grace note\" sound." },
        { "bebop_dominant", "Bebop dominant", "", "bebop", detail::tones_bebop_dominant, detail::semis_bebop_dominant, false, false,
          {}, detail::char_bebop_dominant, "mixolydian", 1.1, {},
          "",
          "",
          "",
          "Swinging, flowing, jazzy.",
          "Bebop and swing (Charlie Parker), jazz lines over dominant 7th chords.",
          "Mixolydian plus a passing major 7th. With eight notes, a run in 8th notes that starts on a chord tone on the beat keeps landing chord tones on the beat." },
        { "bebop_major", "Bebop major", "", "bebop", detail::tones_bebop_major, detail::semis_bebop_major, false, false,
          {}, detail::char_bebop_major, "ionian", 1.2, {},
          "",
          "",
          "",
          "Smooth, swinging, sophisticated.",
          "Jazz lines over major and 6th chords (Barry Harris writes the added note as a b6).",
          "Major plus a passing #5. Running it in 8th notes from a chord tone puts the 6th chord's notes (1, 3, 5, 6) on every beat." },
        { "bebop_dorian", "Bebop minor (Dorian bebop)", "", "bebop", detail::tones_bebop_dorian, detail::semis_bebop_dorian, false, false,
          detail::aka_bebop_dorian, detail::char_bebop_dorian, "dorian", 1.2, {},
          "",
          "",
          "",
          "Cool, swinging, bluesy minor.",
          "Jazz lines over minor 7th chords (and the ii of a ii, V, I).",
          "Dorian plus a passing major 3rd between the b3 and the 4th, so 8th-note runs keep the chord tones on the beat." },
        { "whole_tone", "Whole tone", "", "sym", detail::tones_whole_tone, detail::semis_whole_tone, false, true,
          {}, {}, "lydian_dominant", 0.5, {},
          "",
          "",
          "",
          "Dreamy, floating, hazy, unresolved.",
          "Debussy, TV dream sequences, Thelonious Monk, jazz over 7#5 chords.",
          "Every step is a whole step, so no note is more \"home\" than another and nothing pulls anywhere. There are only two different whole-tone scales." },
        { "dim_hw", "Diminished (half-whole)", "", "sym", detail::tones_dim_hw, detail::semis_dim_hw, false, true,
          detail::aka_dim_hw, detail::char_dim_hw, "mixolydian", 0.6, detail::jazz_dim_hw,
          "",
          "Compared with Mixolydian: it keeps 1, 3, 5, 6 and b7 (a dominant 13th chord), but trades the 2nd for both a b2 and a #2 (b9 and #9) and raises the 4th to #4.",
          "",
          "Tense, angular, jazzy.",
          "Jazz over 7b9 chords, film tension, metal, Stravinsky and Bartok.",
          "Half step, whole step, over and over: it repeats every 3 frets. Over a dominant 7th it gives b9, #9 and #11 while keeping the natural 5th and 13th." },
        { "dim_wh", "Diminished (whole-half)", "", "sym", detail::tones_dim_wh, detail::semis_dim_wh, false, true,
          detail::aka_dim_wh, detail::char_dim_wh, "dim_hw", 0.4, {},
          "",
          "Compared with half-whole diminished: the same alternating pattern, but starting with a whole step. That puts a diminished 7th chord (1 b3 b5 6) on its root instead of a dominant 7th.",
          "",
          "Mysterious, tense, symmetrical.",
          "Jazz over diminished 7th chords, film tension, metal.",
          "Whole step, half step, over and over. It holds a diminished 7th chord on its root, with a note a whole step above each chord tone." },
        { "chromatic", "Chromatic", "", "sym", detail::tones_chromatic, detail::semis_chromatic, false, true,
          {}, {}, nullptr, 99.0, {},
          "",
          "",
          "",
          "No flavor of its own: all twelve notes.",
          "Every style uses chromatic passing notes; whole melodies built on it are modern classical and film.",
          "Every half step. It is the set every other scale is chosen from, so it has no home of its own." },
        { "hungarian_minor", "Hungarian minor", "", "world", detail::tones_hungarian_minor, detail::semis_hungarian_minor, true, false,
          detail::aka_hungarian_minor, detail::char_hungarian_minor, "harmonic_minor", 1.2, {},
          "",
          "",
          "Some older books call it the \"Gypsy minor\". Hungarian minor is the standard, respectful name.",
          "Dramatic, dark, passionate.",
          "Hungarian and Romani music, Liszt, flamenco-tinged and neoclassical metal.",
          "Harmonic minor with a raised 4th: two step-and-a-half gaps (b3 to #4, b6 to 7), each landing a half step under a strong note." },
        { "hungarian_major", "Hungarian major", "", "world", detail::tones_hungarian_major, detail::semis_hungarian_major, true, false,
          {}, detail::char_hungarian_major, "lydian_dominant", 1.5, {},
          "",
          "",
          "",
          "Bright, strange, festive.",
          "Liszt and Hungarian-inspired classical music; jazz over 7#9#11 chords.",
          "A dominant 7th home chord with a #2 and a #4 around it: bright, but tilted." },
        { "double_harmonic", "Double harmonic major (Byzantine)", "", "world", detail::tones_double_harmonic, detail::semis_double_harmonic, true, false,
          detail::aka_double_harmonic, detail::char_double_harmonic, "phrygian_dominant", 1.3, {},
          "",
          "",
          "",
          "Exotic, ornate, intense.",
          "Byzantine chant, Arabic, Turkish and Greek music, North Indian classical (Raga Bhairav), surf rock (Misirlou), desert scenes in film.",
          "Two step-and-a-half gaps, each wrapped in half steps (b2 to 3, b6 to 7), so every strong note has a neighbor a half step away." },
        { "neapolitan_minor", "Neapolitan minor", "", "world", detail::tones_neapolitan_minor, detail::semis_neapolitan_minor, true, false,
          {}, detail::char_neapolitan_minor, "harmonic_minor", 1.3, {},
          "",
          "",
          "",
          "Dark, operatic, dramatic.",
          "Classical and opera, metal.",
          "Harmonic minor with a b2, so the dark Neapolitan chord (bII) lives right in the scale." },
        { "neapolitan_major", "Neapolitan major", "", "world", detail::tones_neapolitan_major, detail::semis_neapolitan_major, true, false,
          {}, detail::char_neapolitan_major, "melodic_minor", 1.4, {},
          "",
          "",
          "",
          "Dark at the bottom, bright at the top.",
          "Classical and opera; jazz.",
          "Melodic minor with a b2: a heavy start (1, b2, b3) that climbs into a bright major top half." },
        { "persian", "Persian", "", "world", detail::tones_persian, detail::semis_persian, true, false,
          {}, detail::char_persian, "double_harmonic", 1.6, {},
          "A Western name. Persian classical music (the dastgah system) uses intervals that fall between our 12 notes, so this is a 12-tone flavor, not a real dastgah.",
          "",
          "",
          "Mysterious, tense, exotic.",
          "Western film and metal music reaching for a Persian color.",
          "Double harmonic with a flat 5th: half steps everywhere, and a home chord that is neither major nor minor but tense." },
        { "hirajoshi", "Hirajoshi", "", "world", detail::tones_hirajoshi, detail::semis_hirajoshi, false, false,
          {}, {}, "aeolian", 1.4, {},
          "",
          "",
          "Sources disagree on this name. This is the version most guitar books use.",
          "Haunting, spare, still.",
          "Japanese koto music as taught in the West, metal, film and game music.",
          "Five notes with two half steps (2 to b3, 5 to b6) and two wide leaps: a sparse, bell-like sound." },
        { "in", "In (Miyako-bushi)", "", "world", detail::tones_in, detail::semis_in, false, false,
          detail::aka_in, detail::char_in, "phrygian", 1.4, {},
          "",
          "",
          "",
          "Melancholy and still.",
          "Japanese music for koto and shamisen (the folk song Sakura).",
          "Two half steps (1 to b2, 5 to b6) and two big leaps: tender and sad." },
        { "insen", "Insen", "", "world", detail::tones_insen, detail::semis_insen, false, false,
          {}, detail::char_insen, "in", 1.5, {},
          "",
          "",
          "",
          "Dark, open, meditative.",
          "Japanese music, and Western film and game music wanting a Japanese color.",
          "Like In, but with a b7 instead of a b6: one half step fewer, so it sounds more open." },
        { "iwato", "Iwato", "", "world", detail::tones_iwato, detail::semis_iwato, false, false,
          {}, detail::char_iwato, "locrian", 1.6, {},
          "",
          "",
          "",
          "Dark, eerie, unresolved.",
          "Japanese music; metal and film music.",
          "Five notes from Locrian: the b2 and b5 make it one of the darkest pentatonic scales." },
        { "yo", "Yo", "", "world", detail::tones_yo, detail::semis_yo, false, false,
          {}, {}, "major_pent", 1.4, {},
          "",
          "",
          "",
          "Open, bright, folk-like.",
          "Japanese folk songs (min'yo) and court music.",
          "No half steps, like major pentatonic, but with a 4th in place of the 3rd: open, neither major nor minor." },
        { "kumoi", "Kumoi", "", "world", detail::tones_kumoi, detail::semis_kumoi, false, false,
          {}, {}, "dorian", 1.5, {},
          "",
          "",
          "Names for Japanese scales vary between sources. This is the version most Western references use.",
          "Gentle, wistful.",
          "Japanese-flavored music; also used in jazz.",
          "A minor 3rd and a major 6th with only one half step (2 to b3): soft minor color." },
        { "pelog", "Pelog (12-tone approximation)", "", "world", detail::tones_pelog, detail::semis_pelog, false, false,
          {}, {}, "phrygian", 1.6, {},
          "Real pelog, from Javanese and Balinese gamelan, has seven uneven steps that differ from one gamelan to the next. This five-note set (close to pelog selisir) is the usual approximation on Western instruments.",
          "",
          "",
          "Shimmering, ancient, solemn.",
          "Javanese and Balinese gamelan (in its real tuning).",
          "Small and large steps side by side (1, b2, b3, then a leap to 5), like the uneven steps of real pelog." },
        { "slendro", "Slendro (12-tone approximation)", "", "world", detail::tones_slendro, detail::semis_slendro, false, false,
          {}, {}, "minor_pent", 1.6, {},
          "Real slendro, from gamelan, divides the octave into five nearly equal steps (about 240 cents each). No set of piano keys can do that, so any 12-tone version is a rough fit; this is a common one.",
          "",
          "",
          "Floating, calm, gong-like.",
          "Javanese and Balinese gamelan (in its real tuning).",
          "Five notes with no half steps, as close as 12 notes get to five equal steps." },
    } };

    // The major modes in order of brightness, brightest first
    inline constexpr std::array<const char*, 7> brightness { "lydian", "ionian", "mixolydian", "dorian", "aeolian", "phrygian", "locrian" };

    // Families of modes: mode 1, 2, ... built on each degree of the first
    inline constexpr std::array<Family, 3> families { {
        { "major", "major", { "ionian", "dorian", "phrygian", "lydian", "mixolydian", "aeolian", "locrian" } },
        { "harmonic_minor", "harmonic minor", { "harmonic_minor", "locrian_n6", "ionian_s5", "ukrainian_dorian", "phrygian_dominant", "lydian_s2", "ultralocrian" } },
        { "melodic_minor", "melodic minor", { "melodic_minor", "dorian_b2", "lydian_aug", "lydian_dominant", "mixolydian_b6", "locrian_s2", "altered" } },
    } };

    // The roots a player picks from: every note with at most one sharp or flat that is commonly used
    inline constexpr std::array<const char*, 17> roots { "C", "C#", "Db", "D", "D#", "Eb", "E", "F", "F#", "Gb", "G", "G#", "Ab", "A", "A#", "Bb", "B" };

    // The scale with this id, or nullptr
    constexpr const LibraryScale* find (std::string_view id)
    {
        for (auto& s : library) if (id == s.id) return &s;
        return nullptr;
    }
}
