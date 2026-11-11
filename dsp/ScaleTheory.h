#pragma once

// Scales as players use them: a library scale (dsp/ScaleLibrary.h) spelled
// on any root, its steps, how two scales differ, modes (the same notes from a
// different home) and a mode's parent, the chords a scale builds, which scale
// fits a chord or a whole progression, and where a scale sits on a fretted
// neck (every note, box positions, notes-per-string patterns). Plain C++20,
// no JUCE.
//
// It returns notes, degrees, scores and the facts behind a choice; the
// sentences a product shows about them are the product's.

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "Harmony.h"
#include "ScaleLibrary.h"
#include "Voicings.h"

namespace vs::scaletheory
{
    using harmony::Chord;
    using harmony::Degree;
    using music::Note;
    using music::mod12;
    using music::mod7;
    using music::pcOf;
    using scales::LibraryScale;

    inline const LibraryScale& scaleAt (int i) { return scales::library[(size_t) i]; }
    inline int scaleIndex (std::string_view id) { for (size_t i = 0; i < scales::library.size(); ++i) if (id == scales::library[i].id) return (int) i; return -1; }
    inline Degree degreeOf (const scales::Degree& g) { return { g.d, g.n, g.step, g.acc, g.semis }; }
    inline int sizeOf (const LibraryScale& s) { return (int) s.tones.size(); }

    // ---- spelling

    // One note of a scale on a spelled root: its degree, note, pitch class and semitones above the root
    struct ScaleNote { std::string d; Note note; int pc = 0; int semis = 0; };

    namespace detail
    {
        inline int accCost (Note n) { return std::abs (n.acc) + (std::abs (n.acc) > 1 ? 4 : 0); }
        // Cb, Fb, E#, B# and double accidentals: names a player doesn't expect
        inline bool odd (Note n) { return std::abs (n.acc) > 1 || (n.acc == 1 && (n.letter == 2 || n.letter == 6)) || (n.acc == -1 && (n.letter == 0 || n.letter == 3)); }
        template <typename T> inline bool has (const std::vector<T>& v, const T& x) { return std::find (v.begin(), v.end(), x) != v.end(); }
    }

    // A scale's notes on a spelled root. A 7-note scale gets one letter per degree (G#
    // harmonic minor has an F##, as theory books write it); others keep their degree
    // spelling except that a double sharp or flat is written plainly (Eb blues has A,
    // not Bbb), and a symmetric scale is spelled the plainest way throughout.
    inline std::vector<ScaleNote> spellScale (const LibraryScale& scale, Note root)
    {
        std::vector<ScaleNote> out;
        for (const auto& t : scale.tones)
        {
            Note n = harmony::above (root, degreeOf (t));
            if (! scale.hepta && (std::abs (n.acc) > 1 || (scale.sym && detail::odd (n)))) n = music::plainNote (pcOf (n));
            out.push_back ({ t.d, n, pcOf (n), mod12 (t.semis) });
        }
        return out;
    }

    // How hard a scale is to read on this root: its accidentals, double ones counting heavily
    inline int readCost (const LibraryScale& scale, Note root)
    {
        int s = 0;
        for (const auto& x : spellScale (scale, root)) s += detail::accCost (x.note);
        return s;
    }

    // The easiest spelled root for a scale on a pitch class
    inline Note easyRoot (int pc, const LibraryScale& scale)
    {
        Note best = music::plainNote (pc);
        double bestCost = 1e9;
        for (auto r : music::spellings (pc))
        {
            const double c = readCost (scale, r) + (detail::odd (r) ? 3 : 0) + (r.acc != music::plainNote (pc).acc ? 0.1 : 0);
            if (c < bestCost) { best = r; bestCost = c; }
        }
        return best;
    }

    // The other name of the root when the scale reads much more easily from it, else nothing
    inline std::optional<Note> simplerRoot (const LibraryScale& scale, Note root)
    {
        const Note alt = easyRoot (pcOf (root), scale);
        if (alt == root) return {};
        if (readCost (scale, alt) + 2 <= readCost (scale, root)) return alt;
        return {};
    }

    // ---- steps

    // The steps between neighbouring notes and back up to the octave, in half steps
    inline std::vector<int> stepSizes (const LibraryScale& scale)
    {
        std::vector<int> out;
        const auto& s = scale.semis;
        for (size_t i = 0; i < s.size(); ++i) out.push_back (i + 1 < s.size() ? s[i + 1] - s[i] : 12 - s[i]);
        return out;
    }

    // Steps as a player writes them: H, W, W+H, 2W, 2W+H (else "nH")
    inline std::vector<std::string> stepText (const LibraryScale& scale)
    {
        std::vector<std::string> out;
        for (int n : stepSizes (scale))
            out.push_back (n == 1 ? "H" : n == 2 ? "W" : n == 3 ? "W+H" : n == 4 ? "2W" : n == 5 ? "2W+H" : std::to_string (n) + "H");
        return out;
    }

    // ---- comparing two scales on the same root

    // What changes from scale b to scale a: pairs [a's degree, b's degree] for notes that
    // moved or were traded, degrees only a has, and degrees only b has
    struct Comparison { std::vector<std::pair<std::string, std::string>> swaps; std::vector<std::string> added, removed; };

