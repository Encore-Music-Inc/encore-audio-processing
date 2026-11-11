#pragma once

// Where a chord's notes are on an instrument: fretted shapes worked out
// (searched) for any instrument and tuning in the tuner's list (dsp/Tunings.h),
// how many fingers a shape needs, a piano voicing that moves least from the
// chord before, and the capo helper. Plain C++20, no JUCE.

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "Harmony.h"

namespace vs::voicings
{
    using harmony::Chord;
    using music::mod12;
    using music::pcOf;

    // Whether chords are played on it by fretting: a string instrument with at least two
    // strings in one of its tunings, except the plucked or bowed ones played otherwise
    inline bool frettable (std::string_view instrumentName, std::string_view part, const std::vector<std::vector<int>>& tuningStrings)
    {
        static const char* const notFretted[] { "Koto (13-string)", "Kantele (5-string)", "Hurdy-gurdy", "Erhu" };
        if (part != "string") return false;
        for (auto* n : notFretted) if (instrumentName == n) return false;
        for (const auto& t : tuningStrings) if (t.size() >= 2) return true;
        return false;
    }

    // One string, or a pair or group played together: its strings (MIDI), the lowest
    // of them, and the fret it begins at (a banjo's short 5th string begins at fret 5)
    struct Course { std::vector<int> midi; int open = 0; int start = 0; };

    // A tuning as courses: strings (MIDI, low side first) grouped by courseSizes (empty: one string each)
    inline std::vector<Course> coursesOf (const std::vector<int>& strings, const std::vector<int>& courseSizes = {}, std::string_view instrumentName = {})
    {
        std::vector<Course> out;
        std::vector<int> sizes = courseSizes;
        if (sizes.empty()) sizes.assign (strings.size(), 1);
        size_t s = 0;
        for (int size : sizes)
        {
            Course c;
            for (int k = 0; k < size && s + (size_t) k < strings.size(); ++k) c.midi.push_back (strings[s + (size_t) k]);
            c.open = c.midi.empty() ? 0 : *std::min_element (c.midi.begin(), c.midi.end());
            out.push_back (c);
            s += (size_t) size;
        }
        if (instrumentName.substr (0, 16) == "Banjo (5-string)" && out.size() == 5) out[0].start = 5;
        return out;
    }

    // The notes a course sounds at a fret (counted from the capo; 0 open or at the capo;
    // -1 muted: nothing). A banjo's short 5th string ignores the capo and only frets above fret 5.
    inline std::optional<std::vector<int>> courseNotes (const Course& course, int fret, int capo = 0)
    {
        if (fret < 0) return {};
        std::vector<int> out;
        if (course.start > 0)
        {
            if (fret == 0) return course.midi;
            const int abs = fret + capo;
            if (abs <= course.start) return {};
            for (int m : course.midi) out.push_back (m + abs - course.start);
            return out;
        }
        for (int m : course.midi) out.push_back (m + capo + fret);
        return out;
    }

    // Every note a shape sounds, low to high
    inline std::vector<int> shapeNotes (const std::vector<Course>& courses, const std::vector<int>& frets, int capo = 0)
    {
        std::vector<int> out;
        for (size_t i = 0; i < courses.size(); ++i)
            if (const auto n = courseNotes (courses[i], frets[i], capo)) out.insert (out.end(), n->begin(), n->end());
        std::stable_sort (out.begin(), out.end());
        return out;
    }

    // Each course higher than the one before (a guitar, not a ukulele with a high G)
    inline bool ascending (const std::vector<Course>& courses)
    {
        for (size_t i = 1; i < courses.size(); ++i) if (! (courses[i].open > courses[i - 1].open)) return false;
        return true;
    }

    // One finger pressed across several strings: its fret and the first and last string
    struct Barre { int fret = 0, from = 0, to = 0; };

    struct Fingering { int fingers = 0; std::optional<Barre> barre; bool partial = false; };

    // How many fingers a shape needs, whether it uses a barre, and whether a finger
    // lies flat across neighbouring strings above it
    inline Fingering fingering (const std::vector<int>& frets)
    {
        struct F { int f, i; };
        std::vector<F> fretted;
        for (int i = 0; i < (int) frets.size(); ++i) if (frets[(size_t) i] > 0) fretted.push_back ({ frets[(size_t) i], i });
        if (fretted.empty()) return {};
        int fmin = fretted[0].f;
        for (auto& x : fretted) fmin = std::min (fmin, x.f);
        std::vector<F> atMin, above;
        for (auto& x : fretted) (x.f == fmin ? atMin : above).push_back (x);
        Fingering out;
        if (atMin.size() >= 2)
        {
            const int a = atMin.front().i, b = atMin.back().i;
            bool ok = true;
            for (int j = a; j <= b; ++j) if (! (frets[(size_t) j] >= fmin)) ok = false;
            if (ok) out.barre = Barre { fmin, a, b };
        }
        int fingers = out.barre ? 1 : (int) atMin.size();
        int runs = 0;
        for (size_t k = 0; k < above.size(); ++k)
            if (k == 0 || above[k].f != above[k - 1].f || above[k].i != above[k - 1].i + 1) ++runs;
        if (fingers + (int) above.size() <= 4) fingers += (int) above.size();
        else
        {
            fingers += runs;
            out.partial = runs < (int) above.size();
        }
        out.fingers = fingers;
        return out;
    }

