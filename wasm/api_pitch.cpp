// The browser's pitch tools (pi_*): plain C functions around vs::Yin (a
// singing voice), vs::VoiceTracker and the pitch-track measurements in
// vs::voice, vs::NoteFinder and vs::timingReport, and the drum tuner
// (vs::drums). Built with the other wasm/api_*.cpp files into master.wasm by
// wasm/build.sh.
//
// Every pointer is memory from malloc. A pitch track crosses as two arrays of
// n doubles, its times (seconds) and its MIDI notes (NaN: no voice). A missing
// number comes back as NaN.
#include <cmath>
#include <limits>
#include <vector>
#include <emscripten/emscripten.h>
#include "dsp/DrumPitch.h"
#include "dsp/VoiceTracker.h"
#include "dsp/Yin.h"

namespace
{
    constexpr double missing = std::numeric_limits<double>::quiet_NaN();

    std::vector<vs::voice::PitchFrame> track (const double* t, const double* midi, int n)
    {
        std::vector<vs::voice::PitchFrame> f;
        for (int i = 0; i < n; ++i) f.push_back ({ t[i], midi[i] });
        return f;
    }

    std::vector<vs::voice::PatternNote> patternNotes (const double* notes, int n)
    {
        std::vector<vs::voice::PatternNote> p;
        for (int i = 0; i < n; ++i) p.push_back ({ notes[3 * i], notes[3 * i + 1], notes[3 * i + 2] });
        return p;
    }

    double orNan (const std::optional<double>& v) { return v ? *v : missing; }

    void writeScored (const std::vector<vs::voice::ScoredNote>& s, double* out)
    {
        for (size_t i = 0; i < s.size(); ++i)
        {
            out[4 * i] = s[i].midi;
            out[4 * i + 1] = orNan (s[i].sung);
            out[4 * i + 2] = orNan (s[i].cents);
            out[4 * i + 3] = s[i].octaves;
        }
    }
}