    inline Comparison compareScales (const LibraryScale& a, const LibraryScale& b)
    {
        Comparison c;
        if (a.hepta && b.hepta)
        {
            for (int step = 0; step < 7; ++step)
            {
                const scales::Degree* x = nullptr;
                const scales::Degree* y = nullptr;
                for (const auto& g : a.tones) if (g.step == step) { x = &g; break; }
                for (const auto& g : b.tones) if (g.step == step) { y = &g; break; }
                if (mod12 (x->semis) != mod12 (y->semis)) c.swaps.push_back ({ x->d, y->d });
            }
            return c;
        }
        auto inSemis = [] (const LibraryScale& s, int v) { for (int x : s.semis) if (x == v) return true; return false; };
        for (const auto& g : a.tones) if (! inSemis (b, mod12 (g.semis))) c.added.push_back (g.d);
        for (const auto& g : b.tones) if (! inSemis (a, mod12 (g.semis))) c.removed.push_back (g.d);
        auto plainDeg = [] (const std::string& d)   // one leading bb, b or #, as the comparison reads a degree
        {
            for (std::string_view p : { "bb", "b", "#" }) if (d.starts_with (p)) return d.substr (p.size());
            return d;
        };
        for (int i = (int) c.added.size() - 1; i >= 0; --i)
        {
            for (size_t j = 0; j < c.removed.size(); ++j)
                if (plainDeg (c.removed[j]) == plainDeg (c.added[(size_t) i]))
                {
                    c.swaps.push_back ({ c.added[(size_t) i], c.removed[j] });
                    c.added.erase (c.added.begin() + i);
                    c.removed.erase (c.removed.begin() + (long) j);
                    break;
                }
        }
        if (c.added.size() == 1 && c.removed.size() == 1)
        {
            c.swaps.push_back ({ c.added.back(), c.removed.back() });
            c.added.clear();
            c.removed.clear();
        }
        return c;
    }

    // ---- modes

    // The library scales with exactly these intervals from the root (any order)
    inline std::vector<int> scalesWithSemis (std::vector<int> semis)
    {
        std::sort (semis.begin(), semis.end());
        std::vector<int> out;
        for (size_t i = 0; i < scales::library.size(); ++i)
        {
            std::vector<int> s (scales::library[i].semis.begin(), scales::library[i].semis.end());
            std::sort (s.begin(), s.end());
            if (s == semis) out.push_back ((int) i);
        }
        return out;
    }

    // Which family a scale is a mode of (an index into scales::families), which mode (1-based) and its mode-1 scale
    struct FamilyOf { int family = -1; int mode = 0; int parent = -1; };
    inline std::optional<FamilyOf> familyOf (const LibraryScale& scale)
    {
        for (size_t f = 0; f < scales::families.size(); ++f)
        {
            const auto& modes = scales::families[f].modes;
            for (size_t k = 0; k < modes.size(); ++k)
                if (modes[k] && std::string_view (modes[k]) == scale.id) return FamilyOf { (int) f, (int) k + 1, scaleIndex (modes[0]) };
        }
        return {};
    }

    // The scale started from each of its notes: the root it starts on, which degree, and the library scales it matches
    struct Rotation { Note root; std::string deg; int k = 0; std::vector<int> scales; };
    inline std::vector<Rotation> rotations (const LibraryScale& scale, Note root)
    {
        const auto notes = spellScale (scale, root);
        std::vector<Rotation> out;
        for (size_t k = 0; k < notes.size(); ++k)
        {
            std::vector<int> semis;
            for (int s : scale.semis) semis.push_back (mod12 (s - scale.semis[k]));
            out.push_back ({ notes[k].note, notes[k].d, (int) k, scalesWithSemis (semis) });
        }
        return out;
    }

    // A mode's parent: its family's mode-1 scale on the root that shares its notes (C major for D Dorian)
    struct Parent { int scale = -1; Note root; int mode = 0; int family = -1; };
    inline std::optional<Parent> parentOf (const LibraryScale& scale, Note root)
    {
        const auto fam = familyOf (scale);
        if (! fam) return {};
        const auto& parent = scaleAt (fam->parent);
        const auto& g = parent.tones[(size_t) fam->mode - 1];
        return Parent { fam->parent, music::onLetter (root.letter - g.step, pcOf (root) - g.semis), fam->mode, fam->family };
    }

    // Every mode of the scale's family on its parent's notes, in mode order
    struct FamilyMode { int scale = -1; Note root; int mode = 0; bool current = false; };
    inline std::optional<std::vector<FamilyMode>> familyModes (const LibraryScale& scale, Note root)
    {
        const auto p = parentOf (scale, root);
        if (! p) return {};
        const auto pn = spellScale (scaleAt (p->scale), p->root);
        std::vector<FamilyMode> out;
        const auto& modes = scales::families[(size_t) p->family].modes;
        for (size_t k = 0; k < modes.size(); ++k)
        {
            if (! modes[k]) continue;
            out.push_back ({ scaleIndex (modes[k]), pn[k].note, (int) k + 1, std::string_view (modes[k]) == scale.id });
        }
        return out;
    }

    // ---- chords from a scale

    // The degree of note b above note a, as a chord formula writes it (b3, #5, bb7)
    inline std::string intervalDeg (Note a, Note b)
    {
        const int step = mod7 (b.letter - a.letter), want = music::majorSemis[(size_t) step];
        int alt = mod12 (pcOf (b) - pcOf (a)) - want;
        if (alt > 6) alt -= 12;
        if (alt < -6) alt += 12;
        return std::string ((size_t) std::abs (alt), alt > 0 ? '#' : 'b') + std::to_string (step + 1);
    }

    // The chord type whose notes are exactly these intervals from the root (-1 when none has a common name)
    inline int typeForSemis (const std::vector<int>& semis)
    {
        std::vector<int> want;
        for (int s : semis) if (! detail::has (want, mod12 (s))) want.push_back (mod12 (s));
        std::sort (want.begin(), want.end());
        int best = -1;
        for (size_t i = 0; i < chords::types.size(); ++i)
        {
            const auto& t = chords::types[i];
            if (t.tones.size() != semis.size()) continue;
            std::vector<int> got;
            for (const auto& g : t.tones) if (! detail::has (got, mod12 (g.semis))) got.push_back (mod12 (g.semis));
            std::sort (got.begin(), got.end());
            if (got != want) continue;
            if (best < 0 || t.cost < chords::types[(size_t) best].cost) best = (int) i;
        }
        return best;
    }

