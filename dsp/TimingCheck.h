// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// How well someone plays in time with a click, from the onsets heard
// (dsp/Onsets.h) and the beat grid the click was played on:
//
//   measureLatency  the round trip from the speakers back to the mic, from
//                   where clicks were sent and where onsets were heard.
//   TimingCheck     matches each onset to the nearest grid position (a beat
//                   or a subdivision), shifted by that round trip: how many ms
//                   early or late each note was.
//   summarize       the numbers of the report: mean, spread and RMS error, a
//                   score out of 100, each position's rush or drag, whether the
//                   mic is hearing the click itself, drift over the take, and
//                   the tempo actually played in the silent (gap) bars.
//
// The grid is plain input: the caller gives each count's frame and length
// and where inside it the positions fall, so this works with any sequencer.
// The words of a report are the product's to write. Plain C++ with no JUCE.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>
#include "dsp/Stats.h"

namespace vs::timing
{
    // ---- latency: the click goes out of the speakers, round to the mic, and comes back this late

    struct LatencyResult
    {
        bool ok = false;      // most clicks were heard, and consistently
        double ms = 0.0;
        double spreadMs = 0.0;
        int matched = 0;
        int total = 0;
    };

    // The round-trip delay from where the clicks were sent and the onsets
    // heard (both in frames). Looks for the delay most clicks agree on (the
    // 3 ms window holding the most click-to-onset delays), so a cough or a
    // missed click doesn't move it, then the median of the delays near it.
    // minS / maxS: the range of delays to consider, in seconds. ok: at least
    // 60% of the clicks were heard and their spread is within maxSpreadMs.
    inline LatencyResult measureLatency (const double* clicks, int nClicks, const double* onsets, int nOnsets, double sr,
                                         double minS = -0.05, double maxS = 0.5, double maxSpreadMs = 3)
    {
        LatencyResult r;
        r.total = nClicks;
        std::vector<double> d;
        for (int c = 0; c < nClicks; ++c)
            for (int o = 0; o < nOnsets; ++o)
            {
                const double x = (onsets[o] - clicks[c]) / sr;
                if (x >= minS && x <= maxS) d.push_back (x * 1000);
            }
        if (d.empty()) return r;
        std::sort (d.begin(), d.end());
        int best = 0, bi = 0;
        for (int i = 0, j = 0; i < (int) d.size(); ++i)
        {
            while (d[(size_t) i] - d[(size_t) j] > 3) ++j;
            if (i - j + 1 > best) { best = i - j + 1; bi = j; }
        }
        const double centre = d[(size_t) bi] + 1.5;
        const double win = std::max (3.0, maxSpreadMs * 2);
        std::vector<double> near;
        for (double x : d)
            if (std::abs (x - centre) <= win) near.push_back (x);
        r.ms = stats::median (near.data(), (int) near.size());
        double s = 0.0;
        for (double x : near) s += (x - r.ms) * (x - r.ms);
        r.spreadMs = std::sqrt (s / (double) near.size());
        r.matched = (int) near.size();
        r.ok = r.matched >= (int) std::ceil (nClicks * 0.6) && r.spreadMs <= maxSpreadMs;
        return r;
    }

    // ---- matching onsets to the beat grid

    // One count (beat) of the grid as the click played it.
    struct Count
    {
        double frame = 0.0;   // where it starts, in (fractional) frames
        double len = 0.0;     // its length in frames
        int bar = 0;          // -1 during a count-in
        int count = 0;        // which count of the bar, from 0
        int level = 2;        // its accent level: 0 silent, 1 soft, 2 normal, 3 accent
        bool countIn = false;
        bool gap = false;     // a silent bar (gap click)
        bool muted = false;   // muted at random
        bool subdivided = false; // the click plays subdivisions in this count
        double bpm = 0.0;
    };

    // A grid position a note can be aimed at.
    struct GridPoint
    {
        double frame = 0.0;   // expected arrival frame (count frame + round trip)
        int bar = 0, count = 0, sub = 0, nsub = 1;
        bool gap = false, muted = false, countIn = false;
        double bpm = 0.0;
        bool clickAt = false; // a click sounded here
    };

