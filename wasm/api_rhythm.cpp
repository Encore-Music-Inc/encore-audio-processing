// The browser's rhythm tools (rh_*): plain C functions around dsp/Tempo.h
// (tap tempo and the tempo finder), dsp/Onsets.h (the onset detector) and
// dsp/TimingCheck.h (latency, matching notes to the beat grid, and the
// report's numbers). Built with the other wasm/api_*.cpp files into
// master.wasm by wasm/build.sh.
//
// Buffers are memory from malloc. Objects are made by *_create and freed by
// *_destroy. Results with several numbers are written to an `out` array of
// doubles, in the order each function lists.
#include <emscripten/emscripten.h>
#include <vector>
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
    // the tempo, where plausible). Returns 0 until 4 seconds are in.
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
