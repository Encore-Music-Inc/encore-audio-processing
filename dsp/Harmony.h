#pragma once

// Harmony: chords and keys, the music vocabulary every ENCORE. product names
// chords with. Degrees ("b7", "#11") and the chord types (dsp/ChordTypes.h,
// written by tools/chords.py); a chord's tones and symbol; reading a symbol;
// spelling a chord's root and bass; naming a set of sounding notes; roman
// numerals; the map of chords a key offers; how one chord moves to another
// and how strong a move it is; ways to change key; transposing. Plain C++20,
// no JUCE.
//
// It returns names and numbers. The sentences a product shows about them
// (why a move works, what an inversion is) are the product's, made from the
// facts returned here (MoveFacts, NoteSetName). Spelled notes and keys are
// dsp/Music.h's.

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "ChordTypes.h"
#include "Music.h"

namespace vs::harmony
{
    using music::Note;
    using music::Key;
    using music::Mode;
    using music::mod12;
    using music::mod7;
    using music::pcOf;
    using chords::ChordType;
    using chords::Family;

    // ---- degrees: intervals the way chord formulas write them ("b3", "#11")

    struct Degree
    {
        std::string d;
        int n = 1, step = 0, acc = 0, semis = 0;
    };

    // A degree like "b7" or "#11" (nothing when it isn't one)
    inline std::optional<Degree> degree (std::string_view d)
    {
        Degree g;
        g.d = std::string (d);
        size_t i = 0;
        if (d.substr (0, 2) == "bb") { g.acc = -2; i = 2; }
        else if (! d.empty() && d[0] == 'b') { g.acc = -1; i = 1; }
        else if (! d.empty() && d[0] == '#') { g.acc = 1; i = 1; }
        if (i >= d.size()) return {};
        int n = 0;
        for (; i < d.size(); ++i)
        {
            if (d[i] < '0' || d[i] > '9') return {};
            n = n * 10 + (d[i] - '0');
        }
        g.n = n;
        g.step = (n - 1) % 7;
        g.semis = music::majorSemis[(size_t) g.step] + g.acc + 12 * ((n - 1) / 7);
        return g;
    }

    inline Degree degreeOf (const chords::Degree& t) { return { t.d, t.n, t.step, t.acc, t.semis }; }

    // The note a degree above a spelled root ("F#" and "b5" is C)
    inline Note above (Note root, const Degree& g) { return music::onLetter (root.letter + g.step, pcOf (root) + g.semis); }
    inline Note above (Note root, std::string_view d) { return above (root, *degree (d)); }

    // A spelled interval: how many letters and how many semitones
    struct Interval { int steps = 0, semis = 0; };
    inline Interval intervalBetween (Note a, Note b) { return { mod7 (b.letter - a.letter), mod12 (pcOf (b) - pcOf (a)) }; }
    inline Note moveBy (Note n, Interval iv) { return music::onLetter (n.letter + iv.steps, pcOf (n) + iv.semis); }

    // ---- chords

    // A chord: spelled root, type (an index into chords::types) and, for a slash chord, a spelled bass
    struct Chord
    {
        Note root;
        int type = 0;
        std::optional<Note> bass;
        bool operator== (const Chord&) const = default;
    };

    inline const ChordType& typeOf (const Chord& c) { return chords::types[(size_t) c.type]; }
    inline int typeIndex (std::string_view id) { const auto* t = chords::find (id); return t ? chords::indexOf (*t) : -1; }

    // A chord, dropping a bass that is its root
    inline Chord chord (Note root, int type, std::optional<Note> bass = {})
    {
        if (bass && *bass == root) bass.reset();
        return { root, type, bass };
    }

    // One of a chord's notes: its degree, spelled note, pitch class and semitones above the root
    struct ChordTone { std::string d; Note note; int pc = 0; int semis = 0; };

    // The chord's notes, spelled, with their degrees
    inline std::vector<ChordTone> chordTones (const Chord& c)
    {
        std::vector<ChordTone> out;
        for (const auto& t : typeOf (c).tones)
        {
            const Note n = above (c.root, degreeOf (t));
            out.push_back ({ t.d, n, pcOf (n), t.semis });
        }
        return out;
    }

    // "F#m7b5", "C/E"
    inline std::string chordSymbol (const Chord& c)
    {
        return music::noteText (c.root) + typeOf (c).sym + (c.bass ? "/" + music::noteText (*c.bass) : std::string());
    }

    // Scores and costs are compared rounded to a millionth: they are sums of short
    // decimals, so two that are equal by the rule tie on every build (compilers
    // round the sums differently in the last place) and the order they were found decides.
    inline long long scoreKey (double v) { return std::llround (v * 1e6); }
    inline bool lessScore (double a, double b) { return scoreKey (a) < scoreKey (b); }

    namespace detail
    {
        // a note name at the start of s: a letter and up to two accidentals (as chord symbols write them)
        inline size_t noteLength (std::string_view s)
        {
            if (s.empty() || s[0] < 'A' || s[0] > 'G') return 0;
            if (s.substr (1, 2) == "##" || s.substr (1, 2) == "bb") return 3;
            if (s.size() > 1 && (s[1] == '#' || s[1] == 'b')) return 2;
            return 1;
        }

        inline std::string_view trim (std::string_view s)
        {
            auto space = [] (char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; };
            while (! s.empty() && space (s.front())) s.remove_prefix (1);
            while (! s.empty() && space (s.back())) s.remove_suffix (1);
            return s;
        }

        inline bool contains (const std::vector<std::string>& v, std::string_view x) { return std::find (v.begin(), v.end(), x) != v.end(); }
        template <typename T> inline bool has (const std::vector<T>& v, const T& x) { return std::find (v.begin(), v.end(), x) != v.end(); }
        inline bool isOpt (const ChordType& t, std::string_view d)
        {
            for (auto* o : t.opt) if (d == o) return true;
            return false;
        }
    }