    // A chord stacked from a scale: its notes (spelled as the scale spells them), its
    // type (-1 for a stack with no common name), symbol and numeral (against the major
    // scale on the scale's root; an odd stack's ends in "?")
    struct StackedChord { std::vector<Note> notes; int type = -1; Chord chord; std::string symbol, numeral; };
    struct ScaleChord { std::string deg; Note root; StackedChord triad, seventh; };

    // The triad and 7th chord a 7-note scale builds on each degree (stacking every other note)
    inline std::vector<ScaleChord> scaleChords (const LibraryScale& scale, Note root)
    {
        if (! scale.hepta) return {};
        const auto notes = spellScale (scale, root);
        const music::Key key { root, music::Mode::major };
        std::vector<ScaleChord> out;
        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& x = notes[i];
            auto build = [&] (int n)
            {
                StackedChord sc;
                std::vector<int> semis;
                for (int k : { 0, 2, 4, 6 })
                {
                    if ((int) sc.notes.size() >= n) break;
                    sc.notes.push_back (notes[(i + (size_t) k) % 7].note);
                    semis.push_back (mod12 (pcOf (sc.notes.back()) - pcOf (x.note)));
                }
                sc.type = typeForSemis (semis);
                if (sc.type < 0)
                {
                    const int step = x.note.letter - root.letter;
                    const int alt = mod12 (pcOf (x.note) - pcOf (harmony::above (root, std::to_string (mod7 (step) + 1))));
                    const std::string acc = alt == 0 ? "" : alt <= 6 ? std::string ((size_t) alt, '#') : std::string ((size_t) (12 - alt), 'b');
                    sc.symbol = music::noteText (x.note) + "(";
                    for (size_t k = 1; k < sc.notes.size(); ++k) sc.symbol += (k > 1 ? " " : "") + intervalDeg (x.note, sc.notes[k]);
                    sc.symbol += ")";
                    sc.numeral = acc + harmony::detail::roman[mod7 (step)] + "?";
                    return sc;
                }
                sc.chord = harmony::chord (x.note, sc.type);
                sc.symbol = music::noteText (x.note) + chords::types[(size_t) sc.type].sym;
                sc.numeral = harmony::plainNumeral (key, sc.chord);
                return sc;
            };
            out.push_back ({ x.d, x.note, build (3), build (4) });
        }
        return out;
    }

    // A common chord whose every note is in a scale, rooted on one of its notes
    struct InsideChord { std::string deg; Note root; int type = 0; Chord chord; std::vector<Note> notes; std::string symbol, numeral; };

    inline std::vector<InsideChord> chordsInside (const LibraryScale& scale, Note root)
    {
        static const char* const inside[] { "maj", "min", "dim", "aug", "sus2", "sus4", "5", "6", "m6", "maj7", "7", "m7", "m7b5", "dim7", "mMaj7", "7sus4", "add9", "madd9", "7#5" };
        const auto notes = spellScale (scale, root);
        std::vector<int> pcs;
        for (const auto& n : notes) pcs.push_back (n.pc);
        const music::Key key { root, music::Mode::major };
        std::vector<InsideChord> out;
        for (const auto& x : notes)
            for (const char* id : inside)
            {
                const int t = harmony::typeIndex (id);
                const Chord c = harmony::chord (x.note, t);
                const auto tones = harmony::chordTones (c);
                bool all = true;
                for (const auto& tn : tones) if (! detail::has (pcs, tn.pc)) all = false;
                if (! all) continue;
                InsideChord ic { x.d, x.note, t, c, {}, music::noteText (x.note) + chords::types[(size_t) t].sym, harmony::plainNumeral (key, c) };
                for (const auto& tn : tones) for (const auto& n : notes) if (n.pc == tn.pc) { ic.notes.push_back (n.note); break; }
                out.push_back (ic);
            }
        return out;
    }

    // ---- which scale fits a chord or a progression

    // A chord as the fit finder reads it: what was typed, its root, the tones it needs
    // and may leave out, its family, its bass, and whether it's an altered dominant (7alt)
    struct FitChord
    {
        std::string text;
        Note root;
        bool alt = false;
        std::optional<Chord> chord;
        chords::Family family = chords::Family::maj;
        std::vector<Degree> need, may;
        std::optional<Note> bass;
        std::string symbol;
    };

    // Reads a chord for the fit finder: any chord symbol, plus "C7alt" or "Calt" (an altered dominant)
    inline std::optional<FitChord> readChord (std::string_view s)
    {
        const auto t = harmony::detail::trim (s);
        // a letter A to G, then ##, bb, # or b or nothing (tried in that order), then "7alt" or "alt"
        size_t rootLen = 0;
        if (! t.empty() && t[0] >= 'A' && t[0] <= 'G')
            for (std::string_view acc : { "##", "bb", "#", "b", "" })
                if (t.substr (1).starts_with (acc))
                {
                    const auto rest = t.substr (1 + acc.size());
                    if (rest == "7alt" || rest == "alt") { rootLen = 1 + acc.size(); break; }
                }
        if (rootLen > 0)
        {
            {
                const auto root = music::parseNote (t.substr (0, rootLen));
                if (root)
                {
                    FitChord c;
                    c.text = std::string (t);
                    c.root = *root;
                    c.alt = true;
                    c.family = chords::Family::dom;
                    for (const char* d : { "1", "3", "b7", "b9", "#9", "b13" }) c.need.push_back (*harmony::degree (d));
                    c.may.push_back (*harmony::degree ("#11"));
                    c.symbol = music::noteText (*root) + "7alt";
                    return c;
                }
            }
        }
        const auto ch = harmony::parseChord (t);
        if (! ch) return {};
        const auto& type = harmony::typeOf (*ch);
        FitChord c;
        c.text = std::string (t);
        c.root = ch->root;
        c.chord = ch;
        c.family = type.family;
        for (const auto& g : type.tones) (harmony::detail::isOpt (type, g.d) ? c.may : c.need).push_back (harmony::degreeOf (g));
        c.bass = ch->bass;
        c.symbol = std::string (t);
        return c;
    }

    // Splits "Am7 D7 | G" or "Am7, D7, G" into chords; the bits it can't read come back in bad
    struct ReadProgression { std::vector<FitChord> chords; std::vector<std::string> bad; };
    inline ReadProgression readProgression (std::string_view text)
    {
        ReadProgression out;
        std::string part;
        auto flush = [&]
        {
            if (part.empty()) return;
            if (auto c = readChord (part)) out.chords.push_back (*c); else out.bad.push_back (part);
            part.clear();
        };
        for (char ch : text)
        {
            const bool sep = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f' || ch == ',' || ch == '|';
            if (sep) flush(); else part += ch;
        }
        flush();
        return out;
    }

    // How a scale on a root sits over one chord: the chord tones it misses (needed and
    // optional), its avoid notes (a half step above a chord tone; over a dominant chord
    // the b9 and b13 are wanted tensions instead), its colour notes and all its notes
    struct Fit { std::vector<Degree> missNeed, missMay; std::vector<ScaleNote> avoid, colour, notes; };

    inline Fit fitOver (const LibraryScale& scale, Note root, const FitChord& c)
    {
        Fit f;
        f.notes = spellScale (scale, root);
        std::vector<int> pcs;
        for (const auto& n : f.notes) pcs.push_back (n.pc);
        std::vector<int> chordPcs;
        for (const auto& g : c.need) chordPcs.push_back (mod12 (pcOf (c.root) + g.semis));
        for (const auto& g : c.may) chordPcs.push_back (mod12 (pcOf (c.root) + g.semis));
        if (c.bass) chordPcs.push_back (pcOf (*c.bass));
        for (const auto& g : c.need) if (! detail::has (pcs, mod12 (pcOf (c.root) + g.semis))) f.missNeed.push_back (g);
        for (const auto& g : c.may) if (! detail::has (pcs, mod12 (pcOf (c.root) + g.semis))) f.missMay.push_back (g);
        for (const auto& n : f.notes)
        {
            if (detail::has (chordPcs, n.pc)) continue;
            const int below = mod12 (n.pc - 1), rel = mod12 (n.pc - pcOf (c.root));
            const bool dominantTension = c.family == chords::Family::dom && (rel == 1 || rel == 8);
            if (detail::has (chordPcs, below) && ! dominantTension) f.avoid.push_back (n); else f.colour.push_back (n);
        }
        return f;
    }

    // The notes of the rest of a progression, for ranking scales over one of its chords
    struct Context { std::vector<int> pcs; };

    // A ranked scale over one chord (lower score is better); inKey: it uses the same notes as the rest of the progression
    struct ScaleChoice { int scale = -1; Note root; double score = 0; Fit fit; bool inKey = false; };

    namespace detail
    {
        inline bool sameSet (const std::vector<int>& a, const std::vector<int>& b)
        {
            std::vector<int> ub;
            for (int x : b) if (! has (ub, x)) ub.push_back (x);
            if (a.size() != ub.size()) return false;
            for (int p : a) if (! has (b, p)) return false;
            return true;
        }

        inline const char* defaultFor (chords::Family f)
        {
            switch (f)
            {
                case chords::Family::maj:   return "ionian";
                case chords::Family::dom:   return "mixolydian";
                case chords::Family::min:   return "dorian";
                case chords::Family::hdim:  return "locrian";
                case chords::Family::dim:   return "dim_wh";
                case chords::Family::aug:   return "whole_tone";
                case chords::Family::sus:   return "mixolydian";
                case chords::Family::power: return "minor_pent";
            }
            return "";
        }

        inline const char* defaultForType (std::string_view id)
        {
            static const std::map<std::string, const char*, std::less<>> m {
                { "m6", "melodic_minor" }, { "m69", "melodic_minor" }, { "mMaj7", "melodic_minor" }, { "mMaj9", "melodic_minor" },
                { "7#11", "lydian_dominant" }, { "9#11", "lydian_dominant" }, { "13#11", "lydian_dominant" }, { "maj7#11", "lydian" },
                { "maj9#11", "lydian" }, { "addS11", "lydian" }, { "7#5", "whole_tone" }, { "7b9", "dim_hw" }, { "13b9", "dim_hw" } };
            const auto it = m.find (id);
            return it == m.end() ? nullptr : it->second;
        }

        inline bool altered (int semis) { semis = mod12 (semis); return semis == 1 || semis == 3 || semis == 6 || semis == 8; }

        inline std::vector<int> relPcs (const FitChord& c, const std::vector<Degree>& degrees)
        {
            std::vector<int> out;
            for (const auto& g : degrees) out.push_back (mod12 (pcOf (c.root) + g.semis));
            return out;
        }
    }

    // Ranked scales for one chord, built on the chord's root (as chord-scale theory works). Best first.
    inline std::vector<ScaleChoice> scalesForChord (const FitChord& c, int limit = 8, const Context* context = nullptr)
    {
        std::vector<ScaleChoice> out;
        bool asksAltered = false;
        if (c.family == chords::Family::dom)
        {
            for (const auto& g : c.need) if (detail::altered (g.semis)) asksAltered = true;
            for (const auto& g : c.may) if (detail::altered (g.semis)) asksAltered = true;
        }
        for (size_t i = 0; i < scales::library.size(); ++i)
        {
            const auto& s = scales::library[i];
            if (std::string_view (s.id) == "chromatic") continue;
            const auto f = fitOver (s, c.root, c);
            if (! f.missNeed.empty()) continue;
            double score = s.prior + 0.3 * (double) f.avoid.size() + 1.0 * (double) f.missMay.size();
            const char* byType = c.chord ? detail::defaultForType (harmony::typeOf (*c.chord).id) : nullptr;
            if (std::string_view (byType ? byType : detail::defaultFor (c.family)) == s.id) score -= 0.5;
            if (c.family == chords::Family::dom)
            {
                int alteredColour = 0;
                for (const auto& n : f.colour) if (detail::altered (n.pc - pcOf (c.root))) ++alteredColour;
                score += (asksAltered ? 0.25 : 0.4) * alteredColour;
            }
            std::vector<int> fpcs;
            for (const auto& n : f.notes) fpcs.push_back (n.pc);
            const bool same = context && detail::sameSet (fpcs, context->pcs);
            if (same) score -= 1.6;
            else if (context)
            {
                int outside = 0;
                for (int p : fpcs) if (! detail::has (context->pcs, p)) ++outside;
                score += 0.3 * outside;
            }
            out.push_back ({ (int) i, c.root, score, f, same });
        }
        std::stable_sort (out.begin(), out.end(), [] (const ScaleChoice& a, const ScaleChoice& b) { return a.score < b.score; });
        if ((int) out.size() > limit) out.resize ((size_t) limit);
        return out;
    }

    // A ranked scale over a whole progression (lower score is better): the chord tones it
    // misses (spelled as the chords spell them), its notes, and whether it's one of the blues answers
    struct ProgressionScale { int scale = -1; Note root; double score = 0; std::vector<Note> missing; std::vector<int> pcs; std::vector<ScaleNote> notes; bool blues = false; };

    // Scales for a whole progression (one scale that covers it, its home guessed from the
    // chords), and the best scales for each chord heard in that light. tonic: the pitch
    // class that sounds like home (-1 with no chords). best: the scale the per-chord lists
    // were heard against (-1 for a blues, whose answers are over everything).
    struct ProgressionScales { std::vector<ProgressionScale> overall; std::vector<std::vector<ScaleChoice>> perChord; int tonic = -1; bool blues = false; int best = -1; };

    inline ProgressionScales scalesForProgression (const std::vector<FitChord>& chords, int limit = 6)
    {
        ProgressionScales r;
        if (chords.empty()) return r;
        std::vector<std::pair<int, double>> weight;   // in the order first weighed
        auto add = [&] (int pc, double w)
        {
            for (auto& [p, v] : weight) if (p == pc) { v += w; return; }
            weight.push_back ({ pc, w });
        };
        for (const auto& c : chords) add (pcOf (c.root), 0.3);
        add (pcOf (chords.back().root), 1);
        add (pcOf (chords[0].root), 1.5);
        for (size_t i = 0; i + 1 < chords.size(); ++i)
        {
            const auto& a = chords[i];
            const auto& b = chords[i + 1];
            if (a.family == chords::Family::dom && mod12 (pcOf (a.root) - pcOf (b.root)) == 7 && b.family != chords::Family::dom) add (pcOf (b.root), 3);
        }
        std::vector<int> needAll;
        for (const auto& c : chords) for (int p : detail::relPcs (c, c.need)) if (! detail::has (needAll, p)) needAll.push_back (p);
        auto covers = [&] (const char* id, int pc)
        {
            const auto& s = scaleAt (scaleIndex (id));
            std::vector<int> pcs;
            for (int x : s.semis) pcs.push_back (mod12 (x + pc));
            for (int p : needAll) if (! detail::has (pcs, p)) return false;
            return true;
        };
        for (int pc = 0; pc < 12; ++pc)
        {
            if (covers ("ionian", pc)) add (pc, 1);
            if (covers ("aeolian", pc) || covers ("harmonic_minor", pc)) add (pc, 0.5);
        }
        double maxW = -1e9;
        for (auto& [p, w] : weight) maxW = std::max (maxW, w);
        {
            auto sorted = weight;
            std::stable_sort (sorted.begin(), sorted.end(), [] (const auto& a, const auto& b) { return b.second < a.second; });
            r.tonic = sorted[0].first;
        }
        auto weightOf = [&] (int pc) { for (auto& [p, w] : weight) if (p == pc) return w; return 0.0; };
        std::vector<int> needPcs, mayPcs;
        for (const auto& c : chords)
        {
            for (int p : detail::relPcs (c, c.need)) if (! detail::has (needPcs, p)) needPcs.push_back (p);
            for (int p : detail::relPcs (c, c.may)) if (! detail::has (mayPcs, p)) mayPcs.push_back (p);
            if (c.bass && ! detail::has (needPcs, pcOf (*c.bass))) needPcs.push_back (pcOf (*c.bass));
        }
        std::vector<std::pair<int, Note>> spelled;   // each root as the chords spell it (the last one written wins)
        for (const auto& c : chords)
        {
            bool found = false;
            for (auto& [p, n] : spelled) if (p == pcOf (c.root)) { n = c.root; found = true; }
            if (! found) spelled.push_back ({ pcOf (c.root), c.root });
        }
        std::vector<std::pair<int, Note>> toneName;
        for (const auto& c : chords)
        {
            std::vector<Degree> all = c.need;
            all.insert (all.end(), c.may.begin(), c.may.end());
            for (const auto& g : all)
            {
                const Note n = harmony::above (c.root, g);
                bool found = false;
                for (auto& [p, x] : toneName) if (p == pcOf (n)) found = true;
                if (! found) toneName.push_back ({ pcOf (n), n });
            }
        }
        std::vector<ProgressionScale> cands;
        for (size_t si = 0; si < scales::library.size(); ++si)
        {
            const auto& s = scales::library[si];
            if (std::string_view (s.id) == "chromatic" || std::string_view (s.group) == "bebop") continue;
            for (int pc = 0; pc < 12; ++pc)
            {
                std::optional<Note> sp;
                for (auto& [p, n] : spelled) if (p == pc) sp = n;
                const Note root = sp ? *sp : easyRoot (pc, s);
                const auto notes = spellScale (s, root);
                std::vector<int> pcs;
                for (const auto& n : notes) if (! detail::has (pcs, n.pc)) pcs.push_back (n.pc);
                std::vector<int> missNeed, missMay;
                for (int p : needPcs) if (! detail::has (pcs, p)) missNeed.push_back (p);
                for (int p : mayPcs) if (! detail::has (pcs, p) && ! detail::has (needPcs, p)) missMay.push_back (p);
                if (missNeed.size() > 2) continue;
                const FitChord* homeChord = nullptr;
                for (const auto& c : chords) if (pcOf (c.root) == pc) { homeChord = &c; break; }
                int extra = 0;
                for (const auto& n : notes) if (! detail::has (needPcs, n.pc) && ! detail::has (mayPcs, n.pc)) ++extra;
                double score = s.prior * 0.6 + 2.5 * (double) missNeed.size() + 1.2 * (double) missMay.size() + 0.35 * extra + 0.6 * (maxW - weightOf (pc));
                if (! homeChord) score += 0.8;
                else if (! fitOver (s, root, *homeChord).missNeed.empty()) score += 1.5;
                ProgressionScale ps;
                ps.scale = (int) si;
                ps.root = root;
                ps.score = score;
                for (int p : missNeed) { std::optional<Note> n; for (auto& [q, x] : toneName) if (q == p) n = x; ps.missing.push_back (n ? *n : music::plainNote (p)); }
                for (int p : missMay) { std::optional<Note> n; for (auto& [q, x] : toneName) if (q == p) n = x; ps.missing.push_back (n ? *n : music::plainNote (p)); }
                for (const auto& n : notes) ps.pcs.push_back (n.pc);
                ps.notes = notes;
                cands.push_back (ps);
            }
        }
        std::stable_sort (cands.begin(), cands.end(), [] (const auto& a, const auto& b) { return a.score < b.score; });
        std::vector<std::vector<int>> sets;
        for (const auto& c : cands)
        {
            std::vector<int> set;
            for (int p : c.pcs) if (! detail::has (set, p)) set.push_back (p);
            std::sort (set.begin(), set.end());
            if (detail::has (sets, set)) continue;
            if (! r.overall.empty() && c.score > r.overall[0].score + 3.5) break;
            sets.push_back (set);
            r.overall.push_back (c);
            if ((int) r.overall.size() >= limit) break;
        }
        bool hasIV = false, hasI = false, allDom = chords.size() >= 2;
        for (const auto& c : chords)
        {
            if (c.family != chords::Family::dom) allDom = false;
            if (mod12 (pcOf (c.root) - r.tonic) == 5) hasIV = true;
            if (pcOf (c.root) == r.tonic) hasI = true;
        }
        r.blues = allDom && hasIV && hasI;
        if (r.blues)
        {
            Note home;
            for (const auto& c : chords) if (pcOf (c.root) == r.tonic) { home = c.root; break; }
            std::vector<ProgressionScale> picks;
            int k = 0;
            for (const char* id : { "minor_pent", "blues", "major_pent" })
            {
                ProgressionScale ps;
                ps.scale = scaleIndex (id);
                ps.root = home;
                ps.score = -3 + k * 0.1;
                ps.blues = true;
                ps.notes = spellScale (scaleAt (ps.scale), home);
                for (const auto& n : ps.notes) ps.pcs.push_back (n.pc);
                picks.push_back (ps);
                ++k;
            }
            r.overall.insert (r.overall.begin(), picks.begin(), picks.end());
            if ((int) r.overall.size() > limit) r.overall.resize ((size_t) limit);
        }
        Context context;
        const Context* ctx = nullptr;
        if (! r.blues && ! r.overall.empty())
        {
            int best = 0;
            for (size_t i = 0; i < r.overall.size(); ++i) if (r.overall[i].missing.empty()) { best = (int) i; break; }
            r.best = best;
            context.pcs = r.overall[(size_t) best].pcs;
            ctx = &context;
        }
        for (const auto& c : chords) r.perChord.push_back (scalesForChord (c, 4, ctx));
        return r;
    }

    // ---- a scale on a fretted neck

    // A scale note at a place on the neck: which course (low side first), the fret
    // (from the capo), its MIDI note, pitch class, degree and spelling
    struct NeckNote { int course = 0, fret = 0, midi = 0, pc = 0; std::string d; Note note; };

    // A box position or a per-string pattern: its notes low to high and the frets it spans.
    // A box also has the fret its window starts at and the CAGED shape it sits on (the
    // letter E, G, A, C or D, or "" when none or not a standard-tuned 6-string guitar);
    // a per-string pattern has the degree it starts on and how many notes per string.
    struct NeckPattern { int lo = 0, hi = 0; std::vector<NeckNote> notes; int s = 0; std::string shape, startDeg; int perString = 0; };

    namespace detail
    {
        inline std::optional<int> pitchAt (const voicings::Course& course, int fret, int capo)
        {
            const auto m = voicings::courseNotes (course, fret, capo);
            if (! m) return {};
            return (*m)[0];
        }

        // the scale note on a pitch class (the last one, when two share it)
        inline const ScaleNote* byPc (const std::vector<ScaleNote>& notes, int pc)
        {
            for (auto it = notes.rbegin(); it != notes.rend(); ++it) if (it->pc == pc) return &*it;
            return nullptr;
        }
    }

    // Each course higher than the one before, and none short (a guitar; not a ukulele with a high G, or a banjo)
    inline bool ascendingNeck (const std::vector<voicings::Course>& courses)
    {
        for (size_t i = 1; i < courses.size(); ++i) if (! (courses[i].open > courses[i - 1].open && ! courses[i].start)) return false;
        return true;
    }

    // Every scale note on the neck; frets: the highest fret counted from the nut
    inline std::vector<NeckNote> neckNotes (const LibraryScale& scale, Note root, const std::vector<voicings::Course>& courses, int capo = 0, int frets = 15)
    {
        const auto notes = spellScale (scale, root);
        std::vector<NeckNote> out;
        for (size_t i = 0; i < courses.size(); ++i)
            for (int f = 0; f + capo <= frets; ++f)
            {
                const auto m = detail::pitchAt (courses[i], f, capo);
                if (! m) continue;
                if (const auto* n = detail::byPc (notes, mod12 (*m))) out.push_back ({ (int) i, f, *m, n->pc, n->d, n->note });
            }
        return out;
    }

    // True for a 6-string guitar's standard E A D G B E (at any pitch: Eb standard counts)
    inline bool isStandardSix (const std::vector<voicings::Course>& courses)
    {
        if (courses.size() != 6) return false;
        const int want[5] { 5, 5, 5, 4, 5 };
        for (size_t i = 1; i < 6; ++i) if (courses[i].open - courses[i - 1].open != want[i - 1]) return false;
        return true;
    }

    namespace detail
    {
        struct Placed { int course, fret, midi; };

        inline NeckNote dress (const Placed& x, const std::vector<ScaleNote>& notes)
        {
            const auto* n = byPc (notes, mod12 (x.midi));
            return { x.course, x.fret, x.midi, n->pc, n->d, n->note };
        }

        // which CAGED chord shape of the parent major key a box sits on: where that key's root falls in it
        inline std::string cagedShape (const LibraryScale& scale, Note root, int s, const std::vector<voicings::Course>& courses, int capo)
        {
            std::optional<int> home;
            const auto par = parentOf (scale, root);
            const std::string_view id = scale.id;
            if (par && std::string_view (scales::families[(size_t) par->family].id) == "major") home = pcOf (par->root);
            else if (id == "major_pent" || id == "ionian") home = pcOf (root);
            else if (id == "minor_pent" || id == "blues") home = mod12 (pcOf (root) + 3);
            if (! home) return "";
            auto at = [&] (int i, int f) { const auto m = pitchAt (courses[(size_t) i], f, capo); return m && mod12 (*m) == *home; };
            if (at (0, s + 1)) return "E";
            if (at (0, s + 3)) return "G";
            if (at (1, s + 1)) return "A";
            if (at (1, s + 3)) return "C";
            if (at (2, s + 1)) return "D";
            return "";
        }
    }

    // Box positions: the scale inside a few frets, low string to high, each pitch once.
    // For each scale note on the lowest string a window starts at that fret or one below
    // (whichever needs fewer stretches), as wide as a hand position plus one stretch up
    // to fill a gap before the next string. standard: name the CAGED shapes.
    inline std::vector<NeckPattern> boxPositions (const LibraryScale& scale, Note root, const std::vector<voicings::Course>& courses, int capo = 0, int frets = 15, bool standard = false)
    {
        using detail::Placed;
        const int n = (int) courses.size();
        if (n < 2) return {};
        const bool asc = ascendingNeck (courses);
        const auto notes = spellScale (scale, root);
        const int maxFret = frets - capo;
        std::vector<int> gaps;
        for (int i = 1; i < n; ++i) { const int g = courses[(size_t) i].open - courses[(size_t) i - 1].open; if (g > 0) gaps.push_back (g); }
        int usual = 5;
        if (! gaps.empty()) { auto s = gaps; std::sort (s.begin(), s.end()); usual = s[s.size() >> 1]; }
        const int core = std::max (4, std::min (7, usual)) - 2;
        int anchor = -1;
        for (int i = 0; i < n; ++i)
            if (! courses[(size_t) i].start && (anchor < 0 || courses[(size_t) i].open < courses[(size_t) anchor].open)) anchor = i;
        if (anchor < 0) return {};
        struct Built { int s; std::vector<Placed> picked; int cost; };
        auto build = [&] (int s)
        {
            Built b { s, {}, 0 };
            int last = -1000000000, stretches = 0;
            for (int i = 0; i < n; ++i)
            {
                const auto& c = courses[(size_t) i];
                std::vector<Placed> here;
                for (int f = std::max (0, s); f <= std::min (maxFret, s + core); ++f)
                {
                    const auto m = detail::pitchAt (c, f, capo);
                    if (! m || ! detail::byPc (notes, mod12 (*m))) continue;
                    if (asc && *m <= last) continue;
                    here.push_back ({ i, f, *m });
                }
                const int f = s + core + 1;
                if (asc && f <= maxFret)
                {
                    const auto m = detail::pitchAt (c, f, capo);
                    const bool hasNext = i + 1 < n;
                    const auto nextOpen = hasNext ? detail::pitchAt (courses[(size_t) i + 1], 0, capo) : std::optional<int> {};
                    const int reach = hasNext && m && nextOpen ? *m - *nextOpen : -1;
                    const bool nextReach = reach >= std::max (0, s) && reach <= s + core;
                    if (m && detail::byPc (notes, mod12 (*m)) && *m > last && ! nextReach)
                    {
                        here.push_back ({ i, f, *m });
                        if (hasNext) ++stretches;
                    }
                }
                for (const auto& x : here) { b.picked.push_back (x); last = std::max (last, x.midi); }
            }
            int uneven = 0;
            for (int i = 0; i < n; ++i)
            {
                int k = 0;
                for (const auto& x : b.picked) if (x.course == i) ++k;
                uneven += k == 0 ? 3 : k > 3 ? k - 3 : 0;
            }
            b.cost = stretches + uneven + (s < 0 ? 9 : 0);
            return b;
        };
        std::vector<NeckPattern> out;
        std::vector<std::string> seen;
        std::vector<int> starts;
        for (int f = 0; f <= std::min (11, maxFret - core); ++f)
        {
            const auto m = detail::pitchAt (courses[(size_t) anchor], f, capo);
            if (m && detail::byPc (notes, mod12 (*m))) starts.push_back (f);
        }
        for (int a : starts)
        {
            std::vector<Built> opts;
            if (a > 0) opts.push_back (build (a - 1));
            opts.push_back (build (a));
            std::stable_sort (opts.begin(), opts.end(), [] (const Built& x, const Built& y) { return x.cost < y.cost; });
            const auto& b = opts[0];
            if (b.picked.empty()) continue;
            std::string key;
            for (size_t k = 0; k < b.picked.size(); ++k) key += (k ? "," : "") + std::to_string (b.picked[k].course) + ":" + std::to_string (b.picked[k].fret);
            if (harmony::detail::contains (seen, key)) continue;
            seen.push_back (key);
            NeckPattern p;
            p.lo = b.picked[0].fret;
            p.hi = b.picked[0].fret;
            for (const auto& x : b.picked) { p.lo = std::min (p.lo, x.fret); p.hi = std::max (p.hi, x.fret); p.notes.push_back (detail::dress (x, notes)); }
            p.s = b.s;
            out.push_back (p);
        }
        std::stable_sort (out.begin(), out.end(), [] (const NeckPattern& a, const NeckPattern& b) { return a.lo < b.lo; });
        for (auto& p : out) p.shape = standard ? detail::cagedShape (scale, root, p.s, courses, capo) : "";
        return out;
    }

    // Three notes per string (two for a scale of five or fewer): from each scale note on
    // the lowest string, the same number on every string, climbing the scale. Only for
    // tunings where each string is higher than the last.
    inline std::vector<NeckPattern> perStringPatterns (const LibraryScale& scale, Note root, const std::vector<voicings::Course>& courses, int capo = 0, int frets = 15)
    {
        using detail::Placed;
        const int n = (int) courses.size();
        if (n < 3 || ! ascendingNeck (courses)) return {};
        const int nps = sizeOf (scale) >= 6 ? 3 : 2;
        const auto notes = spellScale (scale, root);
        const int maxFret = frets - capo;
        const int lowest = *detail::pitchAt (courses[0], 0, capo);
        std::vector<int> ladder;
        for (int m = lowest; m <= lowest + 60; ++m) if (detail::byPc (notes, mod12 (m))) ladder.push_back (m);
        std::vector<NeckPattern> out;
        for (int k = 0; k < sizeOf (scale); ++k)
        {
            int first = -1;
            for (size_t j = 0; j < ladder.size(); ++j) if (mod12 (ladder[j]) == notes[(size_t) k].pc) { first = (int) j; break; }
            for (int start = first; start >= 0 && start < (int) ladder.size(); start += sizeOf (scale))
            {
                std::vector<Placed> picked;
                bool ok = true;
                for (int i = 0; i < n && ok; ++i)
                {
                    const int open = *detail::pitchAt (courses[(size_t) i], 0, capo);
                    for (int j = 0; j < nps; ++j)
                    {
                        const int idx = start + i * nps + j;
                        if (idx >= (int) ladder.size()) { ok = false; break; }
                        const int m = ladder[(size_t) idx], f = m - open;
                        if (f < 0 || f > maxFret) { ok = false; break; }
                        picked.push_back ({ i, f, m });
                    }
                }
                if (! ok) continue;
                bool wide = false;
                for (int i = 0; i < n; ++i)
                {
                    int lo = 1 << 30, hi = -(1 << 30);
                    for (const auto& x : picked) if (x.course == i) { lo = std::min (lo, x.fret); hi = std::max (hi, x.fret); }
                    if (hi - lo > 5) wide = true;
                }
                if (wide) continue;
                NeckPattern p;
                p.lo = picked[0].fret;
                p.hi = picked[0].fret;
                for (const auto& x : picked) { p.lo = std::min (p.lo, x.fret); p.hi = std::max (p.hi, x.fret); }
                if (p.hi - p.lo > 8) continue;
                for (const auto& x : picked) p.notes.push_back (detail::dress (x, notes));
                p.startDeg = notes[(size_t) k].d;
                p.perString = nps;
                out.push_back (p);
                break;
            }
        }
        std::stable_sort (out.begin(), out.end(), [] (const NeckPattern& a, const NeckPattern& b) { return a.lo < b.lo; });
        return out;
    }

    // The notes to play a scale up and down: a pattern's notes, or one octave up from the root near `around`
    struct RunNote { int midi = 0; int course = -1, fret = -1; int pc = 0; std::string d; Note note; };
    inline std::vector<RunNote> upAndDown (const LibraryScale& scale, Note root, const NeckPattern* pattern = nullptr, int around = 52)
    {
        std::vector<RunNote> up;
        if (pattern)
        {
            auto sorted = pattern->notes;
            std::stable_sort (sorted.begin(), sorted.end(), [] (const NeckNote& a, const NeckNote& b) { return a.midi < b.midi; });
            for (const auto& x : sorted) up.push_back ({ x.midi, x.course, x.fret, x.pc, x.d, x.note });
        }
        else
        {
            const auto notes = spellScale (scale, root);
            int base = around + mod12 (pcOf (root) - around);
            if (base - around > 6) base -= 12;
            for (const auto& x : notes) up.push_back ({ base + x.semis, -1, -1, x.pc, x.d, x.note });
            up.push_back ({ base + 12, -1, -1, notes[0].pc, notes[0].d, notes[0].note });
        }
        std::vector<RunNote> out = up;
        for (int i = (int) up.size() - 2; i >= 0; --i) out.push_back (up[(size_t) i]);
        return out;
    }
}
