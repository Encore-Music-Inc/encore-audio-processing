// The browser's musical words (mu_*): plain C functions around dsp/Music.h,
// so the web names notes, spells them in keys and converts Hz and MIDI the
// same way the desktop products do. Built with the other wasm/api_*.cpp files
// into master.wasm by wasm/build.sh. The scale library and other lists are
// data files (data/*.json), not functions.
//
// A spelled note crosses as two ints: its letter (0..6, C..B) and accidental
// (-2..2). A key crosses as its tonic's letter and accidental and minor (0 or
// 1). Text is written to out (memory from malloc, cap bytes) and ends with a
// 0 byte; the functions that write text return its length, or -1 when it
// doesn't fit.
#include <cstring>
#include <string>
#include <emscripten/emscripten.h>
#include "dsp/Music.h"

namespace
{
    int put (const std::string& s, char* out, int cap)
    {
        if ((int) s.size() + 1 > cap) { if (cap > 0) out[0] = 0; return -1; }
        std::memcpy (out, s.c_str(), s.size() + 1);
        return (int) s.size();
    }

    vs::music::Key key (int letter, int acc, int minor)
    {
        return { vs::music::note (letter, acc), minor ? vs::music::Mode::minor : vs::music::Mode::major };
    }

    void put (vs::music::Note n, int* out) { out[0] = n.letter; out[1] = n.acc; }
}

extern "C"
{
    // A MIDI note's name by the rule ("Eb4"; withOctave 0: "Eb"), "--" for a negative note
    EMSCRIPTEN_KEEPALIVE int mu_note_name (int midi, int withOctave, char* out, int cap)
    {
        return put (vs::music::noteName (midi, withOctave != 0), out, cap);
    }

    // The same, spelled for a key
    EMSCRIPTEN_KEEPALIVE int mu_note_name_in_key (int midi, int withOctave, int keyLetter, int keyAcc, int minor, char* out, int cap)
    {
        return put (vs::music::noteName (midi, key (keyLetter, keyAcc, minor), withOctave != 0), out, cap);
    }

    // A spelled note's text ("F#", "Bbb")
    EMSCRIPTEN_KEEPALIVE int mu_note_text (int letter, int acc, char* out, int cap)
    {
        return put (vs::music::noteText (vs::music::note (letter, acc)), out, cap);
    }

    // A MIDI note as text with its octave, spelled as given (B#3 sounds as C4)
    EMSCRIPTEN_KEEPALIVE int mu_midi_text (int midi, int letter, int acc, char* out, int cap)
    {
        return put (vs::music::midiText (midi, vs::music::note (letter, acc)), out, cap);
    }

    // Reads a note name ("F#", "Bb", "Cbb", "Gx") from text (a 0-terminated
    // string from malloc) into out[0..1]; 1 if it is one, else 0
    EMSCRIPTEN_KEEPALIVE int mu_parse_note (const char* text, int* out)
    {
        const auto n = vs::music::parseNote (text);
        if (! n) return 0;
        put (*n, out);
        return 1;
    }

    // Reads a key ("F# minor", "Bbm", "C#/Db major") into out[0..2]: letter, accidental, minor; 1 if it is one
    EMSCRIPTEN_KEEPALIVE int mu_parse_key (const char* text, int* out)
    {
        const auto k = vs::music::parseKey (text);
        if (! k) return 0;
        put (k->tonic, out);
        out[2] = k->mode == vs::music::Mode::minor ? 1 : 0;
        return 1;
    }

    EMSCRIPTEN_KEEPALIVE int mu_pc_of (int letter, int acc) { return vs::music::pcOf (vs::music::note (letter, acc)); }

    // The rule's spelling of a pitch class with no key, into out[0..1]
    EMSCRIPTEN_KEEPALIVE void mu_plain_note (int pc, int* out) { put (vs::music::plainNote (pc), out); }

    // Every spelling of a pitch class with at most one sharp or flat, naturals
    // first, into out (2 ints each, room for 3); returns how many
    EMSCRIPTEN_KEEPALIVE int mu_spellings (int pc, int* out)
    {
        const auto all = vs::music::spellings (pc);
        for (size_t i = 0; i < all.size(); ++i) put (all[i], out + 2 * i);
        return (int) all.size();
    }

    // A pitch class spelled in the key whose tonic is given, into out[0..1]
    EMSCRIPTEN_KEEPALIVE void mu_in_key (int pc, int tonicLetter, int tonicAcc, int* out)
    {
        put (vs::music::inKey (pc, vs::music::note (tonicLetter, tonicAcc)), out);
    }

    // A pitch class spelled by the rule: side 1 or -1 asks for a black key's
    // sharp or flat name; otherwise in the key (hasKey 1) or plainly
    EMSCRIPTEN_KEEPALIVE void mu_spell_note (int pc, int hasKey, int tonicLetter, int tonicAcc, int side, int* out)
    {
        const auto k = key (tonicLetter, tonicAcc, 0);
        put (vs::music::spellNote (pc, hasKey ? &k : nullptr, side), out);
    }

    // "F# major", "Eb minor"
    EMSCRIPTEN_KEEPALIVE int mu_key_name (int tonicLetter, int tonicAcc, int minor, char* out, int cap)
    {
        return put (vs::music::keyName (key (tonicLetter, tonicAcc, minor)), out, cap);
    }

    // The key's seven notes (natural minor for a minor key) into out (14 ints)
    EMSCRIPTEN_KEEPALIVE void mu_scale_notes (int tonicLetter, int tonicAcc, int minor, int* out)
    {
        const auto notes = vs::music::scaleNotes (key (tonicLetter, tonicAcc, minor));
        for (size_t i = 0; i < notes.size(); ++i) put (notes[i], out + 2 * i);
    }

    // Sharps (positive) or flats (negative) in the key signature
    EMSCRIPTEN_KEEPALIVE int mu_signature (int tonicLetter, int tonicAcc, int minor)
    {
        return vs::music::signature (key (tonicLetter, tonicAcc, minor));
    }

    EMSCRIPTEN_KEEPALIVE double mu_hz_to_midi (double hz, double a4) { return vs::music::hzToMidi (hz, a4); }
    EMSCRIPTEN_KEEPALIVE double mu_midi_to_hz (double midi, double a4) { return vs::music::midiToHz (midi, a4); }

    // A drum note's General MIDI name ("" when it has none)
    EMSCRIPTEN_KEEPALIVE int mu_gm_drum_name (int note, char* out, int cap)
    {
        const char* n = vs::music::gmDrumName (note);
        return put (n ? n : "", out, cap);
    }

    // The nine-pad kit's slot (1..9) that plays a General MIDI note no pad has, or 0
    EMSCRIPTEN_KEEPALIVE int mu_pad_for_note (int note) { return vs::music::padForNote (note); }
}