    // Reads a chord symbol like "F#m7b5", "C/E" or "Bbmaj9" (nothing when it isn't one known here)
    inline std::optional<Chord> parseChord (std::string_view s)
    {
        s = detail::trim (s);
        const size_t rootLen = detail::noteLength (s);
        if (rootLen == 0) return {};
        std::string_view rest = s.substr (rootLen), sym = rest, bassText;
        // a trailing "/<note>" is the bass (the symbol before it is matched as short as it can be)
        if (const size_t slash = rest.rfind ('/'); slash != std::string_view::npos)
        {
            const auto tail = rest.substr (slash + 1);
            if (detail::noteLength (tail) == tail.size() && ! tail.empty()) { sym = rest.substr (0, slash); bassText = tail; }
        }
        static const std::map<std::string, std::string, std::less<>> aliases {
            { "M", "" }, { "maj", "" }, { "min", "m" }, { "-", "m" }, { "\302\260", "dim" }, { "o", "dim" }, { "+", "aug" },
            { "\303\270", "m7b5" }, { "\303\2707", "m7b5" }, { "o7", "dim7" }, { "\302\2607", "dim7" }, { "-7", "m7" },
            { "M7", "maj7" }, { "\316\224", "maj7" }, { "\316\2247", "maj7" }, { "sus", "sus4" }, { "mMaj7", "m(maj7)" },
            { "madd9", "m(add9)" }, { "+7", "7#5" }, { "aug7", "7#5" } };
        std::string symbol (sym);
        if (const auto a = aliases.find (symbol); a != aliases.end()) symbol = a->second;
        const auto* t = chords::bySymbol (symbol);
        if (! t) return {};
        const auto root = music::parseNote (s.substr (0, rootLen));
        if (! root) return {};
        std::optional<Note> bass;
        if (! bassText.empty()) bass = music::parseNote (bassText);
        return chord (*root, chords::indexOf (*t), bass);
    }

    // Pitch classes of a chord (with its bass first when the bass isn't one of its tones)
    inline std::vector<int> chordPcs (const Chord& c)
    {
        std::vector<int> pcs;
        for (const auto& t : chordTones (c)) pcs.push_back (t.pc);
        if (c.bass && ! detail::has (pcs, pcOf (*c.bass))) pcs.insert (pcs.begin(), pcOf (*c.bass));
        return pcs;
    }

    // ---- spelling a chord's root and bass

    namespace detail
    {
        inline int accCost (Note n) { return std::abs (n.acc) + (std::abs (n.acc) > 1 ? 4 : 0); }
        inline bool isBlack (int pc) { return music::plainNote (pc).acc != 0; }
    }

    // How to spell a chord's root: in a key when there is one (and it needs at most
    // one accidental), else a white key's plain name, else the black key's sharp or
    // flat name, whichever spells the chord's tones with fewer accidentals. side: 1
    // or -1 asks for the sharp or flat name of a black key, whatever the key says.
    inline Note spellRoot (int pc, int type, const Note* keyTonic = nullptr, int side = 0)
    {
        if (side != 0 && detail::isBlack (pc)) return music::spellNote (pc, nullptr, side);
        if (keyTonic)
        {
            const Note n = music::inKey (pc, *keyTonic);
            if (std::abs (n.acc) <= 1) return n;
        }
        const Note plain = music::plainNote (pc);
        if (plain.acc == 0) return plain;
        Note best = plain;
        double bestCost = 1e9;
        for (auto r : music::spellings (pc))
        {
            if (r.acc == 0) continue;
            double cost = 0;
            for (const auto& t : chords::types[(size_t) type].tones) cost += detail::accCost (above (r, degreeOf (t)));
            if (! (r == plain)) cost += 0.05;
            if (lessScore (cost, bestCost)) { best = r; bestCost = cost; }
        }
        return best;
    }

    // A bass note under a chord, spelled to match the root's side (sharps with sharps); side as for spellRoot
    inline Note spellBass (int pc, Note root, const Note* keyTonic = nullptr, int side = 0)
    {
        if (side != 0 && detail::isBlack (pc)) return music::spellNote (pc, nullptr, side);
        if (keyTonic)
        {
            const Note n = music::inKey (pc, *keyTonic);
            if (std::abs (n.acc) <= 1) return n;
        }
        const auto opts = music::spellings (pc);
        if (opts[0].acc == 0) return opts[0];
        const int want = root.acc < 0 ? -1 : root.acc > 0 ? 1 : music::plainNote (pc).acc;
        for (auto n : opts) if (n.acc == want) return n;
        return opts[0];
    }

    // ---- naming a set of sounding notes

    // Each pitch class a name labels ("R", "b3", "bass"), in rising pitch class order
    using ToneLabels = std::vector<std::pair<int, std::string>>;

    namespace detail
    {
        inline void setLabel (ToneLabels& l, int pc, std::string label)
        {
            for (auto& [p, s] : l) if (p == pc) { s = std::move (label); return; }
            l.push_back ({ pc, std::move (label) });
            std::stable_sort (l.begin(), l.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
        }
    }

    // One name for a set of notes. kind: a single note, a chord (with its type and
    // spelled chord), or two notes that aren't a power chord (an interval).
    struct NoteSetName
    {
        enum class Kind { note, chord, interval } kind = Kind::note;
        std::string symbol;
        double cost = 0;
        std::vector<Note> notes;       // the notes that sound, spelled
        ToneLabels labels;
        // a chord
        Chord chord;
        bool no3 = false;              // named without its 3rd ("C7(no3)")
        bool foreign = false;          // the bass is a note outside the chord (a slash chord)
        std::string bassDegree;        // the chord tone in the bass ("3", "b7"), "" in root position or over a foreign bass
        bool powerFifthInBass = false; // a power chord with its 5th in the bass
        std::vector<std::string> formula, missing;
        // an interval: its size, its lower and upper note, and the chord it most often
        // stands for (hintType "maj" or "min", or "" when none; hintOverBass when its root is the lower note)
        int semis = 0;
        Note low, top;
        std::string hintType;
        Note hintRoot;
        bool hintOverBass = false;
    };

    namespace detail
    {
        struct Match
        {
            int rootPc = 0, type = 0;
            double cost = 0;
            std::vector<std::string> missing;
            bool no3 = false;
            int bassPc = 0;
            bool foreign = false;
            std::string bassDeg;
        };

        inline void matchAll (const std::vector<int>& pcs, int bassPc, bool foreign, std::vector<Match>& out, int upperLow = -1)
        {
            static const std::vector<std::string> no3Ok { "7", "maj7", "9", "maj9", "13", "6" };
            for (int r : pcs)
            {
                std::vector<int> rel;
                for (int p : pcs) { const int s = mod12 (p - r); if (! has (rel, s)) rel.push_back (s); }
                for (const auto& t : chords::types)
                {
                    if (t.tones.size() < 3 && pcs.size() > 2) continue;
                    // semitone class -> degree (a later tone on the same class wins, as a map built in order does)
                    std::vector<std::pair<int, std::string>> haveList;
                    for (const auto& g : t.tones)
                    {
                        const int k = mod12 (g.semis);
                        bool found = false;
                        for (auto& [s, d] : haveList) if (s == k) { d = g.d; found = true; }
                        if (! found) haveList.push_back ({ k, g.d });
                    }
                    auto have = [&] (int s) -> const std::string* { for (auto& [k, d] : haveList) if (k == s) return &d; return nullptr; };
                    bool covers = true;
                    for (int s : rel) if (! have (s)) covers = false;
                    if (! covers) continue;
                    if (foreign && have (mod12 (bassPc - r))) continue;
                    std::vector<std::string> missing;
                    for (const auto& g : t.tones) if (! has (rel, mod12 (g.semis))) missing.push_back (g.d);
                    double cost = t.cost;
                    bool ok = true, no3 = false;
                    for (const auto& d : missing)
                    {
                        if (isOpt (t, d)) cost += d == "5" ? (t.tones.size() >= 4 ? 0.5 : 2.5) : 0.3;
                        else if (d == "3" && contains (no3Ok, t.id) && ! foreign && ! no3 && rel.size() >= 3) { cost += 3; no3 = true; }
                        else ok = false;
                    }
                    if (! ok) continue;
                    std::string bassDeg = foreign ? std::string() : *have (mod12 (bassPc - r));
                    if (! foreign && bassPc != r)
                    {
                        cost += 1.5;
                        const auto g = *degree (bassDeg);
                        if (g.n > 7 || g.d == "6" || g.d == "2" || g.d == "4") cost += 1;
                    }
                    if (foreign)
                    {
                        if (t.cost > 2.5) continue;
                        if ((std::string_view (t.id) == "dim7" || std::string_view (t.id) == "aug") && r != upperLow) continue;
                        cost += 2.8;
                    }
                    out.push_back ({ r, chords::indexOf (t), cost, missing, no3, bassPc, foreign, bassDeg });
                }
            }
        }

