// The browser's rhythm tools (rh_*): plain C functions around dsp/Tempo.h
// (tap tempo and the tempo finder), dsp/Onsets.h (the onset detector),
// dsp/TimingCheck.h (latency, matching notes to the beat grid, and the
// report's numbers) and the beat grid's meter maths in dsp/Metronome.h
// (groupings, accents, note values, subdivisions, tempo names). Built with
// the other wasm/api_*.cpp files into master.wasm by wasm/build.sh.
//
// Buffers are memory from malloc. Objects are made by *_create and freed by
// *_destroy. Results with several numbers are written to an `out` array of
// doubles, in the order each function lists. A list of whole numbers (a
// grouping such as 2+2+3, an accent pattern) crosses as ints, with its length
// beside it; a function that writes one into out (room for cap ints) returns
// how many it wrote, or -1 when they don't fit. Text is written to out
// (cap bytes) and ends with a 0 byte; those functions return its length, or
// -1 when it doesn't fit.
#include <emscripten/emscripten.h>
#include <cstring>
#include <string>
#include <vector>
#include "dsp/Metronome.h"
#include "dsp/Onsets.h"
#include "dsp/Tempo.h"
#include "dsp/TimingCheck.h"

namespace
{
    int putFit (const vs::tempo::TapFit& f, double* out)
    {
        out[0] = f.bpm;
        out[1] = f.taps;
        out[2] = f.used;
        out[3] = f.spreadMs;
        return f.valid ? 1 : 0;
    }

    struct Finder
    {
        vs::tempo::TempoFinder finder;
        std::vector<float> in;
        Finder (double sr, int maxBlock) : finder (sr), in ((size_t) maxBlock, 0.0f) {}
    };

    struct Detector
    {
        vs::OnsetDetector detector;
        std::vector<float> in;
        std::vector<double> found;
        Detector (double sr, double sensitivity, double refractoryMs, int maxBlock)
            : detector (sr, sensitivity, refractoryMs), in ((size_t) maxBlock, 0.0f) {}
    };

    struct Check
    {
        vs::timing::TimingCheck check;
        vs::timing::TimeReport report;
        Check (double sr, double latencyMs) : check (sr, latencyMs) {}
    };

    int putInts (const std::vector<int>& v, int* out, int cap)
    {
        if ((int) v.size() > cap) return -1;
        std::copy (v.begin(), v.end(), out);
        return (int) v.size();
    }

    int putText (const std::string& s, char* out, int cap)
    {
        if ((int) s.size() + 1 > cap) { if (cap > 0) out[0] = 0; return -1; }
        std::memcpy (out, s.c_str(), s.size() + 1);
        return (int) s.size();
    }

    std::vector<int> ints (const int* x, int n) { return std::vector<int> (x, x + std::max (0, n)); }
}