    // One note heard, matched to its grid position.
    struct Hit
    {
        double frame = 0.0;
        double at = 0.0;      // seconds from the first count after the count-in to the grid position
        double err = 0.0;     // ms off the grid: negative early, positive late
        int bar = 0, count = 0, sub = 0, nsub = 1;
        bool gap = false, muted = false, countIn = false;
        double bpm = 0.0;
        bool clickAt = false;
        int gridIndex = 0;
    };

    // Takes the counts as the click plays them and the onsets heard, and works
    // out how early or late each note was against the nearest grid position.
    // latencyMs: the calibrated round trip. No note more than 150 ms off, or
    // more than 35% of the way to the neighbouring position, counts as aimed
    // at it; two notes on one position keep the closer.
    class TimingCheck
    {
    public:
        explicit TimingCheck (double sampleRate, double latencyMs = 0)
            : sr (sampleRate), lat ((latencyMs * sampleRate) / 1000), cap (0.15 * sampleRate)
        {
        }

        // A count, with where the positions to hold notes to fall inside it,
        // as fractions of the count (the first is 0). In order.
        void addCount (const Count& c, const double* fractions, int nFractions)
        {
            if (! started && ! c.countIn) { started = true; startFrame = c.frame; }
            for (int k = 0; k < nFractions; ++k)
            {
                GridPoint p;
                p.frame = c.frame + fractions[k] * c.len + lat;
                p.bar = c.bar;
                p.count = c.count;
                p.sub = k;
                p.nsub = nFractions;
                p.gap = c.gap;
                p.muted = c.muted;
                p.countIn = c.countIn;
                p.bpm = c.bpm;
                p.clickAt = c.level > 0 && ! c.muted && (k == 0 || c.subdivided);
                grid.push_back (p);
            }
            knownUntil = c.frame + c.len + lat;
            resolve();
        }

        // Onsets heard (frames, in order)
        void addOnsets (const double* frames, int n)
        {
            for (int i = 0; i < n; ++i) waiting.push_back (frames[i]);
            resolve();
        }

        // Matches the onsets whose neighbourhood of the grid is known; all of them when final.
        void resolve (bool final = false)
        {
            const auto& g = grid;
            size_t w = 0;
            while (w < waiting.size() && (final || waiting[w] + cap < knownUntil))
            {
                const double o = waiting[w++];
                // the nearest grid point (binary search)
                int lo = 0, hi = (int) g.size() - 1;
                if (hi < 0) { ++extra; continue; }
                while (hi - lo > 1)
                {
                    const int mid = (lo + hi) >> 1;
                    if (g[(size_t) mid].frame <= o) lo = mid;
                    else hi = mid;
                }
                const int i = std::abs (g[(size_t) lo].frame - o) <= std::abs (g[(size_t) hi].frame - o) ? lo : hi;
                const GridPoint& p = g[(size_t) i];
                const double err = o - p.frame;
                // how far off still counts as aimed at this point: 35% of the way to the neighbour on that side
                const int nb = err >= 0 ? i + 1 : i - 1;
                const double room = nb >= 0 && nb < (int) g.size() ? std::abs (g[(size_t) nb].frame - p.frame) * 0.35 : cap;
                if (std::abs (err) > std::min (cap, room)) { ++extra; continue; }
                Hit h;
                h.frame = o;
                h.at = (p.frame - lat - (started ? startFrame : p.frame)) / sr;
                h.err = (err * 1000) / sr;
                h.bar = p.bar;
                h.count = p.count;
                h.sub = p.sub;
                h.nsub = p.nsub;
                h.gap = p.gap;
                h.muted = p.muted;
                h.countIn = p.countIn;
                h.bpm = p.bpm;
                h.clickAt = p.clickAt;
                h.gridIndex = i;
                auto had = byGrid.find (i);
                if (had != byGrid.end())
                {
                    ++extra;
                    if (std::abs (had->second) <= std::abs (h.err)) continue;
                    for (size_t j = 0; j < hits.size(); ++j)
                        if (hits[j].gridIndex == i) { hits.erase (hits.begin() + (std::ptrdiff_t) j); break; }
                }
                byGrid[i] = h.err;
                hits.push_back (h);
            }
            waiting.erase (waiting.begin(), waiting.begin() + (std::ptrdiff_t) w);
        }