extern "C"
{
    // A singing voice's pitch in x[0..n) (vs::Yin::voice). out: 3 doubles, the
    // Hz (0: no clear note), the clarity (1 = perfectly periodic) and the RMS.
    // candidates: up to maxCandidates pairs (Hz, depth), every dip found.
    // Returns how many candidates there are (more than written when over maxCandidates).
    EMSCRIPTEN_KEEPALIVE int pi_voice (const float* x, int n, double sampleRate, double minHz, double maxHz, double* out, double* candidates, int maxCandidates)
    {
        vs::Yin yin (vs::Yin::voice (minHz, maxHz));
        const auto& r = yin.analyse (x, n, sampleRate);
        out[0] = r.hz; out[1] = r.clarity; out[2] = r.rms;
        const int count = (int) r.candidates.size();
        for (int i = 0; i < count && i < maxCandidates; ++i)
        {
            candidates[2 * i] = r.candidates[(size_t) i].hz;
            candidates[2 * i + 1] = r.candidates[(size_t) i].d;
        }
        return count;
    }

    // A voice tracker (vs::VoiceTracker) with these options; free it with pi_tracker_destroy.
    EMSCRIPTEN_KEEPALIVE vs::VoiceTracker* pi_tracker_create (double minHz, double maxHz, double gate, double minClarity, double window, double a4)
    {
        vs::VoiceTracker::Options o;
        o.minHz = minHz; o.maxHz = maxHz; o.gate = gate; o.minClarity = minClarity; o.window = window; o.a4 = a4;
        return new vs::VoiceTracker (o);
    }

    EMSCRIPTEN_KEEPALIVE void pi_tracker_destroy (vs::VoiceTracker* h) { delete h; }
    EMSCRIPTEN_KEEPALIVE void pi_tracker_reset (vs::VoiceTracker* h) { h->reset(); }

    // The newest samples x[0..n) at time t (seconds). out: 5 doubles, t, the MIDI
    // note with its fraction (NaN: no voice), Hz, clarity, RMS.
    EMSCRIPTEN_KEEPALIVE void pi_tracker_feed (vs::VoiceTracker* h, const float* x, int n, double sampleRate, double t, double* out)
    {
        const auto f = h->feed (x, n, sampleRate, t);
        out[0] = f.t; out[1] = f.midi; out[2] = f.hz; out[3] = f.clarity; out[4] = f.rms;
    }

    // Where a voice sits comfortably. out: 3 doubles, lo, hi, seconds. Returns 0
    // with under 3 seconds of steady singing.
    EMSCRIPTEN_KEEPALIVE int pi_tessitura (const double* t, const double* midi, int n, double* out)
    {
        const auto r = vs::voice::tessitura (track (t, midi, n));
        if (! r) return 0;
        out[0] = r->lo; out[1] = r->hi; out[2] = r->seconds;
        return 1;
    }

    // A range test (vs::voice::analyzeRange); tessN < 0: no singing for the
    // tessitura. out: 7 doubles, low, high, lowExact, highExact, tessLo, tessHi,
    // tessMeasured (0 or 1). Returns 0 when there wasn't enough singing.
    EMSCRIPTEN_KEEPALIVE int pi_analyze_range (const double* t, const double* midi, int n, const double* tessT, const double* tessMidi, int tessN, double* out)
    {
        const auto tess = tessN >= 0 ? track (tessT, tessMidi, tessN) : std::vector<vs::voice::PitchFrame> {};
        const auto r = vs::voice::analyzeRange (track (t, midi, n), tessN >= 0 ? &tess : nullptr);
        if (! r) return 0;
        out[0] = r->low; out[1] = r->high; out[2] = r->lowExact; out[3] = r->highExact;
        out[4] = r->tessLo; out[5] = r->tessHi; out[6] = r->tessMeasured ? 1 : 0;
        return 1;
    }

    // A held note (vs::voice::analyzeSustain); target NaN: none. out: 10
    // doubles, seconds, meanMidi, drift, wobble, vibrato (0 or 1), its rate,
    // width and regularity, cents (NaN without a target), octaves. Returns 0 when
    // there's no held note long enough.
    EMSCRIPTEN_KEEPALIVE int pi_analyze_sustain (const double* t, const double* midi, int n, double target, double* out)
    {
        const auto r = vs::voice::analyzeSustain (track (t, midi, n), std::isnan (target) ? std::nullopt : std::optional<double> (target));
        if (! r) return 0;
        out[0] = r->seconds; out[1] = r->meanMidi; out[2] = r->drift; out[3] = r->wobble;
        out[4] = r->vibrato ? 1 : 0;
        out[5] = r->vibrato ? r->vibrato->rate : missing;
        out[6] = r->vibrato ? r->vibrato->width : missing;
        out[7] = r->vibrato ? r->vibrato->regularity : missing;
        out[8] = orNan (r->cents); out[9] = r->octaves;
        return 1;
    }

    // The notes sung (vs::voice::segmentNotes). out: up to maxNotes triples,
    // start, end, MIDI. Returns how many notes there are.
    EMSCRIPTEN_KEEPALIVE int pi_segment_notes (const double* t, const double* midi, int n, double minDur, double* out, int maxNotes)
    {
        const auto s = vs::voice::segmentNotes (track (t, midi, n), minDur);
        for (int i = 0; i < (int) s.size() && i < maxNotes; ++i)
        {
            out[3 * i] = s[(size_t) i].start; out[3 * i + 1] = s[(size_t) i].end; out[3 * i + 2] = s[(size_t) i].midi;
        }
        return (int) s.size();
    }

    // One try at matching a note (vs::voice::scoreMatch). out: 7 doubles, heard
    // (0 or 1), cents, settle (NaN: never settled), steadiness, octaves, sungMidi, score.
    EMSCRIPTEN_KEEPALIVE void pi_score_match (const double* t, const double* midi, int n, double target, double* out)
    {
        const auto r = vs::voice::scoreMatch (track (t, midi, n), target);
        out[0] = r.heard ? 1 : 0; out[1] = r.cents; out[2] = orNan (r.settle); out[3] = r.steadiness;
        out[4] = r.octaves; out[5] = r.sungMidi; out[6] = r.score;
    }

    // A pattern's notes against what was sung (vs::voice::scoreNotes). notes:
    // nNotes triples, MIDI, start, length. out: nNotes quads, MIDI, sung, cents
    // (both NaN: not heard), octaves.
    EMSCRIPTEN_KEEPALIVE void pi_score_notes (const double* t, const double* midi, int n, const double* notes, int nNotes, double* out)
    {
        writeScored (vs::voice::scoreNotes (track (t, midi, n), patternNotes (notes, nNotes)), out);
    }

    // As pi_score_notes, forgiving early or late singing (vs::voice::scorePattern).
    // Returns the timing chosen, in seconds.
    EMSCRIPTEN_KEEPALIVE double pi_score_pattern (const double* t, const double* midi, int n, const double* notes, int nNotes, double* out)
    {
        const auto r = vs::voice::scorePattern (track (t, midi, n), patternNotes (notes, nNotes));
        writeScored (r.notes, out);
        return r.lag;
    }

    // The numbers behind an exercise's feedback (vs::voice::notesSummary).
    // scored: n quads as pi_score_notes writes them. out: 6 doubles, heard,
    // total, good, topAvg, lowAvg, all.
    EMSCRIPTEN_KEEPALIVE void pi_notes_summary (const double* scored, int n, double* out)
    {
        std::vector<vs::voice::ScoredNote> s;
        for (int i = 0; i < n; ++i)
        {
            vs::voice::ScoredNote x { scored[4 * i], std::nullopt, std::nullopt, scored[4 * i + 3] };
            if (! std::isnan (scored[4 * i + 2])) { x.sung = scored[4 * i + 1]; x.cents = scored[4 * i + 2]; }
            s.push_back (x);
        }
        const auto r = vs::voice::notesSummary (s);
        out[0] = r.heard; out[1] = r.total; out[2] = r.good; out[3] = r.topAvg; out[4] = r.lowAvg; out[5] = r.all;
    }

    // A siren (vs::voice::scoreSiren). out: 5 doubles, coverage, lowest,
    // highest (NaN: none reached), jump, jumpAt (NaN: no jump of 2.5 semitones).
    EMSCRIPTEN_KEEPALIVE void pi_score_siren (const double* t, const double* midi, int n, double from, double to, double up, double down, double* out)
    {
        const auto r = vs::voice::scoreSiren (track (t, midi, n), { from, to, up, down });
        out[0] = r.coverage; out[1] = orNan (r.lowest); out[2] = orNan (r.highest); out[3] = r.jump; out[4] = orNan (r.jumpAt);
    }

    // A tune sung back (vs::voice::scoreSingBack). targets: nTargets MIDI notes.
    // out: octaves, score, then nTargets note triples (target, sung, cents),
    // then nTargets - 1 jump triples (semitones asked, sung, cents).
    EMSCRIPTEN_KEEPALIVE void pi_score_sing_back (const double* t, const double* midi, int n, const double* targets, int nTargets, double* out)
    {
        const auto r = vs::voice::scoreSingBack (track (t, midi, n), std::vector<double> (targets, targets + std::max (0, nTargets)));
        out[0] = r.octaves; out[1] = r.score;
        double* p = out + 2;
        for (const auto* list : { &r.notes, &r.jumps })
            for (const auto& i : *list) { p[0] = i.target; p[1] = orNan (i.sung); p[2] = orNan (i.cents); p += 3; }
    }

    // A note finder (vs::NoteFinder); free it with pi_note_finder_destroy.
    EMSCRIPTEN_KEEPALIVE vs::NoteFinder* pi_note_finder_create (int hold, int quietReset) { return new vs::NoteFinder (hold, quietReset); }
    EMSCRIPTEN_KEEPALIVE void pi_note_finder_destroy (vs::NoteFinder* h) { delete h; }

    // One reading (present 0: none at all; midi NaN: no pitch). Returns 1 and
    // out: 2 doubles (the note, when it started) when a note is found.
    EMSCRIPTEN_KEEPALIVE int pi_note_finder_feed (vs::NoteFinder* h, int present, double t, double midi, double level, double* out)
    {
        const vs::NoteFinder::Reading r { t, midi, level };
        const auto note = h->feed (present ? &r : nullptr);
        if (! note) return 0;
        out[0] = note->midi; out[1] = note->t;
        return 1;
    }

    // Timing against a click (vs::timingReport): n note times and beat times
    // (NaN: none). offsets: one per note with both, in ms. out: 3 doubles,
    // mean, spread, onTime. Returns how many offsets (0: no report).
    EMSCRIPTEN_KEEPALIVE int pi_timing_report (const double* times, const double* beats, int n, double ok, double* offsets, double* out)
    {
        const auto r = vs::timingReport (std::vector<double> (times, times + n), std::vector<double> (beats, beats + n), ok);
        if (! r) return 0;
        for (size_t i = 0; i < r->offsets.size(); ++i) offsets[i] = r->offsets[i];
        out[0] = r->mean; out[1] = r->spread; out[2] = r->onTime;
        return (int) r->offsets.size();
    }

    // The main note of a drum hit in Hz (vs::drums::drumPeak), 0 when there isn't one.
    EMSCRIPTEN_KEEPALIVE double pi_drum_peak (const float* x, int n, double sampleRate, double minHz, double maxHz, int lowest, int size)
    {
        return vs::drums::drumPeak (x, n, sampleRate, minHz, maxHz, lowest != 0, size);
    }

    // The rods' pitches hz[0..n) (0: not measured): returns their geometric
    // mean; cents: n doubles, each rod's cents from it (NaN: not measured).
    EMSCRIPTEN_KEEPALIVE double pi_rod_spread (const double* hz, int n, double* cents)
    {
        const auto r = vs::drums::rodSpread (std::vector<double> (hz, hz + std::max (0, n)));
        for (int i = 0; i < n; ++i) cents[i] = r.cents[(size_t) i];
        return r.mean;
    }

    // Notes for count toms tuned in steps, into out[0..count).
    EMSCRIPTEN_KEEPALIVE void pi_tom_steps (double top, int count, double step, double* out)
    {
        const auto s = vs::drums::tomSteps (top, count, step);
        for (int i = 0; i < count; ++i) out[i] = s[(size_t) i];
    }
}
