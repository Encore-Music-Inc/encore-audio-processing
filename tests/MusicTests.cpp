// The musical words (dsp/Music.h), the scale library (dsp/ScaleLibrary.h, made
// by tools/scales.py) and pitch correction's lists (dsp/Scales.h), built
// without JUCE as the browser build is. Each check prints PASS or FAIL; the
// suite exits non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "dsp/Music.h"
#include "dsp/ScaleLibrary.h"
#include "dsp/Scales.h"

namespace
{
    int failures = 0;
    using namespace vs::music;

    void expect (const std::string& name, bool ok, const std::string& detail = "")
    {
        std::printf ("%s %-58s %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
        if (! ok) ++failures;
    }

    std::string text (const std::vector<Note>& ns)
    {
        std::string s;
        for (auto& n : ns) s += (s.empty() ? "" : " ") + noteText (n);
        return s;
    }

    Key K (const char* tonic, Mode m = Mode::major) { return *makeKey (tonic, m); }

    std::string notesOf (const Key& k)
    {
        const auto ns = scaleNotes (k);
        return text ({ ns.begin(), ns.end() });
    }

    void noteNameRule()
    {
        const char* rule[12] { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        bool ok = true;
        for (int m = 0; m < 128; ++m)
        {
            const std::string want = std::string (rule[m % 12]) + std::to_string (m / 12 - 1);
            ok = ok && noteName (m) == want && noteName (m, false) == rule[m % 12];
        }
        expect ("names by the rule for MIDI 0 to 127 (60 = C4)", ok);
        expect ("C4, Eb4, A4, Bb-1, C#-1", noteName (60) == "C4" && noteName (63) == "Eb4" && noteName (69) == "A4"
                                            && noteName (10) == "Bb-1" && noteName (1) == "C#-1");
        expect ("no note is \"--\"", noteName (-1) == "--" && noteName (-60, false) == "--");
        bool same = true;
        for (int pc = 0; pc < 12; ++pc) same = same && noteName (pc, false) == vs::scales::keyNameList[(size_t) pc];
        expect ("pitch correction's key names are the rule's", same);
    }

    void spelledNotes()
    {
        auto p = [] (const char* s) { auto n = parseNote (s); return n ? noteText (*n) : std::string ("none"); };
        expect ("parses F#, Bb, Cbb, Gx, G##, lower case and spaces",
                p ("F#") == "F#" && p ("Bb") == "Bb" && p ("Cbb") == "Cbb" && p ("Gx") == "G##" && p ("G##") == "G##"
                && p ("eb") == "Eb" && p ("  a#\t") == "A#" && p ("b") == "B" && p ("bb") == "Bb");
        expect ("refuses H, C###, Cb#, empty, 4", p ("H") == "none" && p ("C###") == "none" && p ("Cb#") == "none"
                                                  && p ("") == "none" && p ("4") == "none" && p ("C 4") == "none");
        expect ("pitch classes: Cb 11, B# 0, Fbb 3, E# 5", pcOf (*parseNote ("Cb")) == 11 && pcOf (*parseNote ("B#")) == 0
                                                         && pcOf (*parseNote ("Fbb")) == 3 && pcOf (*parseNote ("E#")) == 5);
        expect ("spellings of 1: C# Db", text (spellings (1)) == "C# Db", text (spellings (1)));
        expect ("spellings of 0: C B# Dbb is out (one accidental at most)", text (spellings (0)) == "C B#", text (spellings (0)));
        expect ("spellings of 4: E Fb", text (spellings (4)) == "E Fb", text (spellings (4)));
        bool all = true;
        for (int pc = 0; pc < 12; ++pc)
            for (auto n : spellings (pc)) all = all && pcOf (n) == pc && std::abs (n.acc) <= 1;
        expect ("every spelling sounds as its pitch class", all);
        expect ("sounds as: Cb is B, E# is F, Dbb is C, F# is plain", soundsAs (*parseNote ("Cb")) == "B" && soundsAs (*parseNote ("E#")) == "F"
                                                                     && soundsAs (*parseNote ("Dbb")) == "C" && soundsAs (*parseNote ("F#")).empty());
        expect ("octaves: B#3 sounds as C4, Cb4 as B3", midiText (60, *parseNote ("B#")) == "B#3" && midiText (59, *parseNote ("Cb")) == "Cb4");
        expect ("side: 1 asks for C#, -1 for Db, white keys stay", noteText (spellNote (1, nullptr, 1)) == "C#"
                                                                    && noteText (spellNote (1, nullptr, -1)) == "Db" && noteText (spellNote (2, nullptr, -1)) == "D");
    }

    void keys()
    {
        expect ("in F: Bb, not A#", noteText (inKey (10, K ("F"))) == "Bb");
        expect ("in E: D#, G#, not Eb, Ab", noteText (inKey (3, K ("E"))) == "D#" && noteText (inKey (8, K ("E"))) == "G#");
        expect ("in Db: Gb and Cb", noteText (inKey (6, K ("Db"))) == "Gb" && noteText (inKey (11, K ("Db"))) == "Cb");
        expect ("a note named for its key: 66 in D is F#4, in Db Gb4",
                noteName (66, K ("D")) == "F#4" && noteName (66, K ("Db")) == "Gb4" && noteName (71, K ("Gb"), false) == "Cb");
        expect ("C# major's notes", notesOf (K ("C#")) == "C# D# E# F# G# A# B#");
        expect ("Eb minor's notes", notesOf (K ("Eb", Mode::minor))
                                    == "Eb F Gb Ab Bb Cb Db");
        bool sig = true;
        for (size_t i = 0; i < majorKeys.size(); ++i)
        {
            const int want = i < 8 ? (int) i : -(int) (i - 7);
            sig = sig && signature (K (majorKeys[i])) == want && signature (K (minorKeys[i], Mode::minor)) == want;
        }
        expect ("key signatures: C 0 ... C# 7, F -1 ... Cb -7 (and minors)", sig);
        expect ("key names", keyName (K ("F#")) == "F# major" && keyName (K ("Eb", Mode::minor)) == "Eb minor");
        auto pk = [] (const char* s) { auto k = parseKey (s); return k ? keyName (*k) : std::string ("none"); };
        expect ("reads keys: \"F# minor\", \"Bbm\", \"C#/Db major\", \"Ab\", \"Eb min\"",
                pk ("F# minor") == "F# minor" && pk ("Bbm") == "Bb minor" && pk ("C#/Db major") == "C# major" && pk ("Ab") == "Ab major"
                && pk ("Eb min") == "Eb minor" && pk ("F#/Gb minor") == "F# minor");
        expect ("refuses \"H major\", \"C dorian\", \"C#/Q\"", pk ("H major") == "none" && pk ("C dorian") == "none" && pk ("C#/Q") == "none");
    }

    void hzAndMidi()
    {
        expect ("A4 = 440 Hz is MIDI 69", std::abs (hzToMidi (440.0) - 69.0) < 1e-12 && std::abs (midiToHz (69.0) - 440.0) < 1e-12);
        expect ("C4 = 261.6256 Hz", std::abs (midiToHz (60.0) - 261.6255653005986) < 1e-9);
        expect ("with A4 = 432, 432 Hz is 69 and 440 Hz is 69.3176", std::abs (hzToMidi (432.0, 432.0) - 69.0) < 1e-12
                                                                       && std::abs (hzToMidi (440.0, 432.0) - 69.31766653633339) < 1e-9);
        double worst = 0;
        for (double m = -20; m <= 140; m += 0.37) worst = std::max (worst, std::abs (hzToMidi (midiToHz (m, 442.0), 442.0) - m));
        expect ("Hz to MIDI and back", worst < 1e-9, std::to_string (worst));
    }

    void drums()
    {
        expect ("General MIDI names 35 to 81", gmDrumName (35) != nullptr && gmDrumName (81) != nullptr && gmDrumName (34) == nullptr
                                                 && gmDrumName (82) == nullptr && std::strcmp (gmDrumName (42), "Closed hat") == 0
                                                 && std::strcmp (gmDrumName (56), "Cowbell") == 0);
        bool ordered = true;
        for (size_t i = 0; i < gmDrums.size(); ++i) ordered = ordered && gmDrums[i].note == 35 + (int) i;
        expect ("the table is every note 35 to 81 in order", ordered);
        expect ("ten lanes: kick on top, ride at the bottom", drumLanes.size() == 10 && drumLanes.front() == 36 && drumLanes.back() == 51);
        expect ("one name per drum note, its General MIDI name, else \"Note n\"", drumName (45) == "Low tom" && drumName (48) == "High-mid tom"
                                                                          && drumName (37) == "Side stick" && drumName (90) == "Note 90");
        bool pads = drumPads.size() == 9;
        for (size_t i = 0; i < drumPads.size(); ++i) pads = pads && drumPads[i].slot == (int) i + 1;
        expect ("nine pads, slots 1 to 9", pads);
        expect ("notes no pad has play the nearest pad", padForNote (40) == 2 && padForNote (44) == 3 && padForNote (51) == 9
                                                          && padForNote (54) == 5 && padForNote (36) == 0 && padForNote (60) == 0);
    }

    std::vector<int> semis (const char* id)
    {
        const auto* s = vs::scales::find (id);
        return s ? std::vector<int> (s->semis.begin(), s->semis.end()) : std::vector<int> {};
    }

    void scaleLibrary()
    {
        using V = std::vector<int>;
        expect ("major: 0 2 4 5 7 9 11", semis ("ionian") == V { 0, 2, 4, 5, 7, 9, 11 });
        expect ("natural minor: 0 2 3 5 7 8 10", semis ("aeolian") == V { 0, 2, 3, 5, 7, 8, 10 });
        expect ("harmonic minor: 0 2 3 5 7 8 11", semis ("harmonic_minor") == V { 0, 2, 3, 5, 7, 8, 11 });
        expect ("melodic minor: 0 2 3 5 7 9 11", semis ("melodic_minor") == V { 0, 2, 3, 5, 7, 9, 11 });
        expect ("dorian: 0 2 3 5 7 9 10", semis ("dorian") == V { 0, 2, 3, 5, 7, 9, 10 });
        expect ("lydian: 0 2 4 6 7 9 11", semis ("lydian") == V { 0, 2, 4, 6, 7, 9, 11 });
        expect ("altered: 0 1 3 4 6 8 10", semis ("altered") == V { 0, 1, 3, 4, 6, 8, 10 });
        expect ("ultralocrian: 0 1 3 4 6 8 9 (bb7 is a 6th)", semis ("ultralocrian") == V { 0, 1, 3, 4, 6, 8, 9 });
        expect ("major pentatonic: 0 2 4 7 9", semis ("major_pent") == V { 0, 2, 4, 7, 9 });
        expect ("minor pentatonic: 0 3 5 7 10", semis ("minor_pent") == V { 0, 3, 5, 7, 10 });
        expect ("blues: 0 3 5 6 7 10", semis ("blues") == V { 0, 3, 5, 6, 7, 10 });
        expect ("whole tone: 0 2 4 6 8 10", semis ("whole_tone") == V { 0, 2, 4, 6, 8, 10 });
        expect ("diminished half-whole: 0 1 3 4 6 7 9 10", semis ("dim_hw") == V { 0, 1, 3, 4, 6, 7, 9, 10 });
        expect ("diminished whole-half: 0 2 3 5 6 8 9 11", semis ("dim_wh") == V { 0, 2, 3, 5, 6, 8, 9, 11 });
        expect ("bebop dominant: 0 2 4 5 7 9 10 11", semis ("bebop_dominant") == V { 0, 2, 4, 5, 7, 9, 10, 11 });
        expect ("hungarian minor: 0 2 3 6 7 8 11", semis ("hungarian_minor") == V { 0, 2, 3, 6, 7, 8, 11 });
        expect ("double harmonic: 0 1 4 5 7 8 11", semis ("double_harmonic") == V { 0, 1, 4, 5, 7, 8, 11 });
        expect ("hirajoshi: 0 2 3 7 8", semis ("hirajoshi") == V { 0, 2, 3, 7, 8 });
        V all (12);
        for (int i = 0; i < 12; ++i) all[(size_t) i] = i;
        expect ("chromatic: all twelve", semis ("chromatic") == all);

        bool shape = true, unique = true;
        std::set<std::string> ids;
        for (auto& s : vs::scales::library)
        {
            unique = unique && ids.insert (s.id).second;
            shape = shape && s.semis.size() == s.tones.size() && s.semis[0] == 0 && *s.name && *s.mood && *s.genres && *s.why;
            for (size_t i = 0; i < s.tones.size(); ++i)
                shape = shape && s.semis[i] == mod12 (s.tones[i].semis) && (i == 0 || s.semis[i] > s.semis[i - 1]);
            if (s.nearId) shape = shape && vs::scales::find (s.nearId) != nullptr;
        }
        expect ("every scale: rising semitones from 0, its words, a known relative", shape);
        expect ("ids are unique", unique, std::to_string (vs::scales::library.size()) + " scales");

        bool modes = true;
        for (auto& f : vs::scales::families)
        {
            const auto first = semis (f.modes[0]);
            for (size_t k = 0; k < 7; ++k)
            {
                V want;
                for (int x : first) want.push_back (mod12 (x - first[k]));
                std::sort (want.begin(), want.end());
                modes = modes && semis (f.modes[k]) == want;
            }
        }
        expect ("each family's modes are its first scale from each degree", modes);
        bool bright = true;
        for (size_t i = 1; i < vs::scales::brightness.size(); ++i)
        {
            int down = 0;
            const auto a = semis (vs::scales::brightness[i - 1]), b = semis (vs::scales::brightness[i]);
            for (size_t k = 0; k < 7; ++k) down += a[k] - b[k];
            bright = bright && down == 1;
        }
        expect ("brightness: each mode one lowered note darker", bright);
    }

    void pitchCorrection()
    {
        using V = std::vector<int>;
        auto steps = [] (int i) { const auto s = vs::scales::get (i); return V (s.steps, s.steps + s.size); };
        expect ("its five scales as before", steps (0) == V { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } && steps (1) == V { 0, 2, 4, 5, 7, 9, 11 }
                                             && steps (2) == V { 0, 2, 3, 5, 7, 8, 10 } && steps (3) == V { 0, 2, 4, 7, 9 }
                                             && steps (4) == V { 0, 3, 5, 7, 10 } && steps (-1) == steps (0) && steps (9) == steps (0));
        expect ("names", vs::scales::scaleNames.size() == 5 && vs::scales::intervalNames.size() == 12
                         && std::strcmp (vs::scales::keyNames[3], "Eb") == 0);
        expect ("a 3rd up in C major: E over C, F over D", vs::scales::harmonyNote (60, 7, 0, 1) == 64 && vs::scales::harmonyNote (62, 7, 0, 1) == 65);
        expect ("nearest in A minor pentatonic: 61.2 goes to 62", vs::scales::nearest (61.2f, 9, vs::scales::get (4)) == 62);
    }
}

int main()
{
    noteNameRule();
    spelledNotes();
    keys();
    hzAndMidi();
    drums();
    scaleLibrary();
    pitchCorrection();
    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    return failures == 0 ? 0 : 1;
}