        const std::vector<Hit>& matched() const { return hits; }
        const std::vector<GridPoint>& positions() const { return grid; }
        // Onsets that were not aimed at any position, or lost to a closer one
        int extras() const { return extra; }

    private:
        double sr, lat, cap;
        std::vector<GridPoint> grid;
        std::vector<double> waiting;
        std::vector<Hit> hits;
        std::map<int, double> byGrid;   // grid index -> its hit's error
        int extra = 0;
        double knownUntil = -std::numeric_limits<double>::infinity();
        bool started = false;
        double startFrame = 0.0;
    };

    // ---- the report's numbers

    // How one place in the bar was played, with the click.
    struct PositionReport
    {
        int count = 0, sub = 0, nsub = 1;
        int n = 0;
        double mean = 0.0, spread = 0.0;
    };

    // How the silent (gap) bars went.
    struct GapReport
    {
        int sections = 0;     // silent stretches with 3 or more notes
        int n = 0;            // notes in silent bars
        double mean = 0.0, spread = 0.0;
        double endErr = 0.0;  // ms off the click's average by the end of each silent stretch
        double playedBpm = 0.0; // the tempo actually played in the silent bars
        double bpm = 0.0;     // the click's tempo there
    };

    struct TimeReport
    {
        int n = 0;               // notes after the count-in
        bool enough = false;     // 4 or more notes: the rest is filled in
        double mean = 0.0;       // ms, negative early (rushing)
        double spread = 0.0;
        double rms = 0.0;
        int score = 0;           // out of 100: 100 exp (-rms / 35), rounded
        double clickMean = 0.0;  // the mean with the click (not in silent or muted bars)
        double clickSpread = 0.0; // the spread with the click (all notes when fewer than 4)
        std::vector<PositionReport> positions;  // by count, then subdivision
        std::vector<int> oddPositions;  // up to 2 positions played clearly off the rest, the oddest first
        bool bleed = false;      // the mic is probably hearing the click itself
        bool hasTrend = false;   // drift over the take, with the click (12 or more notes)
        double trendMsPerMin = 0.0;
        double trendChange = 0.0; // ms, from the first note with the click to the last
        double trendSeconds = 0.0; // the time between them
        bool hasGap = false;
        GapReport gap;
    };

