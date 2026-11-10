#pragma once

// The metronome: the beat grid (meters, groupings into 2s and 3s, accents,
// subdivisions and swing, polyrhythms, the tempo trainer and gap bars), the
// sequencer that works out every count at an exact (fractional) sample
// frame, the click sounds (synthesized, no sample files) and the click
// engine that mixes them into blocks, as an AudioWorklet or a plugin calls
// it. Plain C++20, no JUCE.
//
// The sounds and the timing are computed the same way, operation for
// operation, in double precision with float buffers, so every platform
// renders the same samples.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>
#include "Biquad.h"
#include "Random.h"

namespace vs::metronome
{
    inline constexpr double pi = 3.141592653589793;

    // The running peak of |v|; a NaN sample makes it NaN (a NaN peak then
    // counts as no peak), as a browser's Math.max does
    inline float peakOf (float peak, float v)
    {
        return std::isnan (peak) || std::isnan (v) ? std::numeric_limits<float>::quiet_NaN() : std::max (peak, std::abs (v));
    }

    // Rounds half up, as a browser's Math.round does (2.5 to 3, -2.5 to -2)
    inline double roundHalfUp (double x)
    {
        const double f = std::floor (x);
        return x - f >= 0.5 ? f + 1.0 : f;
    }

    // 0 and NaN count as missing
    inline double orDefault (double v, double fallback) { return v != 0 && ! std::isnan (v) ? v : fallback; }

    inline constexpr double minBpm = 20.0;
    inline constexpr double maxBpm = 400.0;
    inline double clampBpm (double b) { return std::min (maxBpm, std::max (minBpm, orDefault (b, 120.0))); }

    // A count's level: 3 strong accent, 2 normal, 1 soft, 0 silent
    inline constexpr const char* levelNames[] { "Silent", "Soft", "Normal", "Accent" };

    struct Subdivision { const char* id; const char* name; int n; bool swing; };

    inline constexpr Subdivision subdivisions[] {
        { "none", "None", 1, false },
        { "8", "8ths", 2, true },
        { "3", "Triplets", 3, false },
        { "16", "16ths", 4, true },
        { "5", "Quintuplets", 5, false },
        { "6", "Sextuplets", 6, false },
    };

    inline const Subdivision* findSubdivision (const std::string& id)
    {
        for (auto& d : subdivisions) if (id == d.id) return &d;
        return nullptr;
    }

    // Where the clicks inside one count fall, as fractions of the count
    // (always starting at 0). swing: 50 is straight, 66.7 a triplet swing, 75
    // the hardest (dotted 8th and 16th). Only 8ths and 16ths swing. An unknown
    // id is no subdivision.
    inline std::vector<double> subFractions (const std::string& id, double swing = 50.0)
    {
        const double s = std::min (75.0, std::max (50.0, orDefault (swing, 50.0))) / 100.0;
        if (id == "8") return { 0.0, s };
        if (id == "16") return { 0.0, s / 2, 0.5, 0.5 + s / 2 };
        const auto* d = findSubdivision (id);
        const int n = d != nullptr ? d->n : subdivisions[0].n;
        std::vector<double> out;
        for (int i = 0; i < n; ++i) out.push_back ((double) i / n);
        return out;
    }

    // ---- meters and groupings: a bar is `top` counts of 1/`bottom` notes,
    // felt in groups (7/8 as 2+2+3)

    // Every way to split n counts into 2s and 3s, in order (2+2+3, 2+3+2, 3+2+2 for 7)
    inline std::vector<std::vector<int>> twosAndThrees (int n)
    {
        if (n == 0) return { {} };
        std::vector<std::vector<int>> out;
        for (int g : { 2, 3 })
            if (n - g >= 0)
                for (auto& rest : twosAndThrees (n - g))
                {
                    std::vector<int> v { g };
                    v.insert (v.end(), rest.begin(), rest.end());
                    out.push_back (std::move (v));
                }
        return out;
    }

    inline std::string joinGroups (const std::vector<int>& groups)
    {
        std::string s;
        for (size_t i = 0; i < groups.size(); ++i) s += (i ? "+" : "") + std::to_string (groups[i]);
        return s;
    }

    inline std::string groupsLabel (const std::vector<int>& groups)
    {
        return std::all_of (groups.begin(), groups.end(), [] (int g) { return g == 1; }) ? "Every count" : joinGroups (groups);
    }

