// The browser's measurements (ms_*): plain C functions around the true-peak
// meter (dsp/TruePeak.h), the loudness report (dsp/LoudnessReport.h), the
// whole-file analysis (dsp/Analysis.h), the spectrum analyser
// (dsp/Spectrum.h) and the waveform overviews (dsp/Waveform.h). Built with the
// other wasm/api_*.cpp files into master.wasm by wasm/build.sh.
//
// The meters own their input buffers: fill ms_*_left and ms_*_right with up
// to maxBlock samples, then ms_*_process them, and repeat, so a song's length
// isn't limited by the module's memory.
#include <emscripten/emscripten.h>
#include <cstdint>
#include <vector>
#include "dsp/Analysis.h"
#include "dsp/LoudnessReport.h"
#include "dsp/Spectrum.h"
#include "dsp/TruePeak.h"
#include "dsp/Waveform.h"

namespace
{
    struct Input
    {
        std::vector<float> l, r;
        void make (int maxBlock) { l.assign ((size_t) maxBlock, 0.0f); r.assign ((size_t) maxBlock, 0.0f); }
    };

    struct TruePeak : Input { vs::TruePeakMeter meter; };
    struct Report : Input { vs::LoudnessReporter reporter; vs::LoudnessReport rep; };
    struct Analyse : Input
    {
        vs::FileAnalysis analysis;
        vs::analysis::Measurements m;
        std::vector<double> lists[5];
    };
    struct Spectrum : Input { vs::SpectrumAnalyser analyser; };
    struct Overview : Input
    {
        vs::waveform::Overview overview;
        std::vector<float> out;
    };
    struct Blocks : Input
    {
        vs::waveform::BlockPeaks peaks;
        std::vector<double> out;
        explicit Blocks (int size) : peaks (size) {}
    };
}