        inline NoteSetName describe (const Match& m, const Note* keyTonic, int side)
        {
            const Note root = spellRoot (m.rootPc, m.type, keyTonic, side);
            std::optional<Note> bass;
            if (m.bassPc != m.rootPc && m.foreign) bass = spellBass (m.bassPc, root, keyTonic, side);
            Chord c = chord (root, m.type, bass);
            const auto tones = chordTones (c);
            if (! m.foreign && m.bassPc != m.rootPc)
                for (const auto& t : tones) if (t.pc == m.bassPc) { c = chord (root, m.type, t.note); break; }
            const auto& t = chords::types[(size_t) m.type];
            NoteSetName r;
            r.kind = NoteSetName::Kind::chord;
            r.chord = c;
            r.symbol = music::noteText (root) + t.sym + (m.no3 ? "(no3)" : "") + (c.bass ? "/" + music::noteText (*c.bass) : std::string());
            r.cost = m.cost;
            for (const auto& x : tones) if (! contains (m.missing, x.d)) setLabel (r.labels, x.pc, x.d == "1" ? "R" : x.d);
            if (m.foreign) setLabel (r.labels, m.bassPc, "bass");
            for (const auto& x : tones) if (! contains (m.missing, x.d)) r.notes.push_back (x.note);
            r.no3 = m.no3;
            r.foreign = m.foreign;
            r.bassDegree = (! m.foreign && m.bassPc != m.rootPc) ? m.bassDeg : std::string();
            for (const auto& g : t.tones) r.formula.push_back (contains (m.missing, g.d) ? "(" + std::string (g.d) + ")" : std::string (g.d));
            r.missing = m.missing;
            return r;
        }

        inline std::vector<NoteSetName> nameTwo (const std::vector<int>& pcs, int bassPc, const Note* keyTonic, int side)
        {
            const int other = pcs[0] != bassPc ? pcs[0] : pcs[1], semis = mod12 (other - bassPc);
            const Note low = music::spellNote (bassPc, nullptr, 0);
            Note lowSpelled = low;
            {
                music::Key k;
                if (keyTonic) { k.tonic = *keyTonic; lowSpelled = music::spellNote (bassPc, &k, side); }
                else lowSpelled = music::spellNote (bassPc, nullptr, side);
            }
            std::vector<NoteSetName> out;
            const int five = typeIndex ("5");
            auto power = [&] (int rootPc, bool bassAbove)
            {
                const Note root = spellRoot (rootPc, five, keyTonic, side), fifth = above (root, "5");
                const Chord c = chord (root, five, bassAbove ? std::optional<Note> (fifth) : std::nullopt);
                NoteSetName r;
                r.kind = NoteSetName::Kind::chord;
                r.chord = c;
                r.symbol = chordSymbol (c);
                r.cost = bassAbove ? 1.5 : 0.6;
                r.notes = { root, fifth };
                setLabel (r.labels, rootPc, "R");
                setLabel (r.labels, pcOf (fifth), "5");
                r.formula = { "1", "5" };
                r.powerFifthInBass = bassAbove;
                out.push_back (r);
            };
            if (semis == 7) power (bassPc, false);
            if (semis == 5) power (other, true);
            // any other pair is an interval, with the chord it most often stands for
            std::string hint;
            int hintPc = 0;
            if (semis == 3) { hint = "min"; hintPc = bassPc; }
            else if (semis == 4) { hint = "maj"; hintPc = bassPc; }
            else if (semis == 8) { hint = "maj"; hintPc = other; }
            else if (semis == 9) { hint = "min"; hintPc = other; }
            std::optional<Note> up;
            if (! hint.empty()) up = spellRoot (hintPc, typeIndex (hint), keyTonic, side);
            constexpr int stepFor[12] { 0, 1, 1, 2, 2, 3, 3, 4, 5, 5, 6, 6 };
            const std::string third = hint == "min" ? "b3" : "3";
            const Note top = up ? (hintPc == bassPc ? above (*up, third) : *up) : music::onLetter (lowSpelled.letter + stepFor[semis], other);
            const Note lowN = up && hintPc == bassPc ? *up : up ? above (*up, third) : lowSpelled;
            NoteSetName r;
            r.kind = NoteSetName::Kind::interval;
            r.symbol = music::noteText (lowN) + " + " + music::noteText (top);
            r.cost = 5;
            r.notes = { lowN, top };
            r.semis = semis;
            r.low = lowN;
            r.top = top;
            r.hintType = hint;
            if (up) r.hintRoot = *up;
            r.hintOverBass = up && hintPc == bassPc;
            if (! hint.empty())
            {
                setLabel (r.labels, hintPc, "R");
                setLabel (r.labels, hintPc == bassPc ? other : bassPc, third);
            }
            else
            {
                static const char* const labels[12] { "R", "b2", "2", "b3", "3", "4", "b5", "5", "b6", "6", "b7", "7" };
                setLabel (r.labels, bassPc, "R");
                setLabel (r.labels, other, labels[semis]);
            }
            out.push_back (r);
            (void) low;
            return out;
        }
    }