    // A playable shape: per course -1 muted, 0 open, n fretted (from the capo); a score
    // (lower is easier and fuller); the lowest fretted fret (0 for an all-open shape);
    // fingers, barre, the notes it sounds, whether it has no root, and the degrees it leaves out
    struct Voicing
    {
        std::vector<int> frets;
        double score = 0;
        int pos = 0;
        int fingers = 0;
        std::optional<Barre> barre;
        std::vector<int> notes;
        bool rootless = false;
        std::vector<std::string> missing;
    };

    struct FretOptions { int capo = 0; int maxFret = 15; int limit = 24; int minSound = -1; };

    namespace detail
    {
        struct Needs { std::vector<harmony::ChordTone> need, may, all; };

        // the tones a voicing must have and the ones it may drop, for n courses: too many
        // notes for the strings drops the 5th, then the 11th, the 9th and last the root
        inline Needs needs (const Chord& c, int n)
        {
            const auto& t = harmony::typeOf (c);
            Needs out;
            out.all = harmony::chordTones (c);
            for (const auto& x : out.all) (harmony::detail::isOpt (t, x.d) ? out.may : out.need).push_back (x);
            for (const char* d : { "5", "11", "9", "1" })
                if ((int) out.need.size() > n)
                    for (size_t k = 0; k < out.need.size(); ++k)
                        if (out.need[k].d == d) { out.may.push_back (out.need[k]); out.need.erase (out.need.begin() + (long) k); break; }
            return out;
        }
    }