    // The groupings worth offering for a meter: every count, the even ones,
    // then the 2s-and-3s mixes (at most 12)
    inline std::vector<std::vector<int>> groupingOptions (int top, int /*bottom*/)
    {
        std::vector<std::string> seen;
        std::vector<std::vector<int>> out;
        auto add = [&] (const std::vector<int>& g)
        {
            const auto k = joinGroups (g);
            if (! g.empty() && std::find (seen.begin(), seen.end(), k) == seen.end() && std::accumulate (g.begin(), g.end(), 0) == top)
            {
                seen.push_back (k);
                out.push_back (g);
            }
        };
        add (std::vector<int> ((size_t) std::max (0, top), 1));
        for (int size : { 3, 2, 4 })
            if (top > size && top % size == 0) add (std::vector<int> ((size_t) (top / size), size));
        if (top >= 4)
        {
            auto all = twosAndThrees (top);
            std::vector<std::vector<int>> mixes;
            for (auto& g : all)
                if (std::find (g.begin(), g.end(), 2) != g.end() && std::find (g.begin(), g.end(), 3) != g.end()) mixes.push_back (g);
            std::stable_sort (mixes.begin(), mixes.end(), [] (const auto& a, const auto& b) { return a.size() < b.size(); });
            for (auto& g : mixes)
            {
                if (out.size() >= 12) break;
                add (g);
            }
        }
        return out;
    }

    // How a meter is felt unless you say otherwise: 6/8, 9/8, 12/8 in threes,
    // 5/8 as 3+2, 7/8 as 2+2+3, x/4 on every count
    inline std::vector<int> defaultGroups (int top, int bottom)
    {
        if (bottom >= 8 && top > 3)
        {
            if (top % 3 == 0) return std::vector<int> ((size_t) (top / 3), 3);
            if (top == 5) return { 3, 2 };
            // odd ones: 2s, with a 3 at the end (7 as 2+2+3, 11 as 2+2+2+2+3)
            if (top % 2)
            {
                std::vector<int> g ((size_t) ((top - 3) / 2), 2);
                g.push_back (3);
                return g;
            }
            return std::vector<int> ((size_t) (top / 2), 2);
        }
        return std::vector<int> ((size_t) std::max (0, top), 1);
    }

    // "3+2+2" to { 3, 2, 2 }, or nothing unless it's whole numbers adding up to `top`
    inline std::optional<std::vector<int>> parseGroups (const std::string& text, int top)
    {
        std::vector<double> parts;
        std::string digits;
        auto flush = [&]
        {
            if (! digits.empty()) parts.push_back (std::stod (digits));
            digits.clear();
        };
        for (char c : text)
        {
            if (c >= '0' && c <= '9') digits += c;
            else flush();
        }
        flush();
        double sum = 0;
        for (double p : parts) sum += p;
        if (parts.empty() || std::any_of (parts.begin(), parts.end(), [] (double p) { return p < 1; }) || sum != top) return std::nullopt;
        std::vector<int> out;
        for (double p : parts) out.push_back ((int) p);
        return out;
    }

    // The accent pattern a grouping starts with: the downbeat strong, each
    // group's first count normal, the counts inside a group soft
    inline std::vector<int> defaultAccents (const std::vector<int>& groups)
    {
        std::vector<int> out;
        for (size_t gi = 0; gi < groups.size(); ++gi)
            for (int i = 0; i < groups[gi]; ++i) out.push_back (i == 0 ? (gi == 0 ? 3 : 2) : 1);
        return out;
    }

    // For each count of the bar: which group it's in and whether it starts one
    struct GroupPlace { int group; bool start; };

    inline std::vector<GroupPlace> groupMap (const std::vector<int>& groups)
    {
        std::vector<GroupPlace> map;
        for (size_t gi = 0; gi < groups.size(); ++gi)
            for (int i = 0; i < groups[gi]; ++i) map.push_back ({ (int) gi, i == 0 });
        return map;
    }

    inline const char* valueName (int den)
    {
        switch (den) { case 1: return "whole"; case 2: return "half"; case 4: return "quarter"; case 8: return "8th"; case 16: return "16th"; case 32: return "32nd"; default: return nullptr; }
    }

    // The note a beat of `units` counts of 1/`bottom` is: 1 of 8 an 8th, 3 of
    // 8 a dotted quarter, 2 of 4 a half. A bottom with no note name reads
    // "undefined".
    inline std::string noteValueName (int units, int bottom)
    {
        // units/bottom of a whole note, in lowest terms
        const int g = std::gcd (units, bottom);
        const int num = g ? units / g : units, den = g ? bottom / g : bottom;
        if (num == 1 && valueName (den)) return valueName (den);
        if (num == 3 && den % 2 == 0 && valueName (den / 2)) return std::string ("dotted ") + valueName (den / 2);
        const char* b = valueName (bottom);
        return std::to_string (units) + " \xC3\x97 " + (b ? b : "undefined");
    }