    // Every plausible name for a set of sounding notes, best first. midis: MIDI notes
    // (the lowest is the bass). keyTonic: spell for that key (nullptr: fewest
    // accidentals). side: 1 names black-key roots and basses with sharps, -1 with flats, 0 decides.
    inline std::vector<NoteSetName> nameNotes (std::vector<int> midis, const Note* keyTonic = nullptr, int side = 0)
    {
        std::sort (midis.begin(), midis.end());
        midis.erase (std::unique (midis.begin(), midis.end()), midis.end());
        if (midis.empty()) return {};
        std::vector<int> pcs;
        for (int m : midis) if (! detail::has (pcs, mod12 (m))) pcs.push_back (mod12 (m));
        const int bassPc = mod12 (midis[0]);
        if (pcs.size() == 1)
        {
            music::Key k;
            if (keyTonic) k.tonic = *keyTonic;
            const Note n = music::spellNote (bassPc, keyTonic ? &k : nullptr, side);
            NoteSetName r;
            r.kind = NoteSetName::Kind::note;
            r.symbol = music::noteText (n);
            r.notes = { n };
            r.labels = { { bassPc, "R" } };
            return { r };
        }
        if (pcs.size() == 2) return detail::nameTwo (pcs, bassPc, keyTonic, side);
        std::vector<detail::Match> found;
        detail::matchAll (pcs, bassPc, false, found);
        if (pcs.size() >= 4)
        {
            std::vector<int> upper;
            for (int p : pcs) if (p != bassPc) upper.push_back (p);
            int upperLow = -1;
            for (int m : midis) if (mod12 (m) != bassPc) { upperLow = mod12 (m); break; }
            detail::matchAll (upper, bassPc, true, found, upperLow);
        }
        // the cheapest of each root, type and foreign-ness, kept where it was first found
        std::vector<std::pair<std::string, detail::Match>> best;
        for (const auto& m : found)
        {
            const std::string k = std::to_string (m.rootPc) + ":" + std::to_string (m.type) + ":" + (m.foreign ? "f" : "");
            auto it = std::find_if (best.begin(), best.end(), [&] (const auto& b) { return b.first == k; });
            if (it == best.end()) best.push_back ({ k, m });
            else if (lessScore (m.cost, it->second.cost)) it->second = m;
        }
        std::vector<detail::Match> list;
        for (auto& b : best) list.push_back (b.second);
        std::stable_sort (list.begin(), list.end(), [] (const auto& a, const auto& b)
        {
            if (scoreKey (a.cost) != scoreKey (b.cost)) return lessScore (a.cost, b.cost);
            return a.missing.size() < b.missing.size();
        });
        std::vector<std::string> seen;
        std::vector<NoteSetName> out;
        for (const auto& m : list)
        {
            if (lessScore (list[0].cost + 3.6, m.cost)) break;
            auto r = detail::describe (m, keyTonic, side);
            if (detail::contains (seen, r.symbol)) continue;
            seen.push_back (r.symbol);
            out.push_back (std::move (r));
        }
        return out;
    }

    // ---- roman numerals. Every chord is counted against the major scale of the key's
    // tonic, so in a minor key the 3rd, 6th and 7th chords carry a flat (i, ii°, bIII,
    // iv, v, bVI, bVII), the way pop and jazz write them.

    namespace detail
    {
        inline constexpr const char* roman[7] { "I", "II", "III", "IV", "V", "VI", "VII" };

        inline bool startsWith (std::string_view s, std::string_view p) { return s.substr (0, p.size()) == p; }

        inline std::string lowerAscii (std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a'); return s; }
        inline std::string upperAscii (std::string s) { for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char) (c - 'a' + 'A'); return s; }

        inline std::vector<std::string_view> splitSlash (std::string_view s)
        {
            std::vector<std::string_view> out;
            size_t at = 0;
            while (at <= s.size())
            {
                const size_t k = s.find ('/', at);
                const auto part = s.substr (at, k == std::string_view::npos ? std::string_view::npos : k - at);
                if (! part.empty()) out.push_back (part);
                if (k == std::string_view::npos) break;
                at = k + 1;
            }
            return out;
        }

        // A numeral's parts: accidental, the roman letters, the suffix and the slash parts after it
        struct NumeralParts { std::string acc, roman, suffix, rest; };
        inline std::optional<NumeralParts> splitNumeral (std::string_view rn)
        {
            rn = trim (rn);
            NumeralParts p;
            if (! rn.empty() && (rn[0] == 'b' || rn[0] == '#')) { p.acc = std::string (1, rn[0]); rn.remove_prefix (1); }
            static const char* const romans[] { "VII", "VI", "IV", "V", "III", "II", "I", "vii", "vi", "iv", "v", "iii", "ii", "i" };
            for (auto* r : romans)
                if (startsWith (rn, r)) { p.roman = r; rn.remove_prefix (std::string_view (r).size()); break; }
            if (p.roman.empty()) return {};
            const size_t slash = rn.find ('/');
            p.suffix = std::string (rn.substr (0, slash));
            if (slash != std::string_view::npos)
            {
                p.rest = std::string (rn.substr (slash));
                // every slash part must be non-empty (a "//" isn't a numeral)
                if (p.rest.find ("//") != std::string::npos || p.rest.back() == '/') return {};
            }
            return p;
        }

        inline bool isInversionPart (std::string_view s)
        {
            if (! s.empty() && (s[0] == 'b' || s[0] == '#')) s.remove_prefix (1);
            if (s.empty()) return false;
            for (char c : s) if (c < '0' || c > '9') return false;
            return true;
        }

        inline std::string stripAccidentals (std::string_view d)
        {
            while (! d.empty() && (d[0] == 'b' || d[0] == '#')) d.remove_prefix (1);
            return std::string (d);
        }
    }

    // The chord a numeral means in a key: "V7", "bVI", "iiø7", "vii°7/V" (a secondary:
    // counted from that chord's root), "I/3" (an inversion: which tone is in the bass).
    // Nothing when it isn't a numeral or its quality isn't a known chord.
    inline std::optional<Chord> numeralChord (const Key& key, std::string_view rn)
    {
        const auto p = detail::splitNumeral (rn);
        if (! p) return {};
        const bool upper = p->roman == detail::upperAscii (p->roman);
        int step = 0;
        for (int i = 0; i < 7; ++i) if (detail::upperAscii (p->roman) == detail::roman[i]) step = i;
        Note ref = key.tonic;
        std::string inv;
        for (auto part : detail::splitSlash (p->rest))
        {
            if (detail::isInversionPart (part)) inv = std::string (part);
            else
            {
                const auto c = numeralChord (key, part);
                if (! c) return {};
                ref = c->root;
            }
        }
        const Note root = above (ref, p->acc + std::to_string (step + 1));
        std::string sym;
        const std::string& s = p->suffix;
        if (detail::startsWith (s, "\302\260")) sym = s == "\302\2607" ? "dim7" : "dim";
        else if (detail::startsWith (s, "\303\270")) sym = s == "\303\2709" ? "m9b5" : "m7b5";
        else if (s == "+") sym = "aug";
        else if (s == "+7") sym = "7#5";
        else if (upper) sym = s;
        else
        {
            static const std::map<std::string, std::string, std::less<>> lower { { "maj7", "(maj7)" }, { "add9", "(add9)" }, { "add11", "(add11)" }, { "maj9", "(maj9)" } };
            const auto it = lower.find (s);
            sym = "m" + (it != lower.end() ? it->second : s);
        }
        const auto* t = chords::bySymbol (sym);
        if (! t) return {};
        std::optional<Note> bass;
        if (! inv.empty())
        {
            const std::string n = detail::stripAccidentals (inv);
            for (const auto& x : chordTones (chord (root, chords::indexOf (*t))))
                if (detail::stripAccidentals (x.d) == n) { bass = x.note; break; }
        }
        return chord (root, chords::indexOf (*t), bass);
    }