    // Playable shapes for a chord on an instrument, best first
    inline std::vector<Voicing> fretVoicings (const Chord& c, const std::vector<Course>& courses, FretOptions opts = {})
    {
        const int capo = opts.capo, maxFret = opts.maxFret, limit = opts.limit;
        const int n = (int) courses.size();
        const auto nd = detail::needs (c, n);
        std::vector<int> pcs, needPcs;
        for (const auto& x : nd.all) if (! harmony::detail::has (pcs, x.pc)) pcs.push_back (x.pc);
        for (const auto& x : nd.need) if (! harmony::detail::has (needPcs, x.pc)) needPcs.push_back (x.pc);
        const int rootPc = pcOf (c.root), bassPc = c.bass ? pcOf (*c.bass) : rootPc;
        if (c.bass && ! harmony::detail::has (pcs, bassPc)) pcs.push_back (bassPc);
        const bool asc = ascending (courses);
        const bool strict = c.bass.has_value() || (asc && n >= 5);
        const double bassMiss = asc ? 0.8 : 0.3;
        const int extraBass = (c.bass && ! harmony::detail::has (needPcs, bassPc)) ? 1 : 0;
        const int minSound = opts.minSound >= 0 ? opts.minSound
                           : (n <= 4 ? n : std::min (n, std::max (3, (int) needPcs.size() + extraBass)));
        const int frettedFrom = n > 8 ? n - 7 : 0;
        std::vector<std::pair<std::string, Voicing>> found;

        auto consider = [&] (const std::vector<int>& frets, int pos)
        {
            std::vector<int> fretted;
            for (int f : frets) if (f > 0) fretted.push_back (f);
            const int fmin = fretted.empty() ? 0 : *std::min_element (fretted.begin(), fretted.end());
            const int fmax = fretted.empty() ? 0 : *std::max_element (fretted.begin(), fretted.end());
            if (fretted.empty() ? pos != 1 : fmin != pos) return;
            if (! fretted.empty() && fmax - pos > (pos >= 5 ? 4 : 3)) return;
            int sounding = 0;
            for (int f : frets) if (f >= 0) ++sounding;
            if (sounding < minSound) return;
            const auto notes = shapeNotes (courses, frets, capo);
            std::vector<int> have;
            for (int m : notes) if (! harmony::detail::has (have, mod12 (m))) have.push_back (mod12 (m));
            for (int p : needPcs) if (! harmony::detail::has (have, p)) return;
            const int lowPc = mod12 (notes[0]);
            if (strict && lowPc != bassPc) return;
            const auto fg = fingering (frets);
            if (fg.fingers > 4) return;
            int first = -1, last = -1;
            for (int i = 0; i < n; ++i) if (frets[(size_t) i] >= 0) { if (first < 0) first = i; last = i; }
            int interior = 0;
            for (int j = first; j <= last; ++j) if (frets[(size_t) j] < 0) ++interior;
            if (interior > 1) return;
            int opens = 0;
            for (int f : frets) if (f == 0) ++opens;
            const int span = fretted.empty() ? 0 : fmax - pos;
            const bool low = fretted.empty() || pos <= 4;
            int high = 0;
            for (int f : fretted) if (f > 3) ++high;
            int missingCount = 0;
            for (const auto& x : nd.all) if (! harmony::detail::has (have, x.pc)) ++missingCount;
            const double score = (fretted.empty() ? 0 : pos) * 0.3 + (low ? -0.3 : 0.25) * opens - sounding * 0.25
                + (low && opens ? high * 0.5 : 0)
                + interior * 1.5 + (n - 1 - last) * 0.6 + first * 0.15
                + (span >= 3 ? (low ? 0.8 : 0.4) : 0) + (span >= 4 ? 1.1 : 0) + (fg.fingers == 4 ? 0.4 : 0)
                + (fg.barre ? 0.5 : 0) + (fg.partial ? 0.3 : 0)
                + missingCount * 0.6
                + (lowPc != bassPc ? bassMiss : 0);
            std::string key;
            for (size_t i = 0; i < frets.size(); ++i) key += (i ? "," : "") + std::to_string (frets[i]);
            for (const auto& [k, v] : found) if (k == key) return;
            Voicing v;
            v.frets = frets;
            v.score = score;
            v.pos = fretted.empty() ? 0 : pos;
            v.fingers = fg.fingers;
            v.barre = fg.barre;
            v.notes = notes;
            v.rootless = ! harmony::detail::has (have, rootPc);
            for (const auto& x : nd.all) if (! harmony::detail::has (have, x.pc)) v.missing.push_back (x.d);
            found.push_back ({ key, v });
        };

        for (int pos = 1; pos <= std::max (1, maxFret); ++pos)
        {
            const int span = pos >= 5 ? 4 : 3, hi = std::min (maxFret, pos + span);
            std::vector<std::vector<int>> options;
            for (int i = 0; i < n; ++i)
            {
                std::vector<int> o { -1 };
                const auto open = courseNotes (courses[(size_t) i], 0, capo);
                if (harmony::detail::has (pcs, mod12 ((*open)[0]))) o.push_back (0);
                if (i >= frettedFrom)
                    for (int f = pos; f <= hi; ++f)
                        if (const auto m = courseNotes (courses[(size_t) i], f, capo); m && harmony::detail::has (pcs, mod12 ((*m)[0]))) o.push_back (f);
                options.push_back (o);
            }
            std::vector<int> frets ((size_t) n, -1);
            std::function<void (int, bool, bool, int, int)> walk = [&] (int i, bool started, bool gap, int interior, int fretted)
            {
                if (fretted > 6 || interior > 1) return;
                if (i == n) { consider (frets, pos); return; }
                for (int f : options[(size_t) i])
                {
                    frets[(size_t) i] = f;
                    if (f < 0) walk (i + 1, started, started, interior, fretted);
                    else walk (i + 1, true, false, interior + (gap ? 1 : 0), fretted + (f > 0 ? 1 : 0));
                }
                frets[(size_t) i] = -1;
            };
            walk (0, false, false, 0, 0);
        }

        if (found.empty() && n <= 4 && minSound > std::min (3, n))
        {
            FretOptions fewer = opts;
            fewer.minSound = minSound - 1;
            return fretVoicings (c, courses, fewer);
        }
        std::vector<Voicing> sorted;
        for (auto& [k, v] : found) sorted.push_back (v);
        std::stable_sort (sorted.begin(), sorted.end(), [] (const Voicing& a, const Voicing& b) { return a.score < b.score; });
        // no more than four shapes at any one position, so the list covers the neck
        std::map<int, int> perPos;
        std::vector<Voicing> out;
        for (const auto& v : sorted)
        {
            int& k = perPos[v.pos];
            if (k >= 4) continue;
            ++k;
            out.push_back (v);
            if ((int) out.size() >= limit) break;
        }
        return out;
    }

    // A shape as text, low string first: x32010 (frets over 9 in brackets so it still reads)
    inline std::string shapeText (const std::vector<int>& frets)
    {
        std::string s;
        for (int f : frets) s += f < 0 ? "x" : f > 9 ? "(" + std::to_string (f) + ")" : std::to_string (f);
        return s;
    }