    // A note value the tempo can count
    struct UnitOption { int units; std::string name; };

    // What the tempo can count: the written note, or the felt beat when every
    // group is the same size (6/8 in dotted quarters)
    inline std::vector<UnitOption> unitOptions (const std::vector<int>& groups, int bottom)
    {
        std::vector<UnitOption> out { { 1, noteValueName (1, bottom) } };
        if (! groups.empty() && groups[0] > 1 && std::all_of (groups.begin(), groups.end(), [&] (int g) { return g == groups[0]; }))
            out.push_back ({ groups[0], noteValueName (groups[0], bottom) });
        return out;
    }

    // Seconds per count at `bpm` beats a minute, a beat being `units` counts
    inline double countSeconds (double bpm, double units = 1) { return 60 / bpm / units; }

    // The Italian tempo name for a tempo
    inline const char* tempoName (double bpm)
    {
        static constexpr std::pair<double, const char*> names[] {
            { 24, "Larghissimo" }, { 40, "Grave" }, { 60, "Largo" }, { 66, "Larghetto" }, { 76, "Adagio" }, { 92, "Andante" },
            { 108, "Andantino" }, { 120, "Moderato" }, { 156, "Allegro" }, { 176, "Vivace" }, { 200, "Presto" },
        };
        for (auto& [top, name] : names) if (bpm < top) return name;
        return "Prestissimo";
    }

    // ---- tempo trainer and gap click

    enum class TrainerMode { off, steps, ramp };

    struct TrainerSettings
    {
        TrainerMode mode = TrainerMode::off;
        double from = 80, step = 4, every = 4, to = 120;
        bool loop = false;
        double minutes = 5;
    };

    struct GapSettings { bool on = false; int play = 4, mute = 4; };
    struct PolySettings { bool on = false; int n = 3; };

    // The trainer's tempo at bar `bar` (0 = the first bar after any
    // count-in), `seconds` in. steps: from, add `step` every `every` bars until
    // `to` (then stay, or start again with loop). ramp: from to `to` smoothly
    // over `minutes` (then stay, or start again with loop). Nothing when off.
    inline std::optional<double> trainerTempo (const TrainerSettings& tr, int bar, double seconds)
    {
        if (tr.mode == TrainerMode::off) return std::nullopt;
        const double from = clampBpm (tr.from), to = clampBpm (tr.to);
        const double dir = (double) ((to > from) - (to < from)), span = std::abs (to - from);
        if (tr.mode == TrainerMode::steps)
        {
            const double step = std::max (0.1, std::abs (orDefault (tr.step, 1)));
            const double every = std::max (1.0, roundHalfUp (orDefault (tr.every, 1)));
            const double stages = std::ceil (span / step - 1e-9) + 1;
            double i = std::floor (std::max (0, bar) / every);
            if (tr.loop) i = std::fmod (i, stages);
            return from + dir * std::min (i * step, span);
        }
        const double T = std::max (1.0, orDefault (tr.minutes, 1) * 60);
        double t = std::max (0.0, seconds);
        t = tr.loop ? std::fmod (t, T) : std::min (t, T);
        return from + ((to - from) * t) / T;
    }

    // Gap click: `play` bars with the click, then `mute` silent bars, over and over
    inline bool isGapBar (const GapSettings& gap, int bar)
    {
        return gap.on && bar >= 0 && gap.mute > 0 && bar % (gap.play + gap.mute) >= gap.play;
    }

    // ---- the sequencer: works out every count, one after the next, at exact
    // (fractional) sample frames

    // Everything the click plays from. A settings object made with only some
    // fields set has the defaults for the rest; cleanSettings() mends the rest.
    struct MetronomeSettings
    {
        double bpm = 120;
        int top = 4, bottom = 4;
        std::vector<int> groups { 1, 1, 1, 1 };
        int units = 1;
        // one level per count: 3 accent, 2 normal, 1 soft, 0 silent
        std::vector<int> accents { 3, 2, 2, 2 };
        std::string sub = "none";
        double swing = 50;
        PolySettings poly;
        TrainerSettings trainer;
        GapSettings gap;
        double randomMute = 0;
    };