    // A numeral for any chord in a key, from its root and quality (F#m7b5 in C is "#ivø7")
    inline std::string plainNumeral (const Key& key, const Chord& c)
    {
        const int step = mod7 (c.root.letter - key.tonic.letter);
        const Note ref = above (key.tonic, std::to_string (step + 1));
        int alt = mod12 (pcOf (c.root) - pcOf (ref));
        if (alt > 6) alt -= 12;
        const auto& t = typeOf (c);
        const bool lower = t.family == Family::min || t.family == Family::dim || t.family == Family::hdim;
        const std::string r = detail::roman[step];
        return std::string ((size_t) std::abs (alt), alt < 0 ? 'b' : '#') + (lower ? detail::lowerAscii (r) : r) + t.num;
    }

    // The slash part of a numeral: which tone is in the bass ("/3"), "" in root position.
    // overBass: a bass that isn't one of the chord's tones (a product writes " over E").
    struct InversionPart { std::string slash; std::optional<Note> overBass; };
    inline InversionPart inversionPart (const Chord& c)
    {
        if (! c.bass) return {};
        for (const auto& x : chordTones (c)) if (x.note == *c.bass) return { "/" + detail::stripAccidentals (x.d), {} };
        return { "", c.bass };
    }

    inline bool sameChord (const Chord& a, const Chord& b) { return pcOf (a.root) == pcOf (b.root) && a.type == b.type; }

    // ---- the map of a key

    // What a chord does in a key: home (tonic), away (subdominant), tension (dominant)
    enum class Function { home, away, tension };
    enum class PaletteGroup { diatonic, sevenths, secondary, borrowed, relative, spice };

    // One chord on the map of a key
    struct PaletteEntry
    {
        PaletteGroup group = PaletteGroup::diatonic;
        std::string rn;          // the numeral it was made from ("" for the relative key's chords)
        std::string label;       // the numeral shown ("N (bII)", "subV7/V", "V in Am" for the relative key's chords)
        std::string relativeRn;  // the relative key's chords: the numeral in that key
        Chord chord;
        std::string alias;       // how it would be written with a double sharp or flat, when it's written the simpler way instead
        Function fn = Function::home;
        std::string target;      // the numeral of the chord it pulls to, or ""
        std::vector<Note> outside; // the relative key's chords: their notes from outside this key
    };

    struct KeyPalette
    {
        Key key;
        std::vector<std::pair<PaletteGroup, std::vector<PaletteEntry>>> groups;
        std::vector<PaletteEntry> all;
        Key relative;            // the relative key (minor for a major key, major for a minor one)
        Chord relativeHome;      // its home chord
    };

    namespace detail
    {
        struct MapLine { const char* rn; Function fn; };
        using F = Function;
        inline const std::vector<MapLine> majorTriads { { "I", F::home }, { "ii", F::away }, { "iii", F::home }, { "IV", F::away }, { "V", F::tension }, { "vi", F::home }, { "vii\302\260", F::tension } };
        inline const std::vector<MapLine> majorSevenths { { "Imaj7", F::home }, { "ii7", F::away }, { "iii7", F::home }, { "IVmaj7", F::away }, { "V7", F::tension }, { "vi7", F::home }, { "vii\303\2707", F::tension } };
        inline const std::vector<MapLine> minorTriads { { "i", F::home }, { "ii\302\260", F::away }, { "bIII", F::home }, { "iv", F::away }, { "v", F::tension }, { "V", F::tension }, { "bVI", F::away }, { "bVII", F::tension } };
        inline const std::vector<MapLine> minorSevenths { { "i7", F::home }, { "ii\303\2707", F::away }, { "bIIImaj7", F::home }, { "iv7", F::away }, { "bVImaj7", F::away }, { "bVII7", F::tension }, { "V7", F::tension }, { "vii\302\2607", F::tension } };
        inline const std::vector<std::pair<const char*, const char*>> secondaryMajor { { "V7/ii", "ii" }, { "V7/iii", "iii" }, { "V7/IV", "IV" }, { "V7/V", "V" }, { "V7/vi", "vi" } };
        inline const std::vector<std::pair<const char*, const char*>> secondaryMinor { { "V7/iv", "iv" }, { "V7/V", "V" }, { "V7/bVI", "bVI" }, { "V7/bVII", "bVII" } };
        inline const std::vector<MapLine> borrowedMajor { { "iv", F::away }, { "bVII", F::tension }, { "bVI", F::away }, { "bIII", F::home }, { "i", F::home }, { "ii\303\2707", F::away }, { "v", F::tension }, { "II", F::away } };
        inline const std::vector<MapLine> borrowedMinor { { "I", F::home }, { "IV", F::away }, { "ii", F::away } };
        inline const std::vector<MapLine> relativeOfMajor { { "i", F::home }, { "iv", F::away }, { "V", F::tension }, { "V7", F::tension }, { "vii\302\2607", F::tension }, { "ii\303\2707", F::away }, { "bVI", F::away }, { "bVII", F::tension }, { "i7", F::home } };
        inline const std::vector<MapLine> relativeOfMinor { { "I", F::home }, { "IV", F::away }, { "V", F::tension }, { "V7", F::tension }, { "ii", F::away }, { "iii", F::home }, { "Imaj7", F::home }, { "IVmaj7", F::away }, { "V7/V", F::tension } };

        inline Chord numeral (const Key& key, std::string_view rn) { return *numeralChord (key, rn); }
        inline Key makeKey (Note tonic, Mode mode) { return { tonic, mode }; }
    }