    // A piano voicing: the bass note in the left hand (E2 to Eb3) and the chord close
    // together in the right (around middle C), in whichever inversion moves least from
    // the last chord's right hand (prev)
    struct PianoVoicing { int bass = 0; std::vector<int> rh, notes; };

    inline PianoVoicing pianoVoicing (const Chord& c, const std::vector<int>& prev = {})
    {
        auto tones = harmony::chordTones (c);
        const int bassPc = c.bass ? pcOf (*c.bass) : pcOf (c.root);
        const int bass = 40 + mod12 (bassPc - 4);
        auto rh = tones;
        if (rh.size() > 4) rh.erase (std::remove_if (rh.begin(), rh.end(), [] (const auto& x) { return x.d == "5"; }), rh.end());
        if (rh.size() > 5)
        {
            const bool m11 = std::string_view (harmony::typeOf (c).id) == "m11";
            rh.erase (std::remove_if (rh.begin(), rh.end(), [&] (const auto& x) { return ! (x.d != "11" || m11); }), rh.end());
        }
        std::vector<int> pcs;
        for (const auto& x : rh) pcs.push_back (x.pc);
        std::vector<int> best;
        double bestCost = 1e9;
        std::vector<int> p = prev;
        std::stable_sort (p.begin(), p.end());
        for (size_t inv = 0; inv < pcs.size(); ++inv)
            for (int start = 55; start <= 66; ++start)
            {
                if (mod12 (start) != pcs[inv]) continue;
                std::vector<int> stack { start };
                for (size_t k = 1; k < pcs.size(); ++k)
                {
                    int m = stack[k - 1] + 1;
                    while (mod12 (m) != pcs[(inv + k) % pcs.size()]) ++m;
                    stack.push_back (m);
                }
                double centre = 0;
                for (int m : stack) centre += m;
                centre /= (double) stack.size();
                double cost = std::abs (centre - 64) * 0.5;
                if (! p.empty())
                {
                    cost = 0;
                    for (size_t k = 0; k < stack.size(); ++k) cost += std::abs (stack[k] - p[std::min (k, p.size() - 1)]);
                    cost += std::abs (centre - 64) * 0.1;
                }
                if (cost < bestCost) { bestCost = cost; best = stack; }
            }
        PianoVoicing v;
        v.bass = bass;
        v.rh = best;
        v.notes = { bass };
        v.notes.insert (v.notes.end(), best.begin(), best.end());
        return v;
    }

    // The shape to play for a chord with a capo: the chord moved down, spelled the plain guitar way
    inline Chord shapeFor (const Chord& c, int capo)
    {
        const int pc = mod12 (pcOf (c.root) - capo);
        const auto root = harmony::spellRoot (pc, c.type);
        return harmony::chord (root, c.type, c.bass ? std::optional<music::Note> (harmony::spellBass (mod12 (pcOf (*c.bass) - capo), root)) : std::nullopt);
    }

    // One capo position: the shapes for each different chord, how easy each is near the
    // nut, the overall score (lower is easier) and the shapes for every chord in order
    struct CapoOption { int capo = 0; std::vector<Chord> shapes; std::vector<double> ease; double score = 0; std::vector<Chord> all; };

    // The capo helper: for each capo fret, the shapes you'd play and how easy they are. Best first.
    inline std::vector<CapoOption> capoOptions (const std::vector<Chord>& chords, const std::vector<Course>& courses, int maxCapo = 7)
    {
        std::vector<Chord> unique;
        for (const auto& c : chords)
        {
            bool dup = false;
            for (const auto& u : unique)
            {
                const int ub = u.bass ? pcOf (*u.bass) : -1, cb = c.bass ? pcOf (*c.bass) : -1;
                const bool sameBass = (u.bass.has_value() == c.bass.has_value()) && ub == cb;
                if (pcOf (u.root) == pcOf (c.root) && u.type == c.type && sameBass) dup = true;
            }
            if (! dup) unique.push_back (c);
        }
        std::vector<CapoOption> out;
        for (int capo = 0; capo <= maxCapo; ++capo)
        {
            CapoOption o;
            o.capo = capo;
            double sum = 0;
            for (const auto& c : unique)
            {
                const auto s = shapeFor (c, capo);
                o.shapes.push_back (s);
                FretOptions f;
                f.maxFret = 4;
                f.limit = 1;
                const auto v = fretVoicings (s, courses, f);
                o.ease.push_back (v.empty() ? 6.0 : v[0].score);
                sum += o.ease.back();
            }
            o.score = sum / std::max<double> (1, (double) o.ease.size()) + capo * 0.12;
            for (const auto& c : chords) o.all.push_back (shapeFor (c, capo));
            out.push_back (o);
        }
        std::stable_sort (out.begin(), out.end(), [] (const CapoOption& a, const CapoOption& b) { return a.score < b.score; });
        return out;
    }
}