extern "C"
{
    // ---- tap tempo

    // Tempo from n tap times (ms), needing 3 taps. out: bpm, taps, taps used,
    // spread (ms). Returns 1, or 0 with too few taps.
    EMSCRIPTEN_KEEPALIVE int rh_tap_tempo (const double* times, int n, double* out)
    {
        return putFit (vs::tempo::tapTempo (times, n), out);
    }

    // A live tap button: a gap over resetMs starts over; the fit uses the
    // latest keep taps (0: all since the last pause).
    EMSCRIPTEN_KEEPALIVE void* rh_tapper_create (double resetMs, int keep) { return new vs::tempo::TapTempo (resetMs, keep); }
    EMSCRIPTEN_KEEPALIVE void rh_tapper_destroy (void* p) { delete static_cast<vs::tempo::TapTempo*> (p); }
    EMSCRIPTEN_KEEPALIVE void rh_tapper_reset (void* p) { static_cast<vs::tempo::TapTempo*> (p)->reset(); }

    // A tap at ms. out as rh_tap_tempo; returns 1 from the second tap on.
    EMSCRIPTEN_KEEPALIVE int rh_tapper_tap (void* p, double ms, double* out)
    {
        return putFit (static_cast<vs::tempo::TapTempo*> (p)->tap (ms), out);
    }

    // ---- the beat grid's meters: a bar is top counts of 1/bottom notes, felt in groups

    // The groupings worth offering for a meter (every count, the even ones,
    // then the 2s-and-3s mixes), into out as each grouping's length followed
    // by its groups ("1+1+1+1" and "2+2" for 4/4: 4 1 1 1 1 2 2 2). Returns
    // how many groupings, or -1 when they don't fit in cap ints.
    EMSCRIPTEN_KEEPALIVE int rh_grouping_options (int top, int bottom, int* out, int cap)
    {
        int used = 0;
        const auto options = vs::metronome::groupingOptions (top, bottom);
        for (const auto& g : options)
        {
            if (used + 1 + (int) g.size() > cap) return -1;
            out[used++] = (int) g.size();
            for (int v : g) out[used++] = v;
        }
        return (int) options.size();
    }

    // How a meter is felt unless you say otherwise (6/8 in threes, 7/8 as 2+2+3, x/4 on every count)
    EMSCRIPTEN_KEEPALIVE int rh_default_groups (int top, int bottom, int* out, int cap)
    {
        return putInts (vs::metronome::defaultGroups (top, bottom), out, cap);
    }

    // Reads a grouping ("3+2+2", any non-digits between the numbers) from
    // text (0-terminated, from malloc). Returns its length, or 0 unless it is
    // whole numbers of at least 1 adding up to top.
    EMSCRIPTEN_KEEPALIVE int rh_parse_groups (const char* text, int top, int* out, int cap)
    {
        const auto g = vs::metronome::parseGroups (text, top);
        return g ? putInts (*g, out, cap) : 0;
    }

    // The accent pattern a grouping starts with (3 the downbeat, 2 each
    // group's first count, 1 the counts inside a group), one per count
    EMSCRIPTEN_KEEPALIVE int rh_default_accents (const int* groups, int nGroups, int* out, int cap)
    {
        return putInts (vs::metronome::defaultAccents (ints (groups, nGroups)), out, cap);
    }

    // For each count of the bar, two ints: which group it is in, and 1 when
    // it starts that group. Returns how many counts (cap: room for 2 ints each).
    EMSCRIPTEN_KEEPALIVE int rh_group_map (const int* groups, int nGroups, int* out, int cap)
    {
        const auto map = vs::metronome::groupMap (ints (groups, nGroups));
        if (2 * (int) map.size() > cap) return -1;
        for (size_t i = 0; i < map.size(); ++i) { out[2 * i] = map[i].group; out[2 * i + 1] = map[i].start ? 1 : 0; }
        return (int) map.size();
    }

    // The note a beat of units counts of 1/bottom is ("8th", "dotted quarter", "5 × 8th")
    EMSCRIPTEN_KEEPALIVE int rh_note_value_name (int units, int bottom, char* out, int cap)
    {
        return putText (vs::metronome::noteValueName (units, bottom), out, cap);
    }

    // What the tempo can count, as counts per beat (1, the written note;
    // then the felt beat when every group is the same size): into out, room
    // for 2. Each one's name is rh_note_value_name (units, bottom).
    EMSCRIPTEN_KEEPALIVE int rh_unit_options (const int* groups, int nGroups, int bottom, int* out)
    {
        const auto options = vs::metronome::unitOptions (ints (groups, nGroups), bottom);
        for (size_t i = 0; i < options.size(); ++i) out[i] = options[i].units;
        return (int) options.size();
    }

    // Where the clicks inside one count fall, as fractions of it (the first
    // 0), for a subdivision id ("none", "8", "3", "16", "5", "6"; an unknown
    // id is none) and swing (50 straight to 75): into out, room for 6.
    // Returns how many.
    EMSCRIPTEN_KEEPALIVE int rh_sub_fractions (const char* id, double swing, double* out)
    {
        const auto f = vs::metronome::subFractions (id, swing);
        std::copy (f.begin(), f.end(), out);
        return (int) f.size();
    }

    // The Italian name for a tempo ("Andante")
    EMSCRIPTEN_KEEPALIVE int rh_tempo_name (double bpm, char* out, int cap)
    {
        return putText (vs::metronome::tempoName (bpm), out, cap);
    }

    // ---- the tempo finder: fill rh_tempo_input with up to maxBlock mono samples, rh_tempo_push them, repeat

    EMSCRIPTEN_KEEPALIVE void* rh_tempo_create (double sampleRate, int maxBlock) { return new Finder (sampleRate, maxBlock); }
    EMSCRIPTEN_KEEPALIVE void rh_tempo_destroy (void* p) { delete static_cast<Finder*> (p); }
    EMSCRIPTEN_KEEPALIVE float* rh_tempo_input (void* p) { return static_cast<Finder*> (p)->in.data(); }
    EMSCRIPTEN_KEEPALIVE void rh_tempo_push (void* p, int n)
    {
        auto* f = static_cast<Finder*> (p);
        f->finder.push (f->in.data(), n);
    }
    EMSCRIPTEN_KEEPALIVE double rh_tempo_seconds (void* p) { return static_cast<Finder*> (p)->finder.seconds(); }

    // The tempo between minBpm and maxBpm. out: bpm, confidence (0 to 1),
    // strength, how many others (0 to 2), then the others (half and double
    // the tempo, where plausible). Returns 0 until 4 seconds are in, and when
    // nothing in them starts (silence: no tempo).
    EMSCRIPTEN_KEEPALIVE int rh_tempo_estimate (void* p, double minBpm, double maxBpm, double* out)
    {
        const auto e = static_cast<Finder*> (p)->finder.estimate (minBpm, maxBpm);
        out[0] = e.bpm;
        out[1] = e.confidence;
        out[2] = e.strength;
        out[3] = (double) e.others.size();
        for (size_t i = 0; i < e.others.size(); ++i) out[4 + i] = e.others[i];
        return e.valid ? 1 : 0;
    }

    // ---- the onset detector: fill rh_onsets_input with up to maxBlock samples, then rh_onsets_push

    // sensitivity: 0 to 100. refractoryMs: the shortest time between onsets.
    EMSCRIPTEN_KEEPALIVE void* rh_onsets_create (double sampleRate, double sensitivity, double refractoryMs, int maxBlock)
    {
        return new Detector (sampleRate, sensitivity, refractoryMs, maxBlock);
    }
    EMSCRIPTEN_KEEPALIVE void rh_onsets_destroy (void* p) { delete static_cast<Detector*> (p); }
    EMSCRIPTEN_KEEPALIVE float* rh_onsets_input (void* p) { return static_cast<Detector*> (p)->in.data(); }

    // Takes n samples of the input, the first at sample frame `frame` (whole
    // frames). Returns how many onsets were found; rh_onsets_found points at
    // them (fractional frames) until the next push.
    EMSCRIPTEN_KEEPALIVE int rh_onsets_push (void* p, int n, double frame)
    {
        auto* d = static_cast<Detector*> (p);
        d->found.clear();
        d->detector.push (d->in.data(), n, (std::int64_t) frame, d->found);
        return (int) d->found.size();
    }
    EMSCRIPTEN_KEEPALIVE const double* rh_onsets_found (void* p) { return static_cast<Detector*> (p)->found.data(); }

    // ---- latency and the timing check

    // The round trip from clicks sent to onsets heard (frames). out: ok (0 or
    // 1), ms, spread (ms), clicks matched, clicks. Returns ok.
    EMSCRIPTEN_KEEPALIVE int rh_latency (const double* clicks, int nClicks, const double* onsets, int nOnsets, double sampleRate,
                                         double minS, double maxS, double maxSpreadMs, double* out)
    {
        const auto r = vs::timing::measureLatency (clicks, nClicks, onsets, nOnsets, sampleRate, minS, maxS, maxSpreadMs);
        out[0] = r.ok ? 1 : 0;
        out[1] = r.ms;
        out[2] = r.spreadMs;
        out[3] = r.matched;
        out[4] = r.total;
        return r.ok ? 1 : 0;
    }

    EMSCRIPTEN_KEEPALIVE void* rh_check_create (double sampleRate, double latencyMs) { return new Check (sampleRate, latencyMs); }
    EMSCRIPTEN_KEEPALIVE void rh_check_destroy (void* p) { delete static_cast<Check*> (p); }

    // A count of the grid, in order: its frame and length (frames), bar (-1
    // in the count-in), count, accent level (0 to 3), countIn, gap, muted and
    // subdivided (0 or 1), tempo, and where the positions to hold notes to
    // fall inside it (nFractions fractions of the count, the first 0).
    EMSCRIPTEN_KEEPALIVE void rh_check_add_count (void* p, double frame, double len, int bar, int count, int level, int countIn,
                                                 int gap, int muted, int subdivided, double bpm, const double* fractions, int nFractions)
    {
        vs::timing::Count c;
        c.frame = frame;
        c.len = len;
        c.bar = bar;
        c.count = count;
        c.level = level;
        c.countIn = countIn != 0;
        c.gap = gap != 0;
        c.muted = muted != 0;
        c.subdivided = subdivided != 0;
        c.bpm = bpm;
        static_cast<Check*> (p)->check.addCount (c, fractions, nFractions);
    }

    // Onsets heard (frames, in order)
    EMSCRIPTEN_KEEPALIVE void rh_check_add_onsets (void* p, const double* frames, int n)
    {
        static_cast<Check*> (p)->check.addOnsets (frames, n);
    }

    // Matches every onset still waiting (at the end of a take)
    EMSCRIPTEN_KEEPALIVE void rh_check_finish (void* p) { static_cast<Check*> (p)->check.resolve (true); }
    EMSCRIPTEN_KEEPALIVE int rh_check_extras (void* p) { return static_cast<Check*> (p)->check.extras(); }
    EMSCRIPTEN_KEEPALIVE int rh_check_hits (void* p) { return (int) static_cast<Check*> (p)->check.matched().size(); }

    // Hit i. out (12): frame, at (s), err (ms), bar, count, sub, nsub, gap,
    // muted, countIn, bpm, clickAt.
    EMSCRIPTEN_KEEPALIVE void rh_check_hit (void* p, int i, double* out)
    {
        const auto& h = static_cast<Check*> (p)->check.matched()[(size_t) i];
        const double v[] { h.frame, h.at, h.err, (double) h.bar, (double) h.count, (double) h.sub, (double) h.nsub,
                           h.gap ? 1.0 : 0.0, h.muted ? 1.0 : 0.0, h.countIn ? 1.0 : 0.0, h.bpm, h.clickAt ? 1.0 : 0.0 };
        for (int k = 0; k < 12; ++k) out[k] = v[k];
    }

    // The report's numbers for the hits so far. Returns how many positions it has.
    // out (24): n, enough, mean, spread, rms, score, clickMean, clickSpread,
    // bleed, hasTrend, trend ms per minute, trend change (ms), trend seconds,
    // hasGap, gap sections, gap n, gap mean, gap spread, gap endErr, gap
    // played BPM, gap BPM, how many odd positions (0 to 2), then the odd
    // positions' indices.
    EMSCRIPTEN_KEEPALIVE int rh_check_report (void* p, double* out)
    {
        auto* c = static_cast<Check*> (p);
        c->report = vs::timing::summarize (c->check.matched());
        const auto& r = c->report;
        const double v[] { (double) r.n, r.enough ? 1.0 : 0.0, r.mean, r.spread, r.rms, (double) r.score, r.clickMean, r.clickSpread,
                           r.bleed ? 1.0 : 0.0, r.hasTrend ? 1.0 : 0.0, r.trendMsPerMin, r.trendChange, r.trendSeconds,
                           r.hasGap ? 1.0 : 0.0, (double) r.gap.sections, (double) r.gap.n, r.gap.mean, r.gap.spread,
                           r.gap.endErr, r.gap.playedBpm, r.gap.bpm, (double) r.oddPositions.size(),
                           r.oddPositions.size() > 0 ? (double) r.oddPositions[0] : -1.0,
                           r.oddPositions.size() > 1 ? (double) r.oddPositions[1] : -1.0 };
        for (int k = 0; k < 24; ++k) out[k] = v[k];
        return (int) r.positions.size();
    }

    // Position i of the last rh_check_report. out (6): count, sub, nsub, n, mean, spread.
    EMSCRIPTEN_KEEPALIVE void rh_check_position (void* p, int i, double* out)
    {
        const auto& q = static_cast<Check*> (p)->report.positions[(size_t) i];
        out[0] = q.count;
        out[1] = q.sub;
        out[2] = q.nsub;
        out[3] = q.n;
        out[4] = q.mean;
        out[5] = q.spread;
    }
}