    // Fills in anything missing or broken in a settings object, so a bad
    // preset or old save can't break the clock
    inline MetronomeSettings cleanSettings (MetronomeSettings o)
    {
        o.top = std::min (16, std::max (1, o.top != 0 ? o.top : 4));
        o.bottom = (o.bottom == 2 || o.bottom == 4 || o.bottom == 8 || o.bottom == 16) ? o.bottom : 4;
        if (! (std::accumulate (o.groups.begin(), o.groups.end(), 0) == o.top && std::all_of (o.groups.begin(), o.groups.end(), [] (int g) { return g >= 1; })))
            o.groups = defaultGroups (o.top, o.bottom);
        {
            const auto opts = unitOptions (o.groups, o.bottom);
            if (std::none_of (opts.begin(), opts.end(), [&] (const UnitOption& u) { return u.units == o.units; })) o.units = 1;
        }
        if ((int) o.accents.size() == o.top)
            for (auto& a : o.accents) a = std::min (3, std::max (0, a));
        else
            o.accents = defaultAccents (o.groups);
        o.bpm = clampBpm (o.bpm);
        if (findSubdivision (o.sub) == nullptr) o.sub = "none";
        o.swing = std::min (75.0, std::max (50.0, orDefault (o.swing, 50)));
        o.poly.n = std::min (16, std::max (1, o.poly.n != 0 ? o.poly.n : 3));
        o.gap.play = std::max (1, o.gap.play != 0 ? o.gap.play : 1);
        o.gap.mute = std::max (0, o.gap.mute);
        o.randomMute = std::min (100.0, std::max (0.0, std::isnan (o.randomMute) ? 0.0 : o.randomMute));
        return o;
    }

    // A polyrhythm click inside a count: the j-th of the bar's n, at `frac` of the count
    struct PolyClick { int j; double frac; };

    // One count, as the sequencer hands it out
    struct CountEvent
    {
        double frame = 0;   // where the count starts, in (fractional) sample frames
        double len = 0;     // its length in sample frames
        int bar = 0;        // -1 during the count-in
        int count = 0, top = 0, group = 0;
        bool groupStart = false;
        std::vector<int> groups;
        int level = 0;
        bool countIn = false, gap = false, muted = false;
        double bpm = 0;
        int units = 1;
        std::string sub;
        double swing = 50;
        std::vector<double> subs;       // where the clicks inside the count fall, as fractions of it
        std::vector<PolyClick> poly;
        int polyN = 0;
    };

    struct StartOptions { bool countIn = false; std::uint32_t seed = 1; };

    // Hands out the counts one at a time. A new tempo, accents, subdivision,
    // trainer or gap setting takes effect from the next count; a new meter,
    // grouping or polyrhythm from the next bar (so a bar is never cut short).
    class Sequencer
    {
    public:
        double sr;
        MetronomeSettings want, s;
        double frame = 0;      // where the next count starts (fractional sample frames, so nothing drifts)
        int count = 0, bar = 0;
        double seconds = 0;    // time since bar 0 began, for the ramp trainer
        Mulberry32 random { 1 };

        explicit Sequencer (double sampleRate, const MetronomeSettings& settings = {})
            : sr (sampleRate), want (cleanSettings (settings)), s (want)
        {
            start (0);
        }

        void set (const MetronomeSettings& settings) { want = cleanSettings (settings); }

        // Starts again: the first count at `frame`, after one count-in bar when countIn
        void start (double startFrame, StartOptions opts = {})
        {
            frame = startFrame;
            count = 0;
            bar = opts.countIn ? -1 : 0;
            seconds = 0;
            random = Mulberry32 (opts.seed);
            s = want;
        }