extern "C"
{
    // ---- true-peak meter (the limiter's detector, TruePeakDetector) ----

    EMSCRIPTEN_KEEPALIVE void* ms_tp_create (int channels, int maxBlock)
    {
        auto* m = new TruePeak();
        m->make (maxBlock);
        m->meter.prepare (channels, maxBlock);
        return m;
    }
    EMSCRIPTEN_KEEPALIVE void ms_tp_destroy (void* p) { delete static_cast<TruePeak*> (p); }
    EMSCRIPTEN_KEEPALIVE float* ms_tp_left (void* p)  { return static_cast<TruePeak*> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* ms_tp_right (void* p) { return static_cast<TruePeak*> (p)->r.data(); }
    // Clears the reading (a held peak) and the filters
    EMSCRIPTEN_KEEPALIVE void ms_tp_reset (void* p) { static_cast<TruePeak*> (p)->meter.reset(); }
    EMSCRIPTEN_KEEPALIVE void ms_tp_process (void* p, int n)
    {
        auto* m = static_cast<TruePeak*> (p);
        const float* ch[2] { m->l.data(), m->r.data() };
        m->meter.process (ch, n);
    }
    // At the end of a file: the upsampler's tail
    EMSCRIPTEN_KEEPALIVE void ms_tp_finish (void* p) { static_cast<TruePeak*> (p)->meter.finish(); }
    // Linear; channel -1 for the largest
    EMSCRIPTEN_KEEPALIVE float ms_tp_true_peak (void* p, int channel) { return static_cast<TruePeak*> (p)->meter.truePeak (channel); }
    EMSCRIPTEN_KEEPALIVE float ms_tp_sample_peak (void* p, int channel) { return static_cast<TruePeak*> (p)->meter.samplePeak (channel); }
    // The sample the loudest true peak belongs to
    EMSCRIPTEN_KEEPALIVE double ms_tp_position (void* p) { return (double) static_cast<TruePeak*> (p)->meter.truePeakPosition(); }

    // ---- loudness report ----

    EMSCRIPTEN_KEEPALIVE void* ms_lr_create (double sampleRate, int channels, int maxBlock)
    {
        auto* m = new Report();
        m->make (maxBlock);
        m->reporter.prepare (sampleRate, channels, maxBlock);
        return m;
    }
    EMSCRIPTEN_KEEPALIVE void ms_lr_destroy (void* p) { delete static_cast<Report*> (p); }
    EMSCRIPTEN_KEEPALIVE float* ms_lr_left (void* p)  { return static_cast<Report*> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* ms_lr_right (void* p) { return static_cast<Report*> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE void ms_lr_process (void* p, int n)
    {
        auto* m = static_cast<Report*> (p);
        m->reporter.process (m->l.data(), m->r.data(), n);
    }
    EMSCRIPTEN_KEEPALIVE void ms_lr_finish (void* p) { auto* m = static_cast<Report*> (p); m->rep = m->reporter.finish(); }

    // After ms_lr_finish. what: 0 integrated, 1 short-term max, 2 its time,
    // 3 momentary max, 4 its time, 5 loudness range, 6 balance, 7 sample peak,
    // 8 true peak, 9 its time, 10 and 11 the left and right sample peaks,
    // 12 and 13 their RMS levels, 14 the samples measured
    EMSCRIPTEN_KEEPALIVE double ms_lr_value (void* p, int what)
    {
        const auto& r = static_cast<Report*> (p)->rep;
        switch (what)
        {
            case 0: return r.integrated;
            case 1: return r.shortTermMax;
            case 2: return r.shortTermMaxAt;
            case 3: return r.momentaryMax;
            case 4: return r.momentaryMaxAt;
            case 5: return r.range;
            case 6: return r.balanceDb;
            case 7: return r.samplePeakDb;
            case 8: return r.truePeakDb;
            case 9: return r.truePeakAt;
            case 10: return r.peakDb[0];
            case 11: return r.peakDb[1];
            case 12: return r.rmsDb[0];
            case 13: return r.rmsDb[1];
            case 14: return (double) r.frames;
            default: return 0.0;
        }
    }
    // Short-term loudness once a second: ms_lr_timeline_size values
    EMSCRIPTEN_KEEPALIVE const double* ms_lr_timeline (void* p) { return static_cast<Report*> (p)->rep.timeline.data(); }
    EMSCRIPTEN_KEEPALIVE int ms_lr_timeline_size (void* p) { return (int) static_cast<Report*> (p)->rep.timeline.size(); }

    // ---- whole-file analysis (release checks) ----

    // frames: the song's length (needed first). bitDepth 0 when unknown.
    EMSCRIPTEN_KEEPALIVE void* ms_an_create (double sampleRate, int channels, double frames, int bitDepth, int isFloat, int maxBlock)
    {
        auto* m = new Analyse();
        m->make (maxBlock);
        vs::analysis::Options o;
        o.bitDepth = bitDepth;
        o.isFloat = isFloat != 0;
        m->analysis.prepare (sampleRate, channels, (std::int64_t) frames, o);
        return m;
    }
    EMSCRIPTEN_KEEPALIVE void ms_an_destroy (void* p) { delete static_cast<Analyse*> (p); }
    EMSCRIPTEN_KEEPALIVE float* ms_an_left (void* p)  { return static_cast<Analyse*> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* ms_an_right (void* p) { return static_cast<Analyse*> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE void ms_an_process (void* p, int n)
    {
        auto* m = static_cast<Analyse*> (p);
        m->analysis.process (m->l.data(), m->r.data(), n);
    }
    // Ends a pass: 1 means feed the song again (the clipping check), 0 done
    EMSCRIPTEN_KEEPALIVE int ms_an_next_pass (void* p) { return static_cast<Analyse*> (p)->analysis.nextPass() ? 1 : 0; }
    EMSCRIPTEN_KEEPALIVE void ms_an_finish (void* p)
    {
        auto* m = static_cast<Analyse*> (p);
        m->m = m->analysis.finish();
        auto& x = m->m;
        m->lists[0].assign (x.timeline.begin(), x.timeline.end());
        m->lists[1].assign (x.tpOvers.begin(), x.tpOvers.end());
        m->lists[2].assign (x.tpOverZero.begin(), x.tpOverZero.end());
        m->lists[3].assign (x.clip.times.begin(), x.clip.times.end());
        m->lists[4].assign (x.low.outTimes.begin(), x.low.outTimes.end());
    }

    // After ms_an_finish. Booleans read 0 or 1; "has" values say whether the
    // value after them was measured.
    //  0 empty            1 duration         2 samplePeakDb     3 samplePeakAt
    //  4 peakLDb          5 peakRDb          6 dcL              7 dcR
    //  8 oversFloat       9 corr            10 monoLossDb      11 sideToMidDb
    // 12 identical       13 maxDiff         14 silentL         15 silentR
    // 16 silent          17 lufs            18 shortTermMax    19 shortTermMaxAt
    // 20 lra             21 balanceDb       22 truePeakDb      23 truePeakAt
    // 24 tpOverCount     25 tpOverZeroCount 26 clip runs       27 clip events
    // 28 clip longest    29 clip has level  30 clip levelDb    31 low corr
    // 32 low monoLossDb  33 low shareDb     34 leadSec         35 tailSec
    // 36 firstSampleDb   37 lastSampleDb    38 headRmsDb       39 endRmsDb
    // 40 endDecayDb      41 has spectrum    42 cliffHz         43 cliff drop
    // 44 has padded16    45 padded16
    // 100 + 3 band + k: band 0..4 (sub, bass, low mids, mids, highs), k 0 corr, 1 monoLossDb, 2 shareDb
    EMSCRIPTEN_KEEPALIVE double ms_an_value (void* p, int what)
    {
        const auto& m = static_cast<Analyse*> (p)->m;
        if (what >= 100 && what < 100 + 3 * vs::analysis::numBands)
        {
            const auto& b = m.band[(what - 100) / 3];
            const int k = (what - 100) % 3;
            return k == 0 ? b.corr : k == 1 ? b.monoLossDb : b.shareDb;
        }
        switch (what)
        {
            case 0: return m.empty ? 1 : 0;
            case 1: return m.duration;
            case 2: return m.samplePeakDb;
            case 3: return m.samplePeakAt;
            case 4: return m.peakLDb;
            case 5: return m.peakRDb;
            case 6: return m.dcL;
            case 7: return m.dcR;
            case 8: return m.oversFloat;
            case 9: return m.corr;
            case 10: return m.monoLossDb;
            case 11: return m.sideToMidDb;
            case 12: return m.identical ? 1 : 0;
            case 13: return m.maxDiff;
            case 14: return m.silentL ? 1 : 0;
            case 15: return m.silentR ? 1 : 0;
            case 16: return m.silent ? 1 : 0;
            case 17: return m.lufs;
            case 18: return m.shortTermMax;
            case 19: return m.shortTermMaxAt;
            case 20: return m.lra;
            case 21: return m.balanceDb;
            case 22: return m.truePeakDb;
            case 23: return m.truePeakAt;
            case 24: return m.tpOverCount;
            case 25: return m.tpOverZeroCount;
            case 26: return m.clip.runs;
            case 27: return m.clip.events;
            case 28: return m.clip.longest;
            case 29: return m.clip.hasLevel ? 1 : 0;
            case 30: return m.clip.levelDb;
            case 31: return m.low.corr;
            case 32: return m.low.monoLossDb;
            case 33: return m.low.shareDb;
            case 34: return m.leadSec;
            case 35: return m.tailSec;
            case 36: return m.firstSampleDb;
            case 37: return m.lastSampleDb;
            case 38: return m.headRmsDb;
            case 39: return m.endRmsDb;
            case 40: return m.endDecayDb;
            case 41: return m.hasSpectrum ? 1 : 0;
            case 42: return m.spectrum.cliffHz;
            case 43: return m.spectrum.drop;
            case 44: return m.hasPadded16 ? 1 : 0;
            case 45: return m.padded16 ? 1 : 0;
            default: return 0.0;
        }
    }

    // After ms_an_finish, as doubles. which: 0 timeline (short-term LUFS once
    // a second), 1 seconds over -1 dBTP, 2 seconds over 0 dBTP (the first 40
    // of each), 3 clipping times, 4 seconds where the low end is out of phase
    EMSCRIPTEN_KEEPALIVE const double* ms_an_list (void* p, int which) { return static_cast<Analyse*> (p)->lists[which].data(); }
    EMSCRIPTEN_KEEPALIVE int ms_an_list_size (void* p, int which) { return (int) static_cast<Analyse*> (p)->lists[which].size(); }

    // ---- spectrum analyser ----

    // window: 0 symmetric Hann, 1 periodic Hann. smoothing 0: none.
    EMSCRIPTEN_KEEPALIVE void* ms_sp_create (int size, int history, int window, float scale, float attack, float release,
                                             float powerSmoothing, float smoothing, int maxBlock)
    {
        auto* m = new Spectrum();
        m->make (maxBlock);
        vs::SpectrumAnalyser::Options o;
        o.size = size;
        o.history = history;
        o.window = window == 1 ? vs::SpectrumAnalyser::Window::hannPeriodic : vs::SpectrumAnalyser::Window::hannSymmetric;
        o.scale = scale;
        o.attack = attack;
        o.release = release;
        o.powerSmoothing = powerSmoothing;
        o.smoothing = smoothing;
        m->analyser.prepare (o);
        return m;
    }
    EMSCRIPTEN_KEEPALIVE void ms_sp_destroy (void* p) { delete static_cast<Spectrum*> (p); }
    EMSCRIPTEN_KEEPALIVE float* ms_sp_input (void* p) { return static_cast<Spectrum*> (p)->l.data(); }
    // Adds n samples of ms_sp_input to the history
    EMSCRIPTEN_KEEPALIVE void ms_sp_push (void* p, int n) { auto* m = static_cast<Spectrum*> (p); m->analyser.push (m->l.data(), n); }
    EMSCRIPTEN_KEEPALIVE void ms_sp_reset (void* p) { static_cast<Spectrum*> (p)->analyser.reset(); }
    // Magnitudes, levels and power average from the newest samples
    EMSCRIPTEN_KEEPALIVE void ms_sp_update (void* p) { static_cast<Spectrum*> (p)->analyser.update(); }
    // Magnitudes only
    EMSCRIPTEN_KEEPALIVE void ms_sp_analyse (void* p) { static_cast<Spectrum*> (p)->analyser.analyse(); }
    EMSCRIPTEN_KEEPALIVE int ms_sp_bins (void* p) { return static_cast<Spectrum*> (p)->analyser.bins(); }
    EMSCRIPTEN_KEEPALIVE const float* ms_sp_magnitude (void* p) { return static_cast<Spectrum*> (p)->analyser.magnitude().data(); }
    EMSCRIPTEN_KEEPALIVE const float* ms_sp_levels (void* p) { return static_cast<Spectrum*> (p)->analyser.levelsDb().data(); }
    EMSCRIPTEN_KEEPALIVE const float* ms_sp_power (void* p) { return static_cast<Spectrum*> (p)->analyser.power().data(); }
    EMSCRIPTEN_KEEPALIVE float ms_sp_level_at (void* p, double hz, double sampleRate, float tiltDbPerOctave, float tiltCentreHz)
    {
        return static_cast<Spectrum*> (p)->analyser.levelAt (hz, sampleRate, tiltDbPerOctave, tiltCentreHz);
    }

    // ---- waveform overviews ----

    // frames: the song's length (needed first). rms and bands: 0 or 1.
    EMSCRIPTEN_KEEPALIVE void* ms_wf_create (double sampleRate, double frames, int columns, int stride, int rms, int bands, int maxBlock)
    {
        auto* m = new Overview();
        m->make (maxBlock);
        vs::waveform::Overview::Options o;
        o.columns = columns;
        o.stride = stride;
        o.rms = rms != 0;
        o.bands = bands != 0;
        m->overview.prepare (sampleRate, (std::int64_t) frames, o);
        return m;
    }
    EMSCRIPTEN_KEEPALIVE void ms_wf_destroy (void* p) { delete static_cast<Overview*> (p); }
    EMSCRIPTEN_KEEPALIVE float* ms_wf_left (void* p)  { return static_cast<Overview*> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* ms_wf_right (void* p) { return static_cast<Overview*> (p)->r.data(); }
    // stereo 0 reads only ms_wf_left
    EMSCRIPTEN_KEEPALIVE void ms_wf_process (void* p, int n, int stereo)
    {
        auto* m = static_cast<Overview*> (p);
        m->overview.process (m->l.data(), stereo != 0 ? m->r.data() : nullptr, n);
    }
    // which: 0 peak, 1 left peak, 2 right peak, 3 RMS (one value per column),
    // 4 bands (lows, mids, highs per column). Valid until the next call.
    EMSCRIPTEN_KEEPALIVE const float* ms_wf_get (void* p, int which)
    {
        auto* m = static_cast<Overview*> (p);
        const auto& o = m->overview;
        switch (which)
        {
            case 0: m->out = o.peak(); break;
            case 1: m->out = o.peakL(); break;
            case 2: m->out = o.peakR(); break;
            case 3: m->out = o.rms(); break;
            default: m->out = o.bands(); break;
        }
        return m->out.data();
    }

    // Block peaks of one channel for a song of any length (ms_wf_blocks_*)
    EMSCRIPTEN_KEEPALIVE void* ms_wf_blocks_create (int blockSize, int maxBlock)
    {
        auto* m = new Blocks (blockSize);
        m->make (maxBlock);
        return m;
    }
    EMSCRIPTEN_KEEPALIVE void ms_wf_blocks_destroy (void* p) { delete static_cast<Blocks*> (p); }
    EMSCRIPTEN_KEEPALIVE float* ms_wf_blocks_input (void* p) { return static_cast<Blocks*> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE void ms_wf_blocks_process (void* p, int n) { auto* m = static_cast<Blocks*> (p); m->peaks.process (m->l.data(), n); }
    // The peaks so far, the part-filled last block included: ms_wf_blocks_count of them
    EMSCRIPTEN_KEEPALIVE int ms_wf_blocks_count (void* p) { auto* m = static_cast<Blocks*> (p); m->out = m->peaks.peaks(); return (int) m->out.size(); }
    EMSCRIPTEN_KEEPALIVE const double* ms_wf_blocks_peaks (void* p) { return static_cast<Blocks*> (p)->out.data(); }

    // A song's shape: `points` bytes 0..255 into out from n block peaks.
    // Returns points, or 0 when there are no peaks (out untouched).
    EMSCRIPTEN_KEEPALIVE int ms_wf_shape (const double* peaks, int n, int points, std::uint8_t* out)
    {
        const auto s = vs::waveform::shape (peaks, n, points);
        if (s.empty()) return 0;
        std::copy (s.begin(), s.end(), out);
        return (int) s.size();
    }

    // Peaks for drawing a short sound `width` pixels wide into out (malloc'd
    // buffers; r 0 for one channel).
    EMSCRIPTEN_KEEPALIVE void ms_wf_pixel_peaks (const float* l, const float* r, double n, int width, float* out)
    {
        const float* ch[2] { l, r };
        const auto s = vs::waveform::pixelPeaks (ch, r != nullptr ? 2 : 1, (std::int64_t) n, width);
        std::copy (s.begin(), s.end(), out);
    }
}