    // The report's numbers from the matched notes (count-in notes are left out).
    inline TimeReport summarize (const std::vector<Hit>& allHits)
    {
        using stats::mean;
        using stats::standardDeviation;
        TimeReport out;
        std::vector<Hit> hits;
        for (const auto& h : allHits)
            if (! h.countIn) hits.push_back (h);
        out.n = (int) hits.size();
        if (hits.size() < 4) return out;
        out.enough = true;
        auto errsOf = [] (const std::vector<Hit>& hs)
        {
            std::vector<double> e;
            e.reserve (hs.size());
            for (const auto& h : hs) e.push_back (h.err);
            return e;
        };
        const auto errs = errsOf (hits);
        const int ne = (int) errs.size();
        out.mean = mean (errs.data(), ne);
        out.spread = standardDeviation (errs.data(), ne);
        std::vector<double> sq;
        for (double e : errs) sq.push_back (e * e);
        out.rms = std::sqrt (mean (sq.data(), ne));
        out.score = (int) std::floor (100 * std::exp (-out.rms / 35) + 0.5);
        // with the click (not the silent bars, where drifting is the point)
        std::vector<Hit> clicked;
        for (const auto& h : hits)
            if (! h.gap && ! h.muted) clicked.push_back (h);
        const auto clickedErrs = errsOf (clicked);
        out.clickMean = clicked.empty() ? out.mean : mean (clickedErrs.data(), (int) clickedErrs.size());
        out.clickSpread = clicked.size() >= 4 ? standardDeviation (clickedErrs.data(), (int) clickedErrs.size()) : out.spread;
        // each beat position, measured with the click
        struct Pos { int count, sub, nsub; std::vector<double> errs; };
        std::vector<Pos> pos;
        for (const auto& h : clicked.size() >= 4 ? clicked : hits)
        {
            auto it = std::find_if (pos.begin(), pos.end(), [&] (const Pos& p) { return p.count == h.count && p.sub == h.sub; });
            if (it == pos.end()) { pos.push_back ({ h.count, h.sub, h.nsub, {} }); it = pos.end() - 1; }
            it->errs.push_back (h.err);
        }
        std::stable_sort (pos.begin(), pos.end(), [] (const Pos& a, const Pos& b) { return a.count != b.count ? a.count < b.count : a.sub < b.sub; });
        for (const auto& p : pos)
        {
            PositionReport r;
            r.count = p.count;
            r.sub = p.sub;
            r.nsub = p.nsub;
            r.n = (int) p.errs.size();
            r.mean = mean (p.errs.data(), r.n);
            r.spread = standardDeviation (p.errs.data(), r.n);
            out.positions.push_back (r);
        }
        // positions rushed or dragged against the rest: 4 or more notes, 6 ms off, 5 ms from the average
        for (int i = 0; i < (int) out.positions.size(); ++i)
        {
            const auto& p = out.positions[(size_t) i];
            if (p.n >= 4 && std::abs (p.mean) >= 6 && std::abs (p.mean - out.clickMean) >= 5) out.oddPositions.push_back (i);
        }
        std::stable_sort (out.oddPositions.begin(), out.oddPositions.end(), [&] (int a, int b)
        {
            return std::abs (out.positions[(size_t) b].mean - out.clickMean) < std::abs (out.positions[(size_t) a].mean - out.clickMean);
        });
        if (out.oddPositions.size() > 2) out.oddPositions.resize (2);
        // the mic hearing the click: hits too perfect to be a person (people spread at least a few ms)
        std::vector<double> onClick;
        for (const auto& h : clicked)
            if (h.clickAt) onClick.push_back (h.err);
        const int nc = (int) onClick.size();
        if (nc >= 8 && standardDeviation (onClick.data(), nc) < 1.5 && std::abs (mean (onClick.data(), nc)) < 1.5) out.bleed = true;
        // drifting over the take (with the click)
        if (clicked.size() >= 12)
        {
            std::vector<double> at;
            for (const auto& h : clicked) at.push_back (h.at);
            const auto f = stats::fitLine (at.data(), clickedErrs.data(), (int) at.size());
            const double t0 = clicked.front().at, t1 = clicked.back().at;
            out.hasTrend = true;
            out.trendMsPerMin = f.slope * 60;
            out.trendChange = f.at (t1) - f.at (t0);
            out.trendSeconds = t1 - t0;
        }
        // the silent bars: did the tempo hold with no click?
        std::vector<Hit> gapHits;
        for (const auto& h : hits)
            if (h.gap) gapHits.push_back (h);
        if (gapHits.size() >= 3)
        {
            // split into the separate silent stretches (a new stretch when bars jump)
            std::vector<std::vector<Hit>> sections;
            for (const auto& h : gapHits)
            {
                if (! sections.empty() && h.bar - sections.back().back().bar <= 1) sections.back().push_back (h);
                else sections.push_back ({ h });
            }
            double endErr = 0.0, ratio = 0.0;
            int usable = 0;
            for (const auto& s : sections)
            {
                if (s.size() < 3) continue;
                std::vector<double> at, err;
                for (const auto& h : s) { at.push_back (h.at); err.push_back (h.err); }
                const auto f = stats::fitLine (at.data(), err.data(), (int) at.size());
                endErr += f.at (s.back().at) - out.clickMean;
                ratio += 1 + f.slope / 1000;   // ms of drift per second played: the beat was this much longer
                ++usable;
            }
            if (usable > 0)
            {
                endErr /= usable;
                ratio /= usable;
                const auto ge = errsOf (gapHits);
                out.hasGap = true;
                out.gap.sections = usable;
                out.gap.n = (int) gapHits.size();
                out.gap.mean = mean (ge.data(), (int) ge.size());
                out.gap.spread = standardDeviation (ge.data(), (int) ge.size());
                out.gap.endErr = endErr;
                out.gap.bpm = gapHits.front().bpm;
                out.gap.playedBpm = out.gap.bpm / ratio;
            }
        }
        return out;
    }
}
