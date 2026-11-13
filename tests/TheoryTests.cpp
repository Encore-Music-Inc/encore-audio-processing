// The chord and scale theory (dsp/Harmony.h, dsp/Voicings.h, dsp/ScaleTheory.h,
// the chord types made by tools/chords.py) and the browser's requests to it
// (dsp/TheoryCalls.h), built without JUCE as the browser build is. Each check
// prints PASS or FAIL; the suite exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "dsp/TheoryCalls.h"

namespace
{
    int failures = 0;
    using namespace vs;
    using music::Note;
    using music::Key;

    void expect (const std::string& name, bool ok, const std::string& detail = "")
    {
        std::printf ("%s %-58s %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
        if (! ok) ++failures;
    }

    Note N (const char* s) { return *music::parseNote (s); }
    Key K (const char* s) { return *music::parseKey (s); }
    harmony::Chord C (const char* s) { return *harmony::parseChord (s); }
    std::string sym (const harmony::Chord& c) { return harmony::chordSymbol (c); }

    std::string text (const std::vector<Note>& ns)
    {
        std::string s;
        for (auto n : ns) s += (s.empty() ? "" : " ") + music::noteText (n);
        return s;
    }

    std::string frets (const std::vector<int>& f) { return voicings::shapeText (f); }

    std::vector<voicings::Course> guitar() { return voicings::coursesOf ({ 40, 45, 50, 55, 59, 64 }); }

    void chordTypes()
    {
        expect ("the chord type list", chords::types.size() >= 50 && chords::find ("m7b5") != nullptr && std::string (chords::find ("m7b5")->num) == "\303\2707");
        bool unique = true;
        for (size_t i = 0; i < chords::types.size(); ++i)
            for (size_t j = i + 1; j < chords::types.size(); ++j)
                if (std::string (chords::types[i].id) == chords::types[j].id) unique = false;
        expect ("every chord type id once", unique);
    }

    void chordNames()
    {
        expect ("symbols round trip: F#m7b5/C, Bbmaj9#11, C7#9", sym (C ("F#m7b5/C")) == "F#m7b5/C" && sym (C ("Bbmaj9#11")) == "Bbmaj9#11" && sym (C ("C7#9")) == "C7#9");
        expect ("the other ways of writing one: C\302\2607, C-7, C\316\224", sym (C ("C\302\2607")) == "Cdim7" && sym (C ("C-7")) == "Cm7" && sym (C ("C\316\224")) == "Cmaj7");
        expect ("not a chord: H, empty", ! harmony::parseChord ("H") && ! harmony::parseChord (""));
        const auto c7 = harmony::nameNotes ({ 48, 52, 55, 58 });
        expect ("C E G Bb is C7 first", ! c7.empty() && c7[0].symbol == "C7", c7.empty() ? "" : c7[0].symbol);
        const auto inv = harmony::nameNotes ({ 52, 55, 60 });
        expect ("E G C is C/E, its 3rd in the bass", ! inv.empty() && inv[0].symbol == "C/E" && inv[0].bassDegree == "3", inv.empty() ? "" : inv[0].symbol);
        const auto fifth = harmony::nameNotes ({ 48, 55 });
        expect ("C and G is the power chord C5", ! fifth.empty() && fifth[0].symbol == "C5");
        const auto third = harmony::nameNotes ({ 48, 52 });
        expect ("C and E is an interval, standing for C major",
                ! third.empty() && third[0].kind == harmony::NoteSetName::Kind::interval && third[0].semis == 4 && third[0].hintType == "maj");
    }

    void numerals()
    {
        expect ("V7/V in C is D7", sym (*harmony::numeralChord (K ("C major"), "V7/V")) == "D7");
        expect ("ii\303\2707 in A minor is Bm7b5", sym (*harmony::numeralChord (K ("A minor"), "ii\303\2707")) == "Bm7b5");
        expect ("I/3 in G is G/B", sym (*harmony::numeralChord (K ("G major"), "I/3")) == "G/B");
        expect ("not a numeral: X", ! harmony::numeralChord (K ("C major"), "X"));
        expect ("F#m7b5 in C is #iv\303\2707", harmony::plainNumeral (K ("C major"), C ("F#m7b5")) == "#iv\303\2707");
        const auto n = harmony::numeralOf (K ("C major"), C ("G7/B"));
        expect ("G7/B in C is V7 with its 3rd in the bass", n.label == "V7" && n.inversion.slash == "/3");
    }

    void keyMap()
    {
        const auto p = harmony::keyPalette (K ("C major"));
        expect ("the map of C major: 45 chords in 6 groups", p.all.size() == 45 && p.groups.size() == 6, std::to_string (p.all.size()));
        expect ("its relative key is A minor", music::keyName (p.relative) == "A minor" && sym (p.relativeHome) == "Am");
        const auto moves = harmony::nextMoves (K ("C major"), C ("G7"), 3);
        expect ("after G7 in C, C comes first, as a cadence home",
                ! moves.empty() && sym (moves[0].entry.chord) == "C" && moves[0].relation.kind == harmony::MoveKind::resolve
                && moves[0].relation.facts.reason == harmony::MoveReason::cadence && moves[0].relation.facts.seventhSettles);
        const auto r = harmony::relate (K ("C major"), C ("G7"), C ("Am"));
        expect ("G7 to Am is a deceptive cadence", r.kind == harmony::MoveKind::surprise && r.facts.reason == harmony::MoveReason::deceptive);
        const auto mods = harmony::modulations (K ("C major"));
        expect ("from C to G: a pivot, then D7, then G", ! mods.empty() && music::keyName (mods[0].key) == "G major"
                && mods[0].route.size() >= 2 && sym (mods[0].route[mods[0].route.size() - 2]) == "D7" && sym (mods[0].route.back()) == "G");
        expect ("a tritone up from C is F#", music::keyName (harmony::keyBySemis (K ("C major"), 6)) == "F# major");
        expect ("D7 a 2nd up is E7", sym (harmony::transposeChord (C ("D7"), harmony::intervalBetween (N ("C"), N ("D")))) == "E7");
    }

    void fretsAndKeys()
    {
        const auto g = guitar();
        const auto c = voicings::fretVoicings (C ("C"), g, { .limit = 1 });
        expect ("C on guitar: x32010 first", ! c.empty() && frets (c[0].frets) == "x32010", c.empty() ? "" : frets (c[0].frets));
        const auto e = voicings::fretVoicings (C ("E"), g, { .limit = 1 });
        expect ("E on guitar: 022100 first", ! e.empty() && frets (e[0].frets) == "022100", e.empty() ? "" : frets (e[0].frets));
        const auto f = voicings::fingering ({ 1, 3, 3, 2, 1, 1 });
        expect ("F's full shape: four fingers, a barre at fret 1", f.fingers == 4 && f.barre && f.barre->fret == 1);
        const auto banjo = voicings::coursesOf ({ 67, 50, 55, 59, 62 }, {}, "Banjo (5-string)");
        expect ("a banjo's short 5th string starts at fret 5", banjo.size() == 5 && banjo[0].start == 5);
        const auto p = voicings::pianoVoicing (C ("Cmaj7"));
        expect ("Cmaj7 on piano: C in the bass, C E G B above", p.bass == 48 && p.rh == std::vector<int> { 60, 64, 67, 71 });
        expect ("not fretted: an erhu", ! voicings::frettable ("Erhu", "string", { { 62, 69 } }));
        const auto capo = voicings::capoOptions ({ C ("Eb"), C ("Ab"), C ("Bb") }, g, 7);
        expect ("Eb Ab Bb: a capo at 3 plays C F G shapes", ! capo.empty() && capo[0].capo == 3 && sym (capo[0].shapes[0]) == "C",
                capo.empty() ? "" : std::to_string (capo[0].capo));
    }

    const scales::LibraryScale& scale (const char* id) { return scaletheory::scaleAt (scaletheory::scaleIndex (id)); }

    std::string notesOf (const char* id, const char* root)
    {
        std::vector<Note> ns;
        for (const auto& x : scaletheory::spellScale (scale (id), N (root))) ns.push_back (x.note);
        return text (ns);
    }

    void scaleSpelling()
    {
        expect ("G# harmonic minor has an F##", notesOf ("harmonic_minor", "G#") == "G# A# B C# D# E F##", notesOf ("harmonic_minor", "G#"));
        expect ("Eb blues has an A, not a Bbb", notesOf ("blues", "Eb") == "Eb Gb Ab A Bb Db", notesOf ("blues", "Eb"));
        expect ("Db whole tone ends on B, not Cb", notesOf ("whole_tone", "Db") == "Db Eb F G A B", notesOf ("whole_tone", "Db"));
        const auto simpler = scaletheory::simplerRoot (scale ("ionian"), N ("D#"));
        expect ("D# major reads more easily as Eb major", simpler && music::noteText (*simpler) == "Eb");
        const auto steps = scaletheory::stepText (scale ("ionian"));
        expect ("the major scale's steps: W W H W W W H", steps == std::vector<std::string> { "W", "W", "H", "W", "W", "W", "H" });
        const auto cmp = scaletheory::compareScales (scale ("dorian"), scale ("aeolian"));
        expect ("Dorian is natural minor with a 6 for the b6", cmp.swaps.size() == 1 && cmp.swaps[0].first == "6" && cmp.swaps[0].second == "b6");
    }

    void modes()
    {
        const auto p = scaletheory::parentOf (scale ("dorian"), N ("D"));
        expect ("D Dorian's parent is C major, mode 2", p && std::string (scales::library[(size_t) p->scale].id) == "ionian" && music::noteText (p->root) == "C" && p->mode == 2);
        const auto m = scaletheory::familyModes (scale ("dorian"), N ("D"));
        expect ("its family: seven modes, Dorian marked", m && m->size() == 7 && (*m)[1].current);
        const auto chords = scaletheory::scaleChords (scale ("ionian"), N ("C"));
        expect ("C major's chords: C, Dm ... Bdim; 7ths Cmaj7 ... Bm7b5", chords.size() == 7 && chords[1].triad.symbol == "Dm" && chords[6].triad.symbol == "Bdim"
                && chords[0].seventh.symbol == "Cmaj7" && chords[6].seventh.symbol == "Bm7b5" && chords[6].seventh.numeral == "vii\303\2707");
        expect ("C6 is the type for 0 4 7 9", scaletheory::typeForSemis ({ 0, 4, 7, 9 }) == harmony::typeIndex ("6"));
        expect ("an interval as a degree: Eb over C is b3", scaletheory::intervalDeg (N ("C"), N ("Eb")) == "b3");
    }

    void scaleFits()
    {
        const auto g7 = scaletheory::scalesForChord (*scaletheory::readChord ("G7"), 1);
        expect ("over G7: Mixolydian first", ! g7.empty() && std::string (scales::library[(size_t) g7[0].scale].id) == "mixolydian");
        const auto alt = scaletheory::scalesForChord (*scaletheory::readChord ("C7alt"), 1);
        expect ("over C7alt: the altered scale first", ! alt.empty() && std::string (scales::library[(size_t) alt[0].scale].id) == "altered");
        const auto fit = scaletheory::fitOver (scale ("ionian"), N ("C"), *scaletheory::readChord ("Cmaj7"));
        expect ("C major over Cmaj7: F is the avoid note", fit.avoid.size() == 1 && music::noteText (fit.avoid[0].note) == "F");
        const auto prog = scaletheory::readProgression ("Am7, D7 | Gmaj7 Xyz");
        expect ("reads Am7, D7 | Gmaj7 and leaves out Xyz", prog.chords.size() == 3 && prog.bad == std::vector<std::string> { "Xyz" });
        const auto ii = scaletheory::scalesForProgression (prog.chords);
        expect ("Am7 D7 Gmaj7: G major, home on G", ! ii.overall.empty() && std::string (scales::library[(size_t) ii.overall[0].scale].id) == "ionian"
                && music::noteText (ii.overall[0].root) == "G" && ii.tonic == 7 && ii.perChord.size() == 3);
        const auto blues = scaletheory::scalesForProgression (scaletheory::readProgression ("C7 F7 C7 G7").chords);
        expect ("C7 F7 G7: a blues, minor pentatonic first", blues.blues && std::string (scales::library[(size_t) blues.overall[0].scale].id) == "minor_pent");
    }

    void neck()
    {
        const auto g = guitar();
        expect ("standard tuning is standard; drop D isn't", scaletheory::isStandardSix (g) && ! scaletheory::isStandardSix (voicings::coursesOf ({ 38, 45, 50, 55, 59, 64 })));
        const auto boxes = scaletheory::boxPositions (scale ("minor_pent"), N ("A"), g, 0, 15, true);
        bool fifth = false;
        for (const auto& b : boxes) if (b.lo == 5 && b.shape == "G") fifth = true;
        expect ("A minor pentatonic: the box at fret 5 is the G shape", fifth);
        const auto three = scaletheory::perStringPatterns (scale ("ionian"), N ("G"), g);
        bool allThree = ! three.empty();
        for (const auto& p : three) if (p.notes.size() != 18) allThree = false;
        expect ("G major: three notes on every string", allThree && three[0].perString == 3);
        const auto run = scaletheory::upAndDown (scale ("ionian"), N ("C"), nullptr, 52);
        expect ("C major up and down from C3: 15 notes, 48 to 60 and back", run.size() == 15 && run[0].midi == 48 && run[7].midi == 60 && run.back().midi == 48);
    }

    // a song's key from its chords, a capo for a shift, and the voice exercises' notes
    void songKeysAndVoice()
    {
        using theorycalls::call;
        auto key = [] (std::vector<const char*> syms)
        {
            std::vector<harmony::Chord> cs;
            for (auto* x : syms) cs.push_back (C (x));
            const auto k = harmony::guessKey (cs);
            return k ? music::noteText (k->tonic) + (k->mode == music::Mode::minor ? " minor" : " major") : std::string ("none");
        };
        expect ("guessKey: Am F C G Am is A minor", key ({ "Am", "F", "C", "G", "Am" }) == "A minor", key ({ "Am", "F", "C", "G", "Am" }));
        expect ("guessKey: E A B7 E is E major", key ({ "E", "A", "B7", "E" }) == "E major");
        expect ("guessKey: Dm Gm A7 Dm is D minor", key ({ "Dm", "Gm", "A7", "Dm" }) == "D minor");
        expect ("guessKey: no chords, no key", key ({}) == "none");
        expect ("guessKey: a request skips what isn't a chord", call (R"({"fn":"guessKey","chords":["C","nope"]})") == R"({"tonic":{"L":0,"a":0},"mode":"major"})");
        expect ("capo: 1 to 7 frets for a shift up, none otherwise",
                harmony::capoForShift (3) == 3 && harmony::capoForShift (-5) == 7 && ! harmony::capoForShift (-3) && ! harmony::capoForShift (0) && ! harmony::capoForShift (12));

        const voice::VocalRange r { 48, 67, 52, 62 };
        const auto shifts = voice::keyShiftsForRange (60, 72, r, 0);
        expect ("key shifts: every shift an octave either way, best first", shifts.size() == 25 && shifts[0].shift == -9 && shifts[0].fits
                && shifts[0].strain == 2 && shifts[0].keyPc == 3 && std::abs (shifts[0].rank - 7.35) < 1e-12);
        bool sorted = true;
        for (size_t i = 1; i < shifts.size(); ++i) sorted = sorted && shifts[i - 1].rank <= shifts[i].rank;
        expect ("key shifts: in rank order", sorted);

        auto fixed = [] (std::vector<double> v) { auto i = std::make_shared<size_t> (0); return [v, i] { return v[(*i)++ % v.size()]; }; };
        expect ("next match note: level 3 within 7 of the last, never the last", voice::nextMatchNote (3, r, 55.0, fixed ({ 0.5 })) == 58);
        const auto tune = voice::buildSingBack (4, r, fixed ({ 0.1, 0.9, 0.2, 0.3, 0.7, 0.45 }));
        bool inside = tune.size() == 3;
        for (double m : tune) inside = inside && m >= 49 && m <= 66;
        expect ("sing back: three notes from level 3, all inside the safe range", inside);

        const auto w = voice::buildWarmup (r, false), full = voice::buildWarmup (r, true);
        std::string ids, fullIds;
        for (const auto& e : w) ids += e.id + " ";
        for (const auto& e : full) fullIds += e.id + " ";
        expect ("warm-up: the short one's exercises", ids == "hum trill siren arpeggio sustain cooldown ", ids);
        expect ("warm-up: the full one adds the vowel scale", fullIds == "hum trill siren arpeggio vowels sustain cooldown ", fullIds);
        bool safe = true;
        for (const auto& e : full)
            for (const auto& p : e.patterns)
                for (const auto& n : p.notes) safe = safe && n.midi >= 49 && n.midi <= 66;
        expect ("warm-up: every note inside the safe range", safe);
        expect ("warm-up: a siren lasts its slide up and back", voice::patternSeconds (w[2].patterns[0]) == 5.0
                && voice::glideAt (*w[2].patterns[0].glide, 2.5) == w[2].patterns[0].glide->to);
        expect ("a request with too few random numbers is an error", call (R"({"fn":"buildSingBack","level":3,"range":{"low":48,"high":67,"tessLo":52,"tessHi":62},"rand":[0.1]})") == R"({"error":"rand ran out"})");
    }

    void requests()
    {
        using theorycalls::call;
        expect ("a request: chordSymbol", call (R"({"fn":"chordSymbol","chord":{"root":{"L":3,"a":1},"type":"m7b5","bass":{"L":0,"a":0}}})") == R"("F#m7b5/C")");
        expect ("a request with text arguments", call (R"({"fn":"numeralChord","key":"C major","rn":"V7/V"})") == R"({"root":{"L":1,"a":0},"type":"7","bass":null})");
        expect ("numbers rounded to nine decimals", call (R"({"fn":"readCost","scale":"ionian","root":"F#"})") == "6");
        expect ("not JSON", call ("{fn:") == R"({"error":"not a JSON object"})");
        expect ("no such function", call (R"({"fn":"nope"})") == R"({"error":"no such function: nope"})");
        expect ("a bad argument", call (R"({"fn":"chordSymbol","chord":"H"})") == R"({"error":"not a chord: H"})");
        expect ("text escapes and \\u", call (R"({"fn":"parseChord","text":"C°7"})") == R"({"root":{"L":0,"a":0},"type":"dim7","bass":null})");
    }
}

int main()
{
    chordTypes();
    chordNames();
    numerals();
    keyMap();
    fretsAndKeys();
    scaleSpelling();
    modes();
    scaleFits();
    neck();
    songKeysAndVoice();
    requests();
    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    return failures == 0 ? 0 : 1;
}