    // Every chord on the map of a key, in groups: its own chords, with 7ths, secondary
    // dominants, borrowed chords, the relative key's chords and the spicy ones
    inline KeyPalette keyPalette (const Key& key)
    {
        using namespace detail;
        KeyPalette p;
        p.key = key;
        const bool minor = key.mode == Mode::minor;
        auto entry = [&] (PaletteGroup group, std::string_view rn, Function fn, std::string label = {}, std::string target = {})
        {
            PaletteEntry e;
            e.group = group;
            e.rn = std::string (rn);
            e.label = label.empty() ? e.rn : label;
            e.chord = numeral (key, rn);
            if (group != PaletteGroup::diatonic && group != PaletteGroup::sevenths)
            {
                bool odd = false;
                for (const auto& x : chordTones (e.chord)) if (std::abs (x.note.acc) > 1) odd = true;
                if (odd)
                {
                    e.alias = chordSymbol (e.chord);
                    e.chord = chord (spellRoot (pcOf (e.chord.root), e.chord.type), e.chord.type);
                }
            }
            e.fn = fn;
            e.target = target;
            return e;
        };
        auto add = [&] (PaletteGroup id, std::vector<PaletteEntry> list) { p.groups.push_back ({ id, std::move (list) }); };
        {
            std::vector<PaletteEntry> l;
            for (const auto& m : minor ? minorTriads : majorTriads) l.push_back (entry (PaletteGroup::diatonic, m.rn, m.fn));
            add (PaletteGroup::diatonic, l);
        }
        {
            std::vector<PaletteEntry> l;
            for (const auto& m : minor ? minorSevenths : majorSevenths) l.push_back (entry (PaletteGroup::sevenths, m.rn, m.fn));
            add (PaletteGroup::sevenths, l);
        }
        {
            std::vector<PaletteEntry> l;
            for (const auto& [rn, t] : minor ? secondaryMinor : secondaryMajor) l.push_back (entry (PaletteGroup::secondary, rn, Function::tension, {}, t));
            add (PaletteGroup::secondary, l);
        }
        {
            std::vector<PaletteEntry> l;
            for (const auto& m : minor ? borrowedMinor : borrowedMajor) l.push_back (entry (PaletteGroup::borrowed, m.rn, m.fn));
            add (PaletteGroup::borrowed, l);
        }
        const Key rel = makeKey (numeral (key, minor ? "bIII" : "vi").root, minor ? Mode::major : Mode::minor);
        p.relative = rel;
        p.relativeHome = numeral (rel, minor ? "I" : "i");
        {
            std::vector<int> inKeyPcs;
            for (auto n : music::scaleNotes (key)) inKeyPcs.push_back (pcOf (n));
            std::vector<PaletteEntry> l;
            for (const auto& m : minor ? relativeOfMinor : relativeOfMajor)
            {
                PaletteEntry e;
                e.group = PaletteGroup::relative;
                e.relativeRn = m.rn;
                e.label = std::string (m.rn) + " in " + chordSymbol (p.relativeHome);
                e.chord = numeral (rel, m.rn);
                e.fn = m.fn;
                for (const auto& x : chordTones (e.chord)) if (! has (inKeyPcs, x.pc)) e.outside.push_back (x.note);
                l.push_back (e);
            }
            add (PaletteGroup::relative, l);
        }
        {
            std::vector<PaletteEntry> l;
            l.push_back (entry (PaletteGroup::spice, "bII", Function::away, "N (bII)", "V"));
            for (const char* t : minor ? std::vector<const char*> { "V", "iv", "bVI" } : std::vector<const char*> { "ii", "V", "vi", "IV" })
                l.push_back (entry (PaletteGroup::spice, std::string ("vii\302\2607/") + t, Function::tension, {}, t));
            for (const char* t : minor ? std::vector<const char*> { "i", "V", "iv" } : std::vector<const char*> { "I", "V", "ii", "vi" })
                l.push_back (entry (PaletteGroup::spice, std::string ("bII7/") + t, Function::tension, std::string ("subV7/") + t, t));
            add (PaletteGroup::spice, l);
        }
        for (auto& [id, l] : p.groups) for (auto& e : l) p.all.push_back (e);
        return p;
    }

    // The numeral to show for a chord in a key: the map's name for it if it's on the map,
    // else worked out; then its inversion. next: the chord after it (a secondary dominant
    // that doesn't go to its target is just a 7th chord).
    struct Numeral { std::string label; InversionPart inversion; };
    inline Numeral numeralOf (const Key& key, const Chord& c, const KeyPalette* palette = nullptr, const Chord* next = nullptr)
    {
        const KeyPalette own = palette ? KeyPalette {} : keyPalette (key);
        const KeyPalette& p = palette ? *palette : own;
        const PaletteEntry* hit = nullptr;
        for (const auto& e : p.all) if (sameChord (e.chord, c)) { hit = &e; break; }
        if (hit && ! hit->target.empty() && next && pcOf (next->root) != pcOf (detail::numeral (key, hit->target).root)) hit = nullptr;
        return { hit ? hit->label : plainNumeral (key, c), inversionPart (c) };
    }

    // ---- changing key through a chord both keys share (a pivot chord)

    struct Pivot { std::string here, there; Chord chord; };

    // A way to change key: a pivot chord, the new key's V7, then its home (the route).
    // target: the numeral (in this key) of the new key's home chord.
    struct Modulation
    {
        Key key;
        std::string target;
        Pivot pivot;
        std::vector<Pivot> shared;
        std::vector<Chord> route;
    };

    inline std::vector<Modulation> modulations (const Key& key)
    {
        using namespace detail;
        static const std::vector<const char*> triadsMajor { "I", "ii", "iii", "IV", "V", "vi", "vii\302\260" };
        static const std::vector<const char*> triadsMinor { "i", "ii\302\260", "bIII", "iv", "v", "V", "bVI", "bVII" };
        static const std::vector<const char*> targetsMajor { "V", "vi", "IV", "ii", "iii" };
        static const std::vector<const char*> targetsMinor { "bIII", "iv", "v", "bVI", "bVII" };
        auto rank = [] (const std::string& rn)
        {
            if (rn == "ii") return 0;
            if (rn == "IV" || rn == "iv") return 1;
            if (rn == "ii\302\260") return 2;
            if (rn == "vi" || rn == "bVI") return 3;
            if (rn == "iii" || rn == "bIII") return 4;
            return 5;
        };
        std::vector<Modulation> out;
        for (const char* rn : key.mode == Mode::minor ? targetsMinor : targetsMajor)
        {
            const Chord tonicChord = numeral (key, rn);
            const std::string r (rn);
            const Key nk = makeKey (tonicChord.root, r == lowerAscii (r) ? Mode::minor : Mode::major);
            std::vector<Pivot> shared;
            const auto& here = key.mode == Mode::minor ? triadsMinor : triadsMajor;
            const auto& there = nk.mode == Mode::minor ? triadsMinor : triadsMajor;
            for (const char* h : here)
                for (const char* t : there)
                {
                    const Chord hc = numeral (key, h), tc = numeral (nk, t);
                    const std::string hs (h), ts (t);
                    if (sameChord (hc, tc) && hs != "i" && hs != "I" && ts != "i" && ts != "I") shared.push_back ({ hs, ts, tc });
                }
            std::stable_sort (shared.begin(), shared.end(), [&] (const Pivot& a, const Pivot& b) { return rank (a.there) < rank (b.there); });
            if (shared.empty()) continue;
            Modulation m;
            m.key = nk;
            m.target = r;
            m.pivot = shared[0];
            m.shared = shared;
            m.route = { shared[0].chord, numeral (nk, "V7"), numeral (nk, nk.mode == Mode::minor ? "i" : "I") };
            out.push_back (m);
        }
        return out;
    }