        CountEvent next()
        {
            const auto& w = want;
            // a new bar takes everything; mid-bar, only what doesn't change the bar's shape
            if (count == 0) s = w;
            else
            {
                s.bpm = w.bpm;
                if (w.top == s.top) { s.units = w.units; s.accents = w.accents; }
                s.sub = w.sub;
                s.swing = w.swing;
                s.trainer = w.trainer;
                s.gap = w.gap;
                s.randomMute = w.randomMute;
            }
            const bool countIn = bar < 0;
            const auto tempo = trainerTempo (s.trainer, std::max (0, bar), seconds);
            const double bpm = clampBpm (tempo ? *tempo : s.bpm);
            const double len = sr * countSeconds (bpm, s.units);
            const auto where = groupMap (s.groups)[(size_t) count];
            int level = count < (int) s.accents.size() ? s.accents[(size_t) count] : 2;
            if (countIn && level == 0) level = 1; // the count-in sounds every count
            const bool gap = ! countIn && isGapBar (s.gap, bar);
            const bool muted = gap || (! countIn && s.randomMute > 0 && random.uniform() * 100 < s.randomMute);
            CountEvent ev;
            // the polyrhythm: n clicks spread evenly over the bar; the ones that land in this count
            if (s.poly.on && ! countIn)
            {
                const int n = s.poly.n, B = s.top;
                for (int j = (int) std::ceil ((double) (count * n) / B - 1e-9); j * B < (count + 1) * n; ++j)
                    ev.poly.push_back ({ j, (double) (j * B - count * n) / n });
            }
            ev.frame = frame;
            ev.len = len;
            ev.bar = bar;
            ev.count = count;
            ev.top = s.top;
            ev.group = where.group;
            ev.groupStart = where.start;
            ev.groups = s.groups;
            ev.level = level;
            ev.countIn = countIn;
            ev.gap = gap;
            ev.muted = muted;
            ev.bpm = bpm;
            ev.units = s.units;
            ev.sub = s.sub;
            ev.swing = s.swing;
            ev.subs = countIn ? std::vector<double> { 0.0 } : subFractions (s.sub, s.swing);
            ev.polyN = s.poly.on ? s.poly.n : 0;
            frame += len;
            if (! countIn) seconds += len / sr;
            if (++count >= s.top)
            {
                count = 0;
                ++bar;
            }
            return ev;
        }
    };

    // ---- the click sounds: synthesized (no sample files), each starting at
    // full level on its first sample so the click is exactly where it was scheduled

    struct SoundSet { const char* id; const char* name; };

    // Every sound set, in the order a list shows them
    inline constexpr SoundSet soundSets[] {
        { "classic", "Classic click" },
        { "woodblock", "Woodblock" },
        { "cowbell", "Cowbell" },
        { "hihat", "Hi-hat" },
        { "rim", "Rim click" },
        { "beep", "Sine beep" },
        { "groove", "Kick and hat" },
        { "clave", "Clave" },
        { "softtick", "Soft tick" },
    };

    using Buffer = std::vector<float>;

    // A partial of a struck sound: Hz, level, decay seconds
    struct Partial { double hz, level, tau; };

    // Damped sinusoids plus a burst of noise
    inline Buffer modal (double sr, double seconds, std::initializer_list<Partial> partials, double noise = 0, double noiseDecay = 0.002, std::uint32_t seed = 3)
    {
        const int n = (int) std::floor (sr * seconds);
        Buffer x ((size_t) n, 0.0f);
        Mulberry32 r (seed);
        for (const auto& p : partials)
        {
            const double w = (2 * pi * p.hz) / sr, k = std::exp (-1 / (p.tau * sr));
            double amp = p.level;
            for (int i = 0; i < n; ++i)
            {
                x[(size_t) i] = (float) (x[(size_t) i] + amp * std::sin (w * i + 0.6));
                amp *= k;
            }
        }
        if (noise != 0)
        {
            const double k = std::exp (-1 / (noiseDecay * sr));
            double amp = noise, prev = 0;
            for (int i = 0; i < n && amp > 1e-5; ++i)
            {
                const double v = r.uniform() * 2 - 1;
                x[(size_t) i] = (float) (x[(size_t) i] + amp * (v - prev));
                prev = v;
                amp *= k;
            }
        }
        return x;
    }

    // A square wave without aliasing: its odd harmonics, up to just under half the sample rate
    inline Buffer square (double sr, int n, double hz)
    {
        Buffer x ((size_t) n, 0.0f);
        for (int k = 1; k * hz < sr * 0.45; k += 2)
        {
            const double w = (2 * pi * k * hz) / sr, a = 4 / (pi * k);
            for (int i = 0; i < n; ++i) x[(size_t) i] = (float) (x[(size_t) i] + a * std::sin (w * i));
        }
        return x;
    }

    // A biquad (RBJ cookbook) over the whole buffer, in place. The frequency is
    // kept at or under 0.45 of the sample rate, so a filter set above it (the
    // hats' 12 kHz at 22.05 kHz) stays stable.
    inline void biquad (Buffer& x, double sr, BiquadCoefs::Type type, double hz, double q)
    {
        filterDirectFormI (BiquadCoefs::rbjRaw (type, sr, std::min (hz, sr * 0.45), q), x.data(), (int) x.size());
    }

