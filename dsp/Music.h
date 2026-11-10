// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// The musical words every ENCORE. product shares: note names, spelled notes,
// keys, Hz and MIDI, and the names of General MIDI drum notes. Plain C++ with
// no JUCE, so the desktop products and the browser (master.wasm) name things
// the same way.
//
// The note-name rule: a note is spelled for the key when there is one;
// otherwise it is one of C, C#, D, Eb, E, F, F#, G, Ab, A, Bb, B. MIDI 60 is
// C4, and "--" stands for no note.
#pragma once

#include <array>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vs::music
{
    constexpr int mod12 (int n) { return ((n % 12) + 12) % 12; }
    constexpr int mod7 (int n)  { return ((n % 7) + 7) % 7; }
    // Rounds towards minus infinity (-1 / 12 is -1, not 0)
    constexpr int floorDiv (int a, int b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0); }

    inline constexpr std::array<const char*, 7> letters { "C", "D", "E", "F", "G", "A", "B" };
    inline constexpr std::array<int, 7> letterPc { 0, 2, 4, 5, 7, 9, 11 };
    inline constexpr std::array<int, 7> majorSemis { 0, 2, 4, 5, 7, 9, 11 };   // the major scale's degrees 1..7

    // ---- spelled notes: a letter and an accidental, so F# and Gb stay different notes

    // letter: 0..6 (C..B). acc: -2 double flat, -1 flat, 0 natural, 1 sharp, 2 double sharp
    struct Note
    {
        int letter = 0;
        int acc = 0;
        constexpr bool operator== (const Note&) const = default;
    };

    // A spelled note from a letter (wrapped to 0..6) and an accidental
    constexpr Note note (int letter, int acc = 0) { return { mod7 (letter), acc }; }
    constexpr int pcOf (Note n) { return mod12 (letterPc[(size_t) n.letter] + n.acc); }

    // "C", "F#", "Bb", "Ebb"
    inline std::string noteText (Note n)
    {
        std::string s = letters[(size_t) n.letter];
        s.append ((size_t) std::abs (n.acc), n.acc > 0 ? '#' : 'b');
        return s;
    }

    // A note name such as "F#", "Bb", "cbb" or "Gx" (x is a double sharp), with
    // spaces around it allowed; nothing when it isn't one
    inline std::optional<Note> parseNote (std::string_view s)
    {
        auto space = [] (char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; };
        while (! s.empty() && space (s.front())) s.remove_prefix (1);
        while (! s.empty() && space (s.back())) s.remove_suffix (1);
        if (s.empty()) return {};
        const char c = (char) (s[0] >= 'a' && s[0] <= 'z' ? s[0] - 32 : s[0]);
        int letter = -1;
        for (int i = 0; i < 7; ++i) if (letters[(size_t) i][0] == c) letter = i;
        if (letter < 0) return {};
        const auto rest = s.substr (1);
        int acc = 0;
        if (rest.empty()) acc = 0;
        else if (rest == "##" || rest == "x") acc = 2;
        else if (rest == "#") acc = 1;
        else if (rest == "b") acc = -1;
        else if (rest == "bb") acc = -2;
        else return {};
        return note (letter, acc);
    }

    // The note on that letter that sounds as pitch class pc (accidental -5..6)
    constexpr Note onLetter (int letter, int pc)
    {
        int a = mod12 (pc - letterPc[(size_t) mod7 (letter)]);
        if (a > 6) a -= 12;
        return note (letter, a);
    }

    // Every spelling of a pitch class with at most one sharp or flat, naturals first
    inline std::vector<Note> spellings (int pc)
    {
        std::vector<Note> out;
        for (int a = 0; a <= 1; ++a)
            for (int l = 0; l < 7; ++l)
                if (const Note n = onLetter (l, pc); std::abs (n.acc) == a) out.push_back (n);
        return out;
    }

    // The rule's name for a pitch class when there's no key: C, C#, D, Eb, E, F, F#, G, Ab, A, Bb, B
    constexpr Note plainNote (int pc)
    {
        constexpr std::array<Note, 12> plain { { { 0, 0 }, { 0, 1 }, { 1, 0 }, { 2, -1 }, { 2, 0 }, { 3, 0 },
                                                  { 3, 1 }, { 4, 0 }, { 5, -1 }, { 5, 0 }, { 6, -1 }, { 6, 0 } } };
        return plain[(size_t) mod12 (pc)];
    }

    // The black key's sharp (side 1) or flat (side -1) name
    inline Note sideSpelling (int pc, int side)
    {
        for (auto n : spellings (pc)) if (n.acc == side) return n;
        return plainNote (pc);
    }

    // For notes like Cb, Fb, E#, B# or any double accidental: the plain note it sounds as ("" otherwise)
    inline std::string soundsAs (Note n)
    {
        const bool odd = std::abs (n.acc) > 1 || (n.acc == 1 && (n.letter == 2 || n.letter == 6)) || (n.acc == -1 && (n.letter == 0 || n.letter == 3));
        return odd ? noteText (plainNote (pcOf (n))) : std::string();
    }

    // A MIDI note as text with its octave, 60 = C4: spelled as given, or by the
    // rule. B# is written in the octave below the one it sounds in, Cb in the one above.
    inline std::string midiText (int midi, std::optional<Note> spelled = {})
    {
        const Note n = spelled ? *spelled : plainNote (midi);
        return noteText (n) + std::to_string (floorDiv (midi - n.acc, 12) - 1);
    }

    // ---- keys

    enum class Mode { major, minor };
    struct Key
    {
        Note tonic;
        Mode mode = Mode::major;
        constexpr bool operator== (const Key&) const = default;
    };

    // Every key with at most seven sharps or flats, by the name of its tonic
    inline constexpr std::array<const char*, 15> majorKeys { "C", "G", "D", "A", "E", "B", "F#", "C#", "F", "Bb", "Eb", "Ab", "Db", "Gb", "Cb" };
    inline constexpr std::array<const char*, 15> minorKeys { "A", "E", "B", "F#", "C#", "G#", "D#", "A#", "D", "G", "C", "F", "Bb", "Eb", "Ab" };

    // A pitch class spelled in a key: each of the twelve gets the key's
    // degree (and alteration of it) that a chromatic note usually takes
    inline Note inKey (int pc, Note tonic)
    {
        constexpr std::array<int, 12> step { 0, 1, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6 };
        return onLetter (tonic.letter + step[(size_t) mod12 (pc - pcOf (tonic))], pc);
    }
    inline Note inKey (int pc, const Key& key) { return inKey (pc, key.tonic); }

    // A pitch class spelled by the rule: a black key on the side asked for
    // (side 1 sharp, -1 flat), otherwise in the key when there is one, otherwise plainly
    inline Note spellNote (int pc, const Key* key = nullptr, int side = 0)
    {
        pc = mod12 (pc);
        if (side != 0 && plainNote (pc).acc != 0) return sideSpelling (pc, side);
        return key ? inKey (pc, *key) : plainNote (pc);
    }

    // A MIDI note's name by the rule, "E4" (or "E" without the octave); "--" for no note (negative)
    inline std::string noteName (int midi, bool withOctave = true)
    {
        if (midi < 0) return "--";
        return withOctave ? midiText (midi) : noteText (plainNote (midi));
    }
    // The same, spelled for a key
    inline std::string noteName (int midi, const Key& key, bool withOctave = true)
    {
        if (midi < 0) return "--";
        const Note n = inKey (midi, key);
        return withOctave ? midiText (midi, n) : noteText (n);
    }

    inline std::optional<Key> makeKey (std::string_view tonic, Mode mode = Mode::major)
    {
        if (auto n = parseNote (tonic)) return Key { *n, mode };
        return {};
    }

    // "F# major", "Eb minor"
    inline std::string keyName (const Key& k) { return noteText (k.tonic) + (k.mode == Mode::minor ? " minor" : " major"); }

    // A key written as text: a tonic ("F#", "Bb", or a pair such as "C#/Db",
    // which takes the first) and then "major", "minor", "maj", "min" or "m"
    // (nothing means major): "F# minor", "Bbm", "C#/Db major"
    inline std::optional<Key> parseKey (std::string_view s)
    {
        auto lower = [] (std::string_view v) { std::string o (v); for (auto& c : o) if (c >= 'A' && c <= 'Z') c = (char) (c + 32); return o; };
        while (! s.empty() && s.front() == ' ') s.remove_prefix (1);
        while (! s.empty() && s.back() == ' ') s.remove_suffix (1);
        size_t i = 1;
        while (i < s.size() && (s[i] == '#' || s[i] == 'b' || s[i] == 'x')) ++i;
        auto tonic = s.substr (0, std::min (i, s.size()));
        auto rest = s.substr (std::min (i, s.size()));
        if (! rest.empty() && rest.front() == '/')
        {
            size_t j = 1;
            while (j < rest.size() && rest[j] != ' ' && rest[j] != 'm' && rest[j] != 'M') ++j;
            auto other = parseNote (rest.substr (1, j - 1));
            if (! other) return {};
            rest = rest.substr (j);
        }
        while (! rest.empty() && rest.front() == ' ') rest.remove_prefix (1);
        const auto m = lower (rest);
        Mode mode;
        if (m.empty() || m == "major" || m == "maj") mode = Mode::major;
        else if (m == "minor" || m == "min" || rest == "m") mode = Mode::minor;
        else return {};
        return makeKey (tonic, mode);
    }

    // The key's seven notes, spelled (natural minor for a minor key)
    inline std::array<Note, 7> scaleNotes (const Key& key)
    {
        constexpr std::array<int, 7> minorAlt { 0, 0, -1, 0, 0, -1, -1 };
        std::array<Note, 7> out {};
        for (int s = 0; s < 7; ++s)
        {
            const int alt = key.mode == Mode::minor ? minorAlt[(size_t) s] : 0;
            out[(size_t) s] = onLetter (key.tonic.letter + s, pcOf (key.tonic) + majorSemis[(size_t) s] + alt);
        }
        return out;
    }

    // How many sharps (positive) or flats (negative) the key signature has
    inline int signature (const Key& key)
    {
        int s = 0;
        for (auto n : scaleNotes (key)) s += n.acc;
        return s;
    }

    // ---- Hz and MIDI, with any reference pitch for A4

    // A frequency as a MIDI note with a fraction (69.5 is a quarter tone above A4)
    inline double hzToMidi (double hz, double a4 = 440.0) { return 69.0 + 12.0 * std::log2 (hz / a4); }
    inline double midiToHz (double midi, double a4 = 440.0) { return a4 * std::pow (2.0, (midi - 69.0) / 12.0); }
    // The same in single precision, as the pitch engine works (both arguments float)
    inline float hzToMidi (float hz, float a4) { return 69.0f + 12.0f * std::log2 (hz / a4); }

    // ---- General MIDI drums

    struct DrumNote { int note; const char* name; };

    // The General MIDI percussion notes (35 to 81)
    inline constexpr std::array<DrumNote, 47> gmDrums { {
        { 35, "Acoustic kick" }, { 36, "Kick" }, { 37, "Side stick" }, { 38, "Snare" }, { 39, "Clap" }, { 40, "Electric snare" },
        { 41, "Low floor tom" }, { 42, "Closed hat" }, { 43, "High floor tom" }, { 44, "Pedal hat" }, { 45, "Low tom" },
        { 46, "Open hat" }, { 47, "Low-mid tom" }, { 48, "High-mid tom" }, { 49, "Crash" }, { 50, "High tom" }, { 51, "Ride" },
        { 52, "China" }, { 53, "Ride bell" }, { 54, "Tambourine" }, { 55, "Splash" }, { 56, "Cowbell" }, { 57, "Crash 2" },
        { 58, "Vibraslap" }, { 59, "Ride 2" }, { 60, "High bongo" }, { 61, "Low bongo" }, { 62, "Mute high conga" },
        { 63, "Open high conga" }, { 64, "Low conga" }, { 65, "High timbale" }, { 66, "Low timbale" }, { 67, "High agogo" },
        { 68, "Low agogo" }, { 69, "Cabasa" }, { 70, "Maracas" }, { 71, "Short whistle" }, { 72, "Long whistle" },
        { 73, "Short guiro" }, { 74, "Long guiro" }, { 75, "Claves" }, { 76, "High wood block" }, { 77, "Low wood block" },
        { 78, "Mute cuica" }, { 79, "Open cuica" }, { 80, "Mute triangle" }, { 81, "Open triangle" },
    } };

    // The General MIDI name of a drum note, or nullptr
    constexpr const char* gmDrumName (int note)
    {
        return note >= 35 && note <= 81 ? gmDrums[(size_t) (note - 35)].name : nullptr;
    }

    // Every drum note has one name: its General MIDI name, the same in every
    // product, in every layout and on every pad. Outside 35 to 81: "Note <n>".
    inline std::string drumName (int note)
    {
        if (auto* n = gmDrumName (note)) return n;
        return "Note " + std::to_string (note);
    }

    // A drum kit's rows, top to bottom, as a drum grid shows them (ten lanes)
    inline constexpr std::array<int, 10> drumLanes { 36, 38, 39, 42, 46, 41, 45, 48, 49, 51 };

    // A pad kit's nine pads, slot 1 to 9: 1-2-3 the bottom row, 4-5-6 the
    // middle, 7-8-9 the top, the way a numpad is laid out
    // (each pad is named by its note, drumName)
    struct Pad { int slot; int note; };
    inline constexpr std::array<Pad, 9> drumPads { {
        { 1, 36 }, { 2, 38 }, { 3, 42 }, { 4, 39 }, { 5, 37 }, { 6, 46 }, { 7, 48 }, { 8, 45 }, { 9, 49 },
    } };

    // The pad slot (1..9) a General MIDI note no pad has plays instead, so a
    // drum controller's "electric snare" or "pedal hat" still sounds; 0 for none
    constexpr int padForNote (int note)
    {
        constexpr std::array<std::array<int, 2>, 15> alias { { { 35, 1 }, { 40, 2 }, { 44, 3 }, { 41, 8 }, { 43, 8 }, { 47, 8 }, { 50, 7 },
                                                              { 52, 9 }, { 55, 9 }, { 57, 9 }, { 51, 9 }, { 53, 9 }, { 59, 9 }, { 54, 5 }, { 56, 5 } } };
        for (auto& a : alias) if (a[0] == note) return a[1];
        return 0;
    }
}