    // ---- where a chord can go next, and why

    // How a chord moves to the next: a cadence home, a deceptive surprise, or the kind of root motion
    enum class MoveKind { resolve, surprise, strong, smooth, step, leap, colour };

    // Which rule named the move, for the product's sentence about it
    enum class MoveReason { cadence, ownDominant, tritoneSub, leadingTone, deceptive, plagal, backdoor, motion };

    // The facts behind a move, for the product's sentence: the notes that move, the
    // ones the chords share, and what each chord does.
    struct MoveFacts
    {
        MoveReason reason = MoveReason::motion;
        int rootInterval = 0;                       // semitones from one root up to the next (0..11)
        bool toHome = false, fromHome = false;
        bool dominantSeventh = false;               // the first chord is a 7th chord (V7 rather than V)
        std::optional<Note> third, seventh;         // the first chord's 3rd and 7th
        std::optional<Note> nextThird;              // the second chord's 3rd (major or minor)
        bool seventhSettles = false;                // the 7th falls a step to the next 3rd
        Note expected;                              // a deceptive cadence: the root the ear expected
        std::optional<Chord> expectedChord;         // ... and its chord on the map
        std::vector<Note> kept;                     // ... the notes the surprise keeps from it
        std::optional<Note> minorThird, nextFifth;  // a minor plagal cadence: the b3 that sinks to the 5th
        std::vector<Note> common;                   // the notes both chords share
        Function fromFn = Function::home, toFn = Function::home;
        std::optional<Chord> wantsNext;             // the second chord's own target, when it pulls somewhere
    };

    struct Relation { double score = 0; MoveKind kind = MoveKind::colour; MoveFacts facts; };

    namespace detail
    {
        struct ChordInfo
        {
            const PaletteEntry* entry = nullptr;
            Function fn = Function::home;
            bool dominant = false, leading = false;
            std::vector<ChordTone> tones;
            const ChordType* t = nullptr;
        };

        inline ChordInfo infoOf (const Key& key, const Chord& c, const KeyPalette& p)
        {
            ChordInfo i;
            for (const auto& e : p.all) if (sameChord (e.chord, c)) { i.entry = &e; break; }
            i.t = &typeOf (c);
            static const Function byStepMajor[7] { F::home, F::away, F::home, F::away, F::tension, F::home, F::tension };
            static const Function byStepMinor[7] { F::home, F::away, F::home, F::away, F::tension, F::away, F::tension };
            if (i.entry) i.fn = i.entry->fn;
            else if (i.t->family == Family::dom || i.t->family == Family::dim) i.fn = F::tension;
            else i.fn = (key.mode == Mode::minor ? byStepMinor : byStepMajor)[mod7 (c.root.letter - key.tonic.letter)];
            i.tones = chordTones (c);
            bool hasThird = false;
            for (const auto& x : i.tones) if (x.d == "3") hasThird = true;
            i.dominant = (i.t->family == Family::dom && hasThird)
                      || (std::string_view (i.t->id) == "maj" && i.entry && i.entry->fn == F::tension && i.entry->group != PaletteGroup::borrowed && i.entry->rn != "bVII");
            i.leading = i.t->family == Family::dim || (i.t->family == Family::hdim && i.fn == F::tension);
            return i;
        }

        inline const ChordTone* toneOf (const ChordInfo& i, std::string_view d)
        {
            for (const auto& x : i.tones) if (x.d == d) return &x;
            return nullptr;
        }

        inline bool isHome (const Key& key, const Chord& c)
        {
            static const std::vector<std::string> major { "maj", "maj7", "6", "add9", "maj9", "69" };
            static const std::vector<std::string> minor { "min", "m7", "m6", "madd9", "m9", "mMaj7", "m69" };
            return mod12 (pcOf (c.root) - pcOf (key.tonic)) == 0 && contains (key.mode == Mode::minor ? minor : major, typeOf (c).id);
        }

        inline bool stable (Family f) { return f == Family::maj || f == Family::min || f == Family::dom; }

        inline bool isRealV (const ChordInfo& i)
        {
            return i.entry && (i.entry->group == PaletteGroup::diatonic || i.entry->group == PaletteGroup::sevenths || i.entry->group == PaletteGroup::secondary);
        }

        inline double groupScore (const PaletteEntry* e)
        {
            if (! e) return -2;
            switch (e->group)
            {
                case PaletteGroup::diatonic:  return 0;
                case PaletteGroup::sevenths:  return -0.3;
                case PaletteGroup::secondary: return -1;
                case PaletteGroup::relative:  return -1;
                case PaletteGroup::borrowed:  return -1.5;
                case PaletteGroup::spice:     return -2;
            }
            return -2;
        }
    }

