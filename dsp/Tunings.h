// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// The tuner's instruments, tunings, temperaments and reference pitches.
// Made by tools/tunings.py (edit the list there and run it, not this file).
// Names are UTF-8.
#pragma once

#include <array>
#include <vector>

namespace vs::tunings
{
    // strings: every string's MIDI note (60 = C4), low side first. courses: how many strings sound together
    // in each pair or group, in the same order (empty: every string on its own)
    struct Tuning     { const char* name; std::vector<int> strings; std::vector<int> courses; const char* note; };
    // transpose: written = concert + this. part: what the tuner numbers ("string" counts from the high side,
    // "tine" or "drum" from the left, "" names nothing)
    struct Instrument { const char* name; int transpose; const char* part; std::vector<Tuning> tunings; };
    struct Family     { const char* name; std::vector<Instrument> instruments; };
    struct Temperament { const char* name; std::array<float, 12> cents; };                  // offsets from equal temperament, built on C
    struct Reference  { const char* name; int hz; };

    inline const std::vector<Family>& families()
    {
        static const std::vector<Family> list {
            { "Guitars", {
                { "Guitar (6-string)", 0, "string", { { "Standard", { 40, 45, 50, 55, 59, 64 }, {  }, "" }, { "Half step down (Eb)", { 39, 44, 49, 54, 58, 63 }, {  }, "" }, { "Whole step down (D)", { 38, 43, 48, 53, 57, 62 }, {  }, "" }, { "C standard", { 36, 41, 46, 51, 55, 60 }, {  }, "" }, { "B standard", { 35, 40, 45, 50, 54, 59 }, {  }, "" }, { "Drop D", { 38, 45, 50, 55, 59, 64 }, {  }, "" }, { "Drop C#", { 37, 44, 49, 54, 58, 63 }, {  }, "" }, { "Drop C", { 36, 43, 48, 53, 57, 62 }, {  }, "" }, { "Drop B", { 35, 42, 47, 52, 56, 61 }, {  }, "" }, { "Drop A", { 33, 40, 45, 50, 54, 59 }, {  }, "" }, { "Double drop D", { 38, 45, 50, 55, 59, 62 }, {  }, "" }, { "DADGAD", { 38, 45, 50, 55, 57, 62 }, {  }, "" }, { "Open G", { 38, 43, 50, 55, 59, 62 }, {  }, "" }, { "Open D", { 38, 45, 50, 54, 57, 62 }, {  }, "" }, { "Open E", { 40, 47, 52, 56, 59, 64 }, {  }, "" }, { "Open A", { 40, 45, 52, 57, 61, 64 }, {  }, "" }, { "Open C", { 36, 43, 48, 55, 60, 64 }, {  }, "" }, { "Open D minor", { 38, 45, 50, 53, 57, 62 }, {  }, "" }, { "Open G minor", { 38, 43, 50, 55, 58, 62 }, {  }, "" }, { "Open C6", { 36, 45, 48, 55, 60, 64 }, {  }, "" }, { "All fourths", { 40, 45, 50, 55, 60, 65 }, {  }, "" }, { "New standard (Fripp)", { 36, 43, 50, 57, 64, 67 }, {  }, "" }, { "Nashville (high strung)", { 52, 57, 62, 67, 59, 64 }, {  }, "The four low strings an octave up, from a 12-string set" }, { "Open G (Keith Richards 5-string)", { 43, 50, 55, 59, 62 }, {  }, "" } } },
                { "Guitar (12-string)", 0, "string", { { "Standard", { 40, 52, 45, 57, 50, 62, 55, 67, 59, 59, 64, 64 }, { 2, 2, 2, 2, 2, 2 }, "The four low pairs have an octave string; the B and E pairs are two strings at the same pitch" }, { "Half step down (Eb)", { 39, 51, 44, 56, 49, 61, 54, 66, 58, 58, 63, 63 }, { 2, 2, 2, 2, 2, 2 }, "" }, { "Whole step down (D)", { 38, 50, 43, 55, 48, 60, 53, 65, 57, 57, 62, 62 }, { 2, 2, 2, 2, 2, 2 }, "" }, { "Drop D", { 38, 50, 45, 57, 50, 62, 55, 67, 59, 59, 64, 64 }, { 2, 2, 2, 2, 2, 2 }, "" }, { "DADGAD", { 38, 50, 45, 57, 50, 62, 55, 67, 57, 57, 62, 62 }, { 2, 2, 2, 2, 2, 2 }, "" }, { "Open G", { 38, 50, 43, 55, 50, 62, 55, 67, 59, 59, 62, 62 }, { 2, 2, 2, 2, 2, 2 }, "" }, { "Open D", { 38, 50, 45, 57, 50, 62, 54, 66, 57, 57, 62, 62 }, { 2, 2, 2, 2, 2, 2 }, "" } } },
                { "Guitar (7-string)", 0, "string", { { "Standard", { 35, 40, 45, 50, 55, 59, 64 }, {  }, "" }, { "Drop A", { 33, 40, 45, 50, 55, 59, 64 }, {  }, "" }, { "Half step down", { 34, 39, 44, 49, 54, 58, 63 }, {  }, "" }, { "Russian (7-string)", { 38, 43, 47, 50, 55, 59, 62 }, {  }, "Open G, the Russian guitar's own tuning" } } },
                { "Guitar (8-string)", 0, "string", { { "Standard", { 30, 35, 40, 45, 50, 55, 59, 64 }, {  }, "" }, { "Drop E", { 28, 35, 40, 45, 50, 55, 59, 64 }, {  }, "" } } },
                { "Baritone guitar", 0, "string", { { "B standard", { 35, 40, 45, 50, 54, 59 }, {  }, "" }, { "A standard", { 33, 38, 43, 48, 52, 57 }, {  }, "" } } },
                { "Tenor guitar", 0, "string", { { "Standard (fifths)", { 48, 55, 62, 69 }, {  }, "" }, { "Chicago", { 50, 55, 59, 64 }, {  }, "" } } },
                { "Guitalele", 0, "string", { { "Standard", { 45, 50, 55, 60, 64, 69 }, {  }, "" } } },
                { "Requinto guitar", 0, "string", { { "Standard", { 45, 50, 55, 60, 64, 69 }, {  }, "" } } },
                { "Resonator / Dobro", 0, "string", { { "Open G (high bass)", { 43, 47, 50, 55, 59, 62 }, {  }, "" }, { "Open D", { 38, 45, 50, 54, 57, 62 }, {  }, "" } } },
                { "Lap steel", 0, "string", { { "C6", { 48, 52, 55, 57, 60, 64 }, {  }, "" }, { "A6", { 49, 52, 54, 57, 61, 64 }, {  }, "" }, { "Open E", { 40, 47, 52, 56, 59, 64 }, {  }, "" }, { "Open G", { 38, 43, 50, 55, 59, 62 }, {  }, "" } } },
                { "Pedal steel (10-string)", 0, "string", { { "E9", { 47, 50, 52, 54, 56, 59, 64, 68, 63, 66 }, {  }, "Strings 10 to 1, open (no pedals)" }, { "C6", { 36, 41, 45, 48, 52, 55, 57, 60, 64, 62 }, {  }, "Strings 10 to 1, open (no pedals)" } } },
            } },
            { "Basses", {
                { "Bass (4-string)", 0, "string", { { "Standard", { 28, 33, 38, 43 }, {  }, "" }, { "Drop D", { 26, 33, 38, 43 }, {  }, "" }, { "Half step down", { 27, 32, 37, 42 }, {  }, "" }, { "D standard", { 26, 31, 36, 41 }, {  }, "" }, { "Drop C", { 24, 31, 36, 41 }, {  }, "" }, { "BEAD", { 23, 28, 33, 38 }, {  }, "" }, { "Tenor (ADGC)", { 33, 38, 43, 48 }, {  }, "" }, { "Fifths (cello-style)", { 24, 31, 38, 45 }, {  }, "" }, { "Piccolo bass", { 40, 45, 50, 55 }, {  }, "" } } },
                { "Bass (5-string)", 0, "string", { { "Standard (low B)", { 23, 28, 33, 38, 43 }, {  }, "" }, { "High C", { 28, 33, 38, 43, 48 }, {  }, "" }, { "Drop A", { 21, 28, 33, 38, 43 }, {  }, "" } } },
                { "Bass (6-string)", 0, "string", { { "Standard", { 23, 28, 33, 38, 43, 48 }, {  }, "" } } },
                { "Upright / double bass", 0, "string", { { "Orchestral (fourths)", { 28, 33, 38, 43 }, {  }, "" }, { "5-string (low B)", { 23, 28, 33, 38, 43 }, {  }, "" }, { "Solo tuning", { 30, 35, 40, 45 }, {  }, "A whole step up, for solo playing with solo strings" }, { "Fifths", { 24, 31, 38, 45 }, {  }, "" } } },
                { "Acoustic bass guitar", 0, "string", { { "Standard", { 28, 33, 38, 43 }, {  }, "" } } },
                { "Guitarron (Mexican)", 0, "string", { { "Standard", { 33, 38, 43, 48, 40, 45 }, {  }, "Re-entrant: the 5th and 6th strings drop back down" } } },
                { "Bajo sexto", 0, "string", { { "Standard", { 28, 40, 33, 45, 38, 50, 43, 43, 48, 48, 53, 53 }, { 2, 2, 2, 2, 2, 2 }, "About an octave below a 12-string: the three low pairs have an octave string, the top three are pairs at the same pitch" }, { "Solo (B and E on top)", { 28, 40, 33, 45, 38, 50, 43, 43, 47, 47, 52, 52 }, { 2, 2, 2, 2, 2, 2 }, "" } } },
                { "Bajo quinto", 0, "string", { { "Standard", { 33, 45, 38, 50, 43, 43, 48, 48, 53, 53 }, { 2, 2, 2, 2, 2 }, "" } } },
            } },
            { "Ukuleles", {
                { "Ukulele (soprano, concert, tenor)", 0, "string", { { "Standard (high G)", { 67, 60, 64, 69 }, {  }, "Re-entrant: the G string is higher than the C" }, { "Low G", { 55, 60, 64, 69 }, {  }, "" }, { "D tuning (old-style)", { 69, 62, 66, 71 }, {  }, "" }, { "Slack key (open C)", { 67, 60, 64, 67 }, {  }, "" } } },
                { "Baritone ukulele", 0, "string", { { "Standard", { 50, 55, 59, 64 }, {  }, "" } } },
                { "Bass ukulele", 0, "string", { { "Standard", { 28, 33, 38, 43 }, {  }, "" } } },
                { "Banjolele", 0, "string", { { "Standard", { 67, 60, 64, 69 }, {  }, "" } } },
            } },
            { "Mandolins", {
                { "Mandolin", 0, "string", { { "Standard", { 55, 55, 62, 62, 69, 69, 76, 76 }, { 2, 2, 2, 2 }, "Four pairs, both strings of a pair at the same pitch" }, { "Cross A (AEAE)", { 57, 57, 64, 64, 69, 69, 76, 76 }, { 2, 2, 2, 2 }, "" }, { "Open G (GDGD)", { 55, 55, 62, 62, 67, 67, 74, 74 }, { 2, 2, 2, 2 }, "" }, { "Cross G (GDGB)", { 55, 55, 62, 62, 67, 67, 71, 71 }, { 2, 2, 2, 2 }, "" } } },
                { "Mandola", 0, "string", { { "Standard", { 48, 48, 55, 55, 62, 62, 69, 69 }, { 2, 2, 2, 2 }, "" } } },
                { "Octave mandolin", 0, "string", { { "Standard", { 43, 43, 50, 50, 57, 57, 64, 64 }, { 2, 2, 2, 2 }, "" }, { "Octave strings on the low pairs", { 43, 55, 50, 62, 57, 57, 64, 64 }, { 2, 2, 2, 2 }, "" } } },
                { "Mandocello", 0, "string", { { "Standard", { 36, 36, 43, 43, 50, 50, 57, 57 }, { 2, 2, 2, 2 }, "" } } },
            } },
            { "Banjos", {
                { "Banjo (5-string)", 0, "string", { { "Open G", { 67, 50, 55, 59, 62 }, {  }, "The short 5th string comes first" }, { "Double C", { 67, 48, 55, 60, 62 }, {  }, "" }, { "Standard C", { 67, 48, 55, 59, 62 }, {  }, "" }, { "Sawmill / modal (gDGCD)", { 67, 50, 55, 60, 62 }, {  }, "" }, { "Open G minor (gDGBbD)", { 67, 50, 55, 58, 62 }, {  }, "" }, { "Open D", { 66, 50, 54, 57, 62 }, {  }, "" }, { "Open D minor (aDFAD)", { 69, 50, 53, 57, 62 }, {  }, "" }, { "Open A", { 69, 52, 57, 61, 64 }, {  }, "" } } },
                { "Tenor banjo", 0, "string", { { "Standard (fifths)", { 48, 55, 62, 69 }, {  }, "" }, { "Irish", { 43, 50, 57, 64 }, {  }, "" } } },
                { "Plectrum banjo", 0, "string", { { "Standard", { 48, 55, 59, 62 }, {  }, "" } } },
                { "Banjo (6-string)", 0, "string", { { "Standard (guitar)", { 40, 45, 50, 55, 59, 64 }, {  }, "" } } },
            } },
            { "Orchestral strings", {
                { "Violin / fiddle", 0, "string", { { "Standard", { 55, 62, 69, 76 }, {  }, "" }, { "Cross A (AEAE)", { 57, 64, 69, 76 }, {  }, "" }, { "Cross G (GDGD)", { 55, 62, 67, 74 }, {  }, "" }, { "ADAE", { 57, 62, 69, 76 }, {  }, "" }, { "Calico (AEAC#)", { 57, 64, 69, 73 }, {  }, "" }, { "DDAD", { 50, 62, 69, 74 }, {  }, "" }, { "Baroque (A = 415)", { 55, 62, 69, 76 }, {  }, "Set the reference to 415 Hz" } } },
                { "Viola", 0, "string", { { "Standard", { 48, 55, 62, 69 }, {  }, "" } } },
                { "Cello", 0, "string", { { "Standard", { 36, 43, 50, 57 }, {  }, "" }, { "Bach suite 5 (scordatura)", { 36, 43, 50, 55 }, {  }, "" } } },
                { "Hardanger fiddle", 0, "string", { { "Standard (ADAE)", { 57, 62, 69, 76, 59, 62, 64, 66, 69 }, {  }, "The 4 melody strings, then the 5 sympathetic strings under them (a common setup; it changes with the piece)" } } },
                { "Nyckelharpa", 0, "string", { { "Standard", { 48, 55, 60, 69 }, {  }, "The 4 played strings; tune the sympathetic strings by ear" } } },
            } },
            { "Early music", {
                { "Renaissance lute (6-course)", 0, "string", { { "In G", { 43, 55, 48, 60, 53, 65, 57, 57, 62, 62, 67 }, { 2, 2, 2, 2, 2, 1 }, "Single top string; octave strings on the three low pairs (the 4th is sometimes at the same pitch)" }, { "In A", { 45, 57, 50, 62, 55, 67, 59, 59, 64, 64, 69 }, { 2, 2, 2, 2, 2, 1 }, "" } } },
                { "Renaissance lute (8-course)", 0, "string", { { "In G", { 38, 50, 41, 53, 43, 55, 48, 60, 53, 65, 57, 57, 62, 62, 67 }, { 2, 2, 2, 2, 2, 2, 2, 1 }, "Single top string; octave strings on the five low pairs" } } },
                { "Baroque lute (13-course)", 0, "string", { { "D minor", { 33, 45, 35, 47, 36, 48, 38, 50, 40, 52, 41, 53, 43, 55, 45, 57, 50, 50, 53, 53, 57, 57, 62, 65 }, { 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1 }, "Two single top strings, octave strings on the eight basses; the basses are often retuned to the key of the piece" } } },
                { "Viola da gamba", 0, "string", { { "Bass viol", { 38, 43, 48, 52, 57, 62 }, {  }, "" }, { "Bass viol (7-string)", { 33, 38, 43, 48, 52, 57, 62 }, {  }, "" }, { "Tenor viol", { 43, 48, 53, 57, 62, 67 }, {  }, "" }, { "Treble viol", { 50, 55, 60, 64, 69, 74 }, {  }, "" }, { "Violone in G", { 31, 36, 41, 45, 50, 55 }, {  }, "" } } },
                { "Vihuela (Spanish)", 0, "string", { { "Standard", { 43, 43, 48, 48, 53, 53, 57, 57, 62, 62, 67 }, { 2, 2, 2, 2, 2, 1 }, "Single top string, the others in pairs" } } },
                { "Baroque guitar (5-course)", 0, "string", { { "Re-entrant (Spanish)", { 57, 57, 62, 62, 55, 55, 59, 59, 64 }, { 2, 2, 2, 2, 1 }, "No low strings: the 4th and 5th pairs are tuned high" }, { "French (bourdon on the 4th)", { 57, 57, 50, 62, 55, 55, 59, 59, 64 }, { 2, 2, 2, 2, 1 }, "" }, { "Bourdons on the 4th and 5th", { 45, 57, 50, 62, 55, 55, 59, 59, 64 }, { 2, 2, 2, 2, 1 }, "" } } },
                { "Hurdy-gurdy", 0, "string", { { "G/C", { 48, 55, 60, 67, 67 }, { 1, 1, 1, 2 }, "Drones (bourdon C, mouche G, trompette C), then the two melody strings on G. Drone setups vary by maker" } } },
            } },
            { "Folk and world strings", {
                { "Bouzouki (Irish)", 0, "string", { { "GDAD (octave low pairs)", { 43, 55, 50, 62, 57, 57, 62, 62 }, { 2, 2, 2, 2 }, "Octave strings on the G and D pairs, a common setup" }, { "GDAD (all pairs the same pitch)", { 43, 43, 50, 50, 57, 57, 62, 62 }, { 2, 2, 2, 2 }, "" }, { "GDAE", { 43, 55, 50, 62, 57, 57, 64, 64 }, { 2, 2, 2, 2 }, "" }, { "ADAD", { 45, 57, 50, 62, 57, 57, 62, 62 }, { 2, 2, 2, 2 }, "" } } },
                { "Bouzouki (Greek)", 0, "string", { { "Tetrachordo (4-course)", { 48, 60, 53, 65, 57, 57, 62, 62 }, { 2, 2, 2, 2 }, "" }, { "Trichordo (3-course)", { 50, 62, 57, 57, 62, 62 }, { 2, 2, 2 }, "" } } },
                { "Oud", 0, "string", { { "Arabic", { 36, 41, 41, 45, 45, 50, 50, 55, 55, 60, 60 }, { 1, 2, 2, 2, 2, 2 }, "Single low string, then five pairs" }, { "Turkish", { 40, 45, 45, 47, 47, 52, 52, 57, 57, 62, 62 }, { 1, 2, 2, 2, 2, 2 }, "" } } },
                { "Appalachian dulcimer", 0, "string", { { "DAd (Ionian)", { 50, 57, 62 }, {  }, "" }, { "DAdd (doubled melody string)", { 50, 57, 62, 62 }, { 1, 1, 2 }, "" }, { "DAA (Mixolydian)", { 50, 57, 57 }, {  }, "" }, { "DGD", { 50, 55, 62 }, {  }, "" }, { "CGc", { 48, 55, 60 }, {  }, "" } } },
                { "Balalaika (prima)", 0, "string", { { "Standard", { 64, 64, 69 }, {  }, "" } } },
                { "Domra", 0, "string", { { "3-string", { 64, 69, 74 }, {  }, "" }, { "4-string", { 55, 62, 69, 76 }, {  }, "" } } },
                { "Charango", 0, "string", { { "Standard", { 67, 67, 72, 72, 64, 76, 69, 69, 76, 76 }, { 2, 2, 2, 2, 2 }, "Re-entrant: the middle pair has the lowest string, an octave under its partner" } } },
                { "Cavaquinho", 0, "string", { { "Standard (DGBD)", { 62, 67, 71, 74 }, {  }, "" } } },
                { "Cuatro (Venezuelan)", 0, "string", { { "Standard", { 57, 62, 66, 59 }, {  }, "Re-entrant: the 1st string is lower than the 2nd" } } },
                { "Cuatro (Puerto Rican)", 0, "string", { { "Standard", { 47, 59, 52, 64, 57, 57, 62, 62, 67, 67 }, { 2, 2, 2, 2, 2 }, "Octave strings on the B and E pairs" } } },
                { "Tres (Cuban)", 0, "string", { { "Standard (C)", { 55, 67, 60, 60, 64, 76 }, { 2, 2, 2 }, "Octave strings on the G and E pairs" }, { "Unison top pair", { 55, 67, 60, 60, 64, 64 }, { 2, 2, 2 }, "" }, { "D tuning", { 57, 69, 62, 62, 66, 78 }, { 2, 2, 2 }, "" } } },
                { "Tres (Puerto Rican)", 0, "string", { { "Standard", { 55, 67, 67, 60, 60, 60, 52, 64, 64 }, { 3, 3, 3 }, "Three groups of three strings" } } },
                { "Vihuela (Mexican)", 0, "string", { { "Standard", { 57, 62, 67, 59, 64 }, {  }, "Re-entrant" } } },
                { "Tiple (Colombian)", 0, "string", { { "Modern", { 50, 62, 62, 55, 67, 67, 59, 71, 71, 64, 64, 64 }, { 3, 3, 3, 3 }, "Three groups of three: the middle string of the low three groups is an octave down" }, { "Traditional", { 48, 60, 60, 53, 65, 65, 57, 69, 69, 62, 62, 62 }, { 3, 3, 3, 3 }, "" } } },
                { "Bandurria", 0, "string", { { "Standard", { 56, 56, 61, 61, 66, 66, 71, 71, 76, 76, 81, 81 }, { 2, 2, 2, 2, 2, 2 }, "" } } },
                { "Laud (Spanish)", 0, "string", { { "Standard", { 44, 44, 49, 49, 54, 54, 59, 59, 64, 64, 69, 69 }, { 2, 2, 2, 2, 2, 2 }, "" } } },
                { "Kantele (5-string)", 0, "string", { { "Minor", { 62, 64, 65, 67, 69 }, {  }, "" }, { "Major", { 62, 64, 66, 67, 69 }, {  }, "" } } },
                { "Pipa", 0, "string", { { "Standard", { 45, 50, 52, 57 }, {  }, "" } } },
                { "Zhongruan", 0, "string", { { "Standard", { 43, 50, 55, 62 }, {  }, "" } } },
                { "Erhu", 0, "string", { { "Standard", { 62, 69 }, {  }, "" } } },
                { "Shamisen", 0, "string", { { "Honchoshi", { 48, 53, 60 }, {  }, "Usually tuned to the singer: move it all up or down together" }, { "Niagari", { 48, 55, 60 }, {  }, "" }, { "Sansagari", { 48, 53, 58 }, {  }, "" } } },
                { "Koto (13-string)", 0, "string", { { "Hirajoshi", { 50, 55, 57, 58, 62, 63, 67, 69, 70, 74, 75, 79, 81 }, {  }, "" } } },
            } },
            { "Winds and brass", {
                { "Flute", 0, "", { { "Band tuning note (concert Bb)", { 70 }, {  }, "Sharp: pull the headjoint out a little. Flat: push it in. Then check a low and a high note too." }, { "Orchestra tuning note (concert A)", { 69 }, {  }, "Sharp: pull the headjoint out a little. Flat: push it in. Then check a low and a high note too." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Piccolo", -12, "", { { "Band tuning note (concert Bb)", { 82 }, {  }, "Sharp: pull the headjoint out a little. Flat: push it in. Then check a low and a high note too." }, { "Orchestra tuning note (concert A)", { 81 }, {  }, "Sharp: pull the headjoint out a little. Flat: push it in. Then check a low and a high note too." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Oboe", 0, "", { { "Band tuning note (concert Bb)", { 70 }, {  }, "Mostly the reed: push it further in to go sharper, ease it out a little to go flatter. Your embouchure does the rest." }, { "Orchestra tuning note (concert A)", { 69 }, {  }, "Mostly the reed: push it further in to go sharper, ease it out a little to go flatter. Your embouchure does the rest." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "English horn (F)", 7, "", { { "Band tuning note (concert Bb)", { 70 }, {  }, "Mostly the bocal and reed: push in to go sharper, ease out to go flatter." }, { "Orchestra tuning note (concert A)", { 69 }, {  }, "Mostly the bocal and reed: push in to go sharper, ease out to go flatter." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Clarinet (Bb)", 2, "", { { "Band tuning note (concert Bb)", { 70 }, {  }, "Sharp: pull the barrel out a little (and the middle joint, if the low notes are sharp too). Flat: push it in." }, { "Orchestra tuning note (concert A)", { 69 }, {  }, "Sharp: pull the barrel out a little (and the middle joint, if the low notes are sharp too). Flat: push it in." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Clarinet (A)", 3, "", { { "Band tuning note (concert Bb)", { 70 }, {  }, "Sharp: pull the barrel out a little (and the middle joint, if the low notes are sharp too). Flat: push it in." }, { "Orchestra tuning note (concert A)", { 69 }, {  }, "Sharp: pull the barrel out a little (and the middle joint, if the low notes are sharp too). Flat: push it in." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Bass clarinet (Bb)", 14, "", { { "Band tuning note (concert Bb)", { 58 }, {  }, "Sharp: pull the neck out a little. Flat: push it in." }, { "Orchestra tuning note (concert A)", { 57 }, {  }, "Sharp: pull the neck out a little. Flat: push it in." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Bassoon", 0, "", { { "Band tuning note (concert Bb)", { 58 }, {  }, "Mostly the bocal and reed: a longer bocal (a higher number) plays flatter, a shorter one sharper. Your embouchure does the fine work." }, { "Orchestra tuning note (concert A)", { 57 }, {  }, "Mostly the bocal and reed: a longer bocal (a higher number) plays flatter, a shorter one sharper. Your embouchure does the fine work." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Soprano saxophone (Bb)", 2, "", { { "Band tuning note (concert Bb)", { 70 }, {  }, "Sharp: pull the mouthpiece out a little on the neck cork. Flat: push it further on." }, { "Orchestra tuning note (concert A)", { 69 }, {  }, "Sharp: pull the mouthpiece out a little on the neck cork. Flat: push it further on." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Alto saxophone (Eb)", 9, "", { { "Band tuning note (concert Bb)", { 70 }, {  }, "Sharp: pull the mouthpiece out a little on the neck cork. Flat: push it further on." }, { "Orchestra tuning note (concert A)", { 69 }, {  }, "Sharp: pull the mouthpiece out a little on the neck cork. Flat: push it further on." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Tenor saxophone (Bb)", 14, "", { { "Band tuning note (concert Bb)", { 58 }, {  }, "Sharp: pull the mouthpiece out a little on the neck cork. Flat: push it further on." }, { "Orchestra tuning note (concert A)", { 57 }, {  }, "Sharp: pull the mouthpiece out a little on the neck cork. Flat: push it further on." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Baritone saxophone (Eb)", 21, "", { { "Band tuning note (concert Bb)", { 46 }, {  }, "Sharp: pull the mouthpiece out a little on the neck cork. Flat: push it further on." }, { "Orchestra tuning note (concert A)", { 45 }, {  }, "Sharp: pull the mouthpiece out a little on the neck cork. Flat: push it further on." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Trumpet / cornet (Bb)", 2, "", { { "Band tuning note (concert Bb)", { 70 }, {  }, "Sharp: pull the main tuning slide out. Flat: push it in." }, { "Orchestra tuning note (concert A)", { 69 }, {  }, "Sharp: pull the main tuning slide out. Flat: push it in." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Flugelhorn (Bb)", 2, "", { { "Band tuning note (concert Bb)", { 70 }, {  }, "Sharp: pull the main tuning slide out. Flat: push it in." }, { "Orchestra tuning note (concert A)", { 69 }, {  }, "Sharp: pull the main tuning slide out. Flat: push it in." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "French horn (F)", 7, "", { { "Band tuning note (concert Bb)", { 58 }, {  }, "Sharp: pull the main tuning slide out. Flat: push it in. On a double horn, tune the F and Bb sides with their own slides." }, { "Orchestra tuning note (concert A)", { 57 }, {  }, "Sharp: pull the main tuning slide out. Flat: push it in. On a double horn, tune the F and Bb sides with their own slides." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Mellophone (F)", 7, "", { { "Band tuning note (concert Bb)", { 70 }, {  }, "Sharp: pull the main tuning slide out. Flat: push it in." }, { "Orchestra tuning note (concert A)", { 69 }, {  }, "Sharp: pull the main tuning slide out. Flat: push it in." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Trombone", 0, "", { { "Band tuning note (concert Bb)", { 58 }, {  }, "Sharp: pull the tuning slide (at the back) out. Flat: push it in. Concert Bb is first position." }, { "Orchestra tuning note (concert A)", { 57 }, {  }, "Sharp: pull the tuning slide (at the back) out. Flat: push it in. Concert Bb is first position." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Euphonium / baritone horn", 0, "", { { "Band tuning note (concert Bb)", { 58 }, {  }, "Sharp: pull the main tuning slide out. Flat: push it in." }, { "Orchestra tuning note (concert A)", { 57 }, {  }, "Sharp: pull the main tuning slide out. Flat: push it in." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
                { "Tuba", 0, "", { { "Band tuning note (concert Bb)", { 46 }, {  }, "Sharp: pull the main tuning slide out. Flat: push it in." }, { "Orchestra tuning note (concert A)", { 45 }, {  }, "Sharp: pull the main tuning slide out. Flat: push it in." }, { "Any note (long tones)", {  }, {  }, "Hold each note steady: you'll learn which ones run sharp or flat on your instrument" } } },
            } },
            { "Percussion", {
                { "Timpani", 0, "drum", { { "Pair: G and C", { 43, 48 }, {  }, "29\" and 26\" drums. Tap where you'd play, a hand's width in from the edge, and set each drum with its pedal" }, { "Pair: A and D", { 45, 50 }, {  }, "29\" and 26\" drums" }, { "Pair: F and Bb", { 41, 46 }, {  }, "29\" and 26\" drums" }, { "Any note", {  }, {  }, "Usual ranges: 32\" D2 to A2, 29\" F2 to C3, 26\" Bb2 to F3, 23\" D3 to A3" } } },
                { "Kalimba", 0, "tine", { { "17-key in C", { 86, 83, 79, 76, 72, 69, 65, 62, 60, 64, 67, 71, 74, 77, 81, 84, 88 }, {  }, "Tines left to right as you hold it. Sharp: tap the tine in a little (shorter). Flat: tap it out (longer). Use a small hammer" }, { "10-key in C", { 76, 72, 69, 65, 62, 60, 64, 67, 71, 74 }, {  }, "Tines left to right as you hold it. Sharp: tap the tine in a little (shorter). Flat: tap it out (longer). Use a small hammer" }, { "Any note", {  }, {  }, "Tines left to right as you hold it. Sharp: tap the tine in a little (shorter). Flat: tap it out (longer). Use a small hammer" } } },
            } },
            { "Anything else", {
                { "Chromatic (any instrument)", 0, "string", { { "Any note", {  }, {  }, "Voice, piano, harp, drums, handpan, didgeridoo, bagpipes..." } } },
            } },
        };
        return list;
    }

    inline const std::vector<Temperament>& temperaments()
    {
        static const std::vector<Temperament> list {
            { "Equal temperament", { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f } },
            { "Just intonation (major)", { 0.0f, 11.7f, 3.9f, 15.6f, -13.7f, -2.0f, -9.8f, 2.0f, 13.7f, -15.6f, 17.6f, -11.7f } },
            { "Pythagorean", { 0.0f, 13.7f, 3.9f, -5.9f, 7.8f, -2.0f, 11.7f, 2.0f, 15.6f, 5.9f, -3.9f, 9.8f } },
            { "Quarter-comma meantone", { 0.0f, -24.0f, -6.8f, 10.3f, -13.7f, 3.4f, -20.5f, -3.4f, -27.4f, -10.3f, 6.8f, -17.1f } },
            { "Werckmeister III", { 0.0f, -9.8f, -7.8f, -5.9f, -9.8f, -2.0f, -11.7f, -3.9f, -7.8f, -11.7f, -3.9f, -7.8f } },
            { "Kirnberger III", { 0.0f, -9.8f, -6.8f, -5.9f, -13.7f, -2.0f, -9.8f, -3.4f, -7.8f, -10.3f, -3.9f, -11.7f } },
            { "Vallotti", { 0.0f, -3.9f, -2.0f, 0.0f, -5.9f, 2.0f, -5.9f, -2.0f, -2.0f, -3.9f, 2.0f, -5.9f } }
        };
        return list;
    }

    inline const std::vector<Reference>& references()
    {
        static const std::vector<Reference> list { { "440 Hz (modern)", 440 }, { "442 Hz (European orchestras)", 442 }, { "443 Hz", 443 }, { "441 Hz", 441 }, { "432 Hz", 432 }, { "430 Hz (classical era)", 430 }, { "415 Hz (baroque)", 415 }, { "466 Hz (Chorton)", 466 } };
        return list;
    }
}