    // Starts a sound where it first gets loud: filters ease into a hit, and a
    // click has to land right on the beat
    inline Buffer onTime (const Buffer& x)
    {
        float peak = 0;
        for (float v : x) peak = peakOf (peak, v);
        size_t i = 0;
        while (i < x.size() && std::abs (x[i]) < 0.3 * peak) ++i;
        return Buffer (x.begin() + (std::ptrdiff_t) (i >= 2 ? i - 2 : 0), x.end());
    }

    // The drum machine cowbell: two square waves a not-quite-fifth apart (540
    // and 800 Hz), band-passed, with a sharp drop right after the hit and then
    // a ringing tail. pitch scales both
    inline Buffer cowbell (double sr, double seconds, double pitch = 1, double tail = 0.12)
    {
        using T = BiquadCoefs::Type;
        const int n = (int) std::floor (sr * seconds);
        const auto a = square (sr, n, 540 * pitch), b = square (sr, n, 800 * pitch);
        Buffer x ((size_t) n);
        for (size_t i = 0; i < (size_t) n; ++i) x[i] = (float) ((double) a[i] + (double) b[i]);
        biquad (x, sr, T::bandPass, 2000 * pitch, 0.9);
        biquad (x, sr, T::highPass, 400, 0.7);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            x[(size_t) i] = (float) (x[(size_t) i] * (0.7 * std::exp (-t / 0.012) + 0.3 * std::exp (-t / tail)));
        }
        return onTime (x);
    }

    // A hi-hat: bright noise plus six clashing square waves (the drum machine
    // metal), high-passed. tau: how long it rings (short for closed, long for open)
    inline Buffer hihat (double sr, double seconds, double tau, std::uint32_t seed = 11)
    {
        using T = BiquadCoefs::Type;
        const int n = (int) std::floor (sr * seconds);
        Mulberry32 r (seed);
        Buffer x ((size_t) n, 0.0f);
        for (double hz : { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 })
        {
            const auto sq = square (sr, n, hz);
            for (size_t i = 0; i < (size_t) n; ++i) x[i] = (float) (x[i] + (double) sq[i] / 6);
        }
        biquad (x, sr, T::bandPass, 10000, 0.8);
        for (size_t i = 0; i < (size_t) n; ++i) x[i] = (float) (0.6 * x[i] + 0.5 * (r.uniform() * 2 - 1));
        biquad (x, sr, T::highPass, 6500, 0.7);
        biquad (x, sr, T::highPass, 6500, 0.7);
        // and gently off at the very top, so it's a hat, not just sizzle
        biquad (x, sr, T::lowPass, 12000, 0.6);
        for (int i = 0; i < n; ++i) x[(size_t) i] = (float) (x[(size_t) i] * std::exp (-i / sr / tau));
        return onTime (x);
    }

    // A kick: a sine that drops in pitch, with a click on top
    inline Buffer kick (double sr, double seconds, double hiHz, double loHz, double tau)
    {
        const int n = (int) std::floor (sr * seconds);
        Buffer x ((size_t) n);
        double phase = 0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const double f = loHz + (hiHz - loHz) * std::exp (-t / 0.03);
            phase += (2 * pi * f) / sr;
            x[(size_t) i] = (float) (std::exp (-t / tau) * std::sin (phase) + (i < sr * 0.0015 ? 0.5 * (1 - i / (sr * 0.0015)) : 0.0));
        }
        return x;
    }

    // A sine with a short release at the end
    inline Buffer beep (double sr, double hz, double seconds)
    {
        const int n = (int) std::floor (sr * seconds);
        Buffer x ((size_t) n);
        const int rel = (int) std::floor (sr * 0.012);
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = (float) (std::sin ((2 * pi * hz * i) / sr + pi / 4) * (i > n - rel ? (double) (n - i) / rel : 1.0));
        return x;
    }

    // Peak to `level`, with a fade at the very end so nothing clicks off
    inline Buffer finish (Buffer x, double level)
    {
        float peak = 0;
        for (float v : x) peak = peakOf (peak, v);
        const int len = (int) x.size(), fade = std::min (len, 256);
        const double g = level / (peak != 0 && ! std::isnan (peak) ? (double) peak : 1.0);
        for (int i = 0; i < len; ++i) x[(size_t) i] = (float) (x[(size_t) i] * (g * (i > len - fade ? (double) (len - i) / fade : 1.0)));
        return x;
    }

    // A sound set's sounds. alt, when it isn't empty, plays on counts 2, 4,
    // 6... instead of beat or accent.
    struct Sounds { Buffer accent, beat, sub, poly, alt; };

    // One sound set's sounds at `sr`; an unknown id is the classic click
    inline Sounds makeSounds (const std::string& id, double sr)
    {
        Sounds set;
        if (id == "woodblock")
            set = { modal (sr, 0.15, { { 1180, 1, 0.03 }, { 3030, 0.35, 0.012 }, { 5070, 0.15, 0.006 } }, 0.4, 0.001),
                    modal (sr, 0.13, { { 880, 1, 0.028 }, { 2260, 0.35, 0.011 }, { 3780, 0.15, 0.006 } }, 0.35, 0.001),
                    modal (sr, 0.08, { { 660, 1, 0.015 }, { 1700, 0.3, 0.007 } }, 0.25, 0.001),
                    modal (sr, 0.12, { { 1500, 1, 0.025 }, { 3860, 0.3, 0.01 } }, 0.35, 0.001), {} };
        else if (id == "cowbell")
            set = { cowbell (sr, 0.5, 1, 0.16), cowbell (sr, 0.4, 1, 0.11), cowbell (sr, 0.15, 1, 0.03), cowbell (sr, 0.35, 1.335, 0.09), {} };
        else if (id == "hihat")
            // closed on the beats, open on the accent, a tight tick for subdivisions
            set = { hihat (sr, 0.45, 0.13), hihat (sr, 0.12, 0.035), hihat (sr, 0.06, 0.016, 12),
                    modal (sr, 0.15, { { 5200, 1, 0.03 }, { 7800, 0.4, 0.02 } }, 0.3), {} };
        else if (id == "rim")
            set = { modal (sr, 0.06, { { 1700, 1, 0.012 }, { 520, 0.6, 0.012 }, { 4100, 0.3, 0.004 } }, 0.9, 0.0015),
                    modal (sr, 0.05, { { 1500, 1, 0.009 }, { 470, 0.5, 0.01 }, { 3700, 0.3, 0.004 } }, 0.8, 0.0012),
                    modal (sr, 0.03, { { 2100, 1, 0.004 } }, 0.6, 0.0008),
                    modal (sr, 0.05, { { 2600, 1, 0.01 }, { 800, 0.4, 0.01 } }, 0.6, 0.0012), {} };
        else if (id == "beep")
            set = { beep (sr, 1760, 0.07), beep (sr, 880, 0.06), beep (sr, 660, 0.03), beep (sr, 1320, 0.05), {} };
        else if (id == "groove")
            // kick on the odd counts (1 and 3), hat on the even ones (2 and 4): alt is the sound for counts 2, 4, 6...
            set = { kick (sr, 0.4, 170, 48, 0.18), kick (sr, 0.3, 150, 52, 0.12), hihat (sr, 0.06, 0.016, 12),
                    modal (sr, 0.18, { { 200, 0.6, 0.05 }, { 330, 0.3, 0.04 } }, 1.2, 0.03), hihat (sr, 0.12, 0.04) };
        else if (id == "clave")
            // a hard wooden knock: one bright sine ringing 18 ms; the accent a fifth up
            set = { modal (sr, 0.13, { { 3750, 1, 0.018 } }), modal (sr, 0.13, { { 2500, 1, 0.018 } }),
                    modal (sr, 0.07, { { 1900, 1, 0.009 } }), modal (sr, 0.12, { { 3330, 1, 0.016 } }), {} };
        else if (id == "softtick")
            // a short, round tick: a 1300 Hz sine gone in 6 ms; the accent a fifth up
            set = { modal (sr, 0.045, { { 1950, 1, 0.006 } }), modal (sr, 0.045, { { 1300, 1, 0.006 } }),
                    modal (sr, 0.03, { { 1000, 1, 0.004 } }), modal (sr, 0.045, { { 1700, 1, 0.006 } }), {} };
        else
            set = { modal (sr, 0.06, { { 2400, 1, 0.012 }, { 4800, 0.35, 0.006 } }, 0.6),
                    modal (sr, 0.05, { { 1600, 1, 0.011 }, { 3200, 0.3, 0.005 } }, 0.5),
                    modal (sr, 0.03, { { 1150, 1, 0.006 } }, 0.3),
                    modal (sr, 0.05, { { 3300, 1, 0.01 }, { 990, 0.4, 0.01 } }, 0.4), {} };
        Sounds out { finish (std::move (set.accent), 1), finish (std::move (set.beat), 0.8), finish (std::move (set.sub), 0.55),
                     finish (std::move (set.poly), 0.75), {} };
        if (! set.alt.empty()) out.alt = finish (std::move (set.alt), 0.75);
        return out;
    }

    // Slider (0 to 100) to gain: squared, so the low end isn't all crammed together
    inline double sliderGain (double v) { return std::pow (std::min (100.0, std::max (0.0, std::isnan (v) ? 0.0 : v)) / 100, 2); }

    // ---- the click engine: the sequencer plus a mixer. An AudioWorklet
    // calls render() for each 128-frame block.

    // The four volume sliders, 0 to 100
    struct Volumes { double accent = 100, beat = 80, sub = 50, poly = 70; };

    class ClickEngine
    {
    public:
        double sr;
        Sequencer seq;
        std::shared_ptr<const Sounds> sounds;
        bool running = false;
        struct Gains { double accent = 1, beat = 0.5, sub = 0.2, poly = 0.4; } gains;

        explicit ClickEngine (double sampleRate, const MetronomeSettings& settings = {}) : sr (sampleRate), seq (sampleRate, settings)
        {
            voices.reserve (64);
        }

        void setSounds (std::shared_ptr<const Sounds> s) { sounds = std::move (s); }
        void setSounds (Sounds s) { sounds = std::make_shared<const Sounds> (std::move (s)); }

        // The settings, and the four volume sliders
        void set (const MetronomeSettings& settings, const Volumes& v = {})
        {
            seq.set (settings);
            gains = { sliderGain (v.accent), sliderGain (v.beat), sliderGain (v.sub), sliderGain (v.poly) };
        }

        void start (double frame, StartOptions opts = {})
        {
            seq.start (frame, opts);
            voices.clear();
            running = true;
        }

        void stop() { running = false; }

        // The counts that started in the last render() call
        const std::vector<CountEvent>& lastEvents() const { return events; }

        // Mixes frames [frame0, frame0 + n) into out; returns the counts that
        // start in this block (valid until the next call)
        const std::vector<CountEvent>& render (float* out, int n, std::int64_t frame0)
        {
            const std::int64_t end = frame0 + n;
            events.clear();
            std::fill (out, out + n, 0.0f);
            if (running)
            {
                while (seq.frame < (double) end)
                {
                    events.push_back (seq.next());
                    const auto& ev = events.back();
                    if (sounds == nullptr || ev.muted) continue;
                    const auto& g = gains;
                    const Sounds& snd = *sounds;
                    if (ev.level > 0)
                    {
                        // a set with an alt sound (kick and hat) plays it on counts 2, 4, 6...
                        const bool alt = ! snd.alt.empty() && ev.count % 2 == 1;
                        voice (alt ? &snd.alt : ev.level == 3 ? &snd.accent : &snd.beat, roundHalfUp (ev.frame),
                               ev.level == 3 ? g.accent : ev.level == 2 ? g.beat : g.beat * 0.4);
                        for (size_t k = 1; k < ev.subs.size(); ++k) voice (&snd.sub, roundHalfUp (ev.frame + ev.subs[k] * ev.len), g.sub);
                    }
                    for (const auto& p : ev.poly) voice (&snd.poly, roundHalfUp (ev.frame + p.frac * ev.len), g.poly);
                }
            }
            size_t keep = 0;
            for (size_t vi = 0; vi < voices.size(); ++vi)
            {
                auto& v = voices[vi];
                const auto len = (std::int64_t) v.buf->size();
                if (v.at < end)
                {
                    const std::int64_t from = std::max (v.at, frame0), stop = std::min (end, v.at + len);
                    for (std::int64_t f = from; f < stop; ++f)
                        out[f - frame0] = (float) (out[f - frame0] + (double) (*v.buf)[(size_t) (f - v.at)] * v.gain);
                }
                if (v.at + len > end)
                {
                    if (keep != vi) voices[keep] = std::move (v);
                    ++keep;
                }
            }
            voices.erase (voices.begin() + (std::ptrdiff_t) keep, voices.end());
            return events;
        }

    private:
        struct Voice
        {
            std::shared_ptr<const Sounds> owner;
            const Buffer* buf;
            std::int64_t at;
            double gain;
        };
        std::vector<Voice> voices;
        std::vector<CountEvent> events;

        void voice (const Buffer* buf, double at, double gain)
        {
            if (gain > 0) voices.push_back ({ sounds, buf, (std::int64_t) at, gain });
        }
    };
}