    // How one chord moves to another in a key: a score (higher is a stronger, more usual
    // move), its kind, and the facts behind it
    inline Relation relate (const Key& key, const Chord& from, const Chord& to, const KeyPalette* palette = nullptr)
    {
        using namespace detail;
        const KeyPalette own = palette ? KeyPalette {} : keyPalette (key);
        const KeyPalette& p = palette ? *palette : own;
        const auto a = infoOf (key, from, p), b = infoOf (key, to, p);
        const int d = mod12 (pcOf (to.root) - pcOf (from.root));
        Relation r;
        auto& f = r.facts;
        f.rootInterval = d;
        f.fromFn = a.fn;
        f.toFn = b.fn;
        for (const auto& x : b.tones)
        {
            bool shared = false;
            for (const auto& y : a.tones) if (y.pc == x.pc) shared = true;
            if (shared) f.common.push_back (x.note);
        }
        f.toHome = isHome (key, to);
        f.fromHome = isHome (key, from);
        const auto* a3 = toneOf (a, "3");
        const auto* a7 = toneOf (a, "b7");
        const auto* b3 = toneOf (b, "3") ? toneOf (b, "3") : toneOf (b, "b3");
        if (a3) f.third = a3->note;
        if (a7) f.seventh = a7->note;
        if (b3) f.nextThird = b3->note;
        f.dominantSeventh = a7 != nullptr;
        if (a.dominant && d == 5 && stable (b.t->family))
        {
            r.kind = MoveKind::resolve;
            r.score = 10 + (f.toHome ? 1 : 0);
            f.reason = f.toHome ? MoveReason::cadence : MoveReason::ownDominant;
            if (a7 && b3) { const int fall = mod12 (a7->pc - b3->pc); f.seventhSettles = fall == 1 || fall == 2; }
        }
        else if (a.dominant && a.t->family == Family::dom && d == 11 && ! isRealV (a) && stable (b.t->family))
        {
            r.kind = MoveKind::resolve;
            r.score = 9 + (f.toHome ? 0.5 : 0);
            f.reason = MoveReason::tritoneSub;
        }
        else if (a.leading && d == 1 && stable (b.t->family))
        {
            r.kind = MoveKind::resolve;
            r.score = 9 + (f.toHome ? 0.5 : 0);
            f.reason = MoveReason::leadingTone;
        }
        else if (a.dominant && isRealV (a) && ((d == 2 && b.t->family == Family::min) || (d == 1 && b.t->family == Family::maj)))
        {
            r.kind = MoveKind::surprise;
            r.score = 7;
            f.reason = MoveReason::deceptive;
            f.expected = music::onLetter (from.root.letter + 3, pcOf (from.root) + 5);
            for (const auto& [id, l] : p.groups)
                if (id == PaletteGroup::diatonic)
                    for (const auto& e : l)
                        if (pcOf (e.chord.root) == pcOf (f.expected))
                        {
                            f.expectedChord = e.chord;
                            const auto exp = infoOf (key, e.chord, p);
                            for (const auto& x : b.tones)
                            {
                                bool kept = false;
                                for (const auto& y : exp.tones) if (y.pc == x.pc) kept = true;
                                if (kept) f.kept.push_back (x.note);
                            }
                            break;
                        }
        }
        else if (f.toHome && d == 7 && a.fn == F::away)
        {
            r.kind = MoveKind::resolve;
            r.score = 7.5;
            f.reason = MoveReason::plagal;
            const auto* m3 = toneOf (a, "b3");
            const auto* b5 = toneOf (b, "5");
            if (m3 && b5 && mod12 (m3->pc - b5->pc) == 1) { f.minorThird = m3->note; f.nextFifth = b5->note; }
        }
        else if (f.toHome && d == 2 && a.t->family != Family::min && mod12 (pcOf (from.root) - pcOf (key.tonic)) == 10)
        {
            r.kind = MoveKind::resolve;
            r.score = 7;
            f.reason = MoveReason::backdoor;
        }
        else
        {
            f.reason = MoveReason::motion;
            struct Motion { double score; MoveKind kind; };
            static const Motion motions[12] {
                { 1.5, MoveKind::colour }, { 2.5, MoveKind::step }, { 4, MoveKind::step }, { 3, MoveKind::smooth },
                { 3, MoveKind::smooth }, { 6, MoveKind::strong }, { 1, MoveKind::leap }, { 5, MoveKind::strong },
                { 4.6, MoveKind::smooth }, { 4.6, MoveKind::smooth }, { 3.2, MoveKind::step }, { 2.5, MoveKind::step } };
            r.score = motions[d].score;
            r.kind = motions[d].kind;
            auto flow = [] (Function x, Function y) -> std::optional<double>
            {
                if (x == F::home && y == F::away) return 1.5;
                if (x == F::home && y == F::tension) return 1;
                if (x == F::away && y == F::tension) return 2;
                if (x == F::tension && y == F::home) return 1.5;
                if (x == F::away && y == F::home) return 0.5;
                if (x == F::tension && y == F::away) return -1;
                if (x == F::away && y == F::away) return 0.5;
                return 0.0;
            };
            if (const auto fl = flow (a.fn, b.fn)) r.score += *fl;
            if (a.dominant) r.score -= 2.5;
        }
        r.score += groupScore (b.entry);
        if (b.entry && ! b.entry->target.empty() && r.kind != MoveKind::resolve) f.wantsNext = numeral (key, b.entry->target);
        return r;
    }

    // A place to go next, from the map of the key
    struct NextMove { Relation relation; PaletteEntry entry; };

    // The best places to go from a chord in a key: the map's chords (with V7 and the
    // leading-tone 7ths from the 7ths group), at most two on any one root, best first
    inline std::vector<NextMove> nextMoves (const Key& key, const Chord& from, int limit = 8, const KeyPalette* palette = nullptr)
    {
        const KeyPalette own = palette ? KeyPalette {} : keyPalette (key);
        const KeyPalette& p = palette ? *palette : own;
        std::vector<NextMove> all;
        std::vector<std::string> seen;
        for (const auto& e : p.all)
        {
            if (e.group == PaletteGroup::sevenths && ! (e.rn == "V7" || e.rn == "vii\302\2607" || e.rn == "vii\303\2707" || e.rn == "bVII7")) continue;
            if (sameChord (e.chord, from)) continue;
            const auto s = chordSymbol (e.chord);
            if (detail::contains (seen, s)) continue;
            seen.push_back (s);
            all.push_back ({ relate (key, from, e.chord, &p), e });
        }
        std::stable_sort (all.begin(), all.end(), [] (const NextMove& x, const NextMove& y) { return lessScore (y.relation.score, x.relation.score); });
        std::vector<NextMove> out;
        int perRoot[12] {};
        for (const auto& m : all)
        {
            int& n = perRoot[pcOf (m.entry.chord.root)];
            if (n >= 2) continue;
            ++n;
            out.push_back (m);
            if ((int) out.size() >= limit) break;
        }
        return out;
    }

    // ---- transposing

    // The spelled interval from one key's tonic to another's
    inline Interval keyInterval (const Key& from, const Key& to) { return intervalBetween (from.tonic, to.tonic); }

    // A chord moved by a spelled interval (spellings follow, so V7/V stays a V7/V)
    inline Chord transposeChord (const Chord& c, Interval iv)
    {
        auto fix = [&] (Note n) { return std::abs (n.acc) > 1 ? spellRoot (pcOf (n), c.type) : n; };
        const Note root = fix (moveBy (c.root, iv));
        return chord (root, c.type, c.bass ? std::optional<Note> (fix (moveBy (*c.bass, iv))) : std::nullopt);
    }

    // The key a number of semitones up (or down), spelled the usual way for its mode
    // (where there's a choice, on the side the key was on)
    inline Key keyBySemis (const Key& key, int semis)
    {
        const int pc = mod12 (pcOf (key.tonic) + semis);
        std::vector<Note> options;
        for (const char* s : key.mode == Mode::minor ? music::minorKeys : music::majorKeys)
            if (const auto n = music::parseNote (s); n && pcOf (*n) == pc) options.push_back (*n);
        std::stable_sort (options.begin(), options.end(), [&] (Note a, Note b)
        {
            return std::abs (music::signature ({ a, key.mode })) < std::abs (music::signature ({ b, key.mode }));
        });
        auto sign = [] (int v) { return (v > 0) - (v < 0); };
        for (auto n : options) if (sign (music::signature ({ n, key.mode })) == sign (music::signature (key))) return { n, key.mode };
        return { options[0], key.mode };
    }
}
