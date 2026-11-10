// The reference set (data/reference.json): makes each case's input with
// dsp/ReferenceSignals.h, runs it through the audio processing natively and
// checks every expected result within its tolerance. Fails when a result is
// off, when an input is not the one the file names (its hash), when the file
// has a result this run did not produce, or when this run produced one the
// file does not have. wasm/reference.mjs checks the same file through
// master.wasm, all but the cases marked "native" (processors the browser
// build has no functions for), which only the native builds check.
//
//   ReferenceTests [data/reference.json]          check (the default path is this repository's)
//   ReferenceTests --compute cases.json out.json  write the results of the cases in cases.json
//                                                 (tools/reference.py, which writes data/reference.json)
//
// Built without JUCE as the browser build is; a product can build it with
// JUCE too (without VS_NO_JUCE), against its own copy of this repository.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>
#include "dsp/AbCompare.h"
#include "dsp/Analysis.h"
#include "dsp/Channels.h"
#include "dsp/Compressor.h"
#include "dsp/DeviceSim.h"
#include "dsp/Dither.h"
#include "dsp/Doubler.h"
#include "dsp/DrumSynth.h"
#include "dsp/Dynamics.h"
#include "dsp/EqBand.h"
#include "dsp/FracDelay.h"
#include "dsp/Gate.h"
#include "dsp/GuitarFixes.h"
#include "dsp/Loudness.h"
#include "dsp/LoudnessReport.h"
#include "dsp/MasterChain.h"
#include "dsp/Mastering.h"
#include "dsp/Metronome.h"
#include "dsp/Music.h"
#include "dsp/Onsets.h"
#include "dsp/PitchDetector.h"
#include "dsp/ReferenceSignals.h"
#include "dsp/Resampler.h"
#include "dsp/ReverbEngine.h"
#include "dsp/Rider.h"
#include "dsp/Saturator.h"
#include "dsp/ScaleLibrary.h"
#include "dsp/Space.h"
#include "dsp/Spectrum.h"
#include "dsp/Tempo.h"
#include "dsp/TruePeak.h"
#include "dsp/TunerPitch.h"
#include "dsp/VocalEq.h"
#include "dsp/Waveform.h"
#include "dsp/Yin.h"
#include "dsp/BodyMatch.h"
#include "dsp/DeEsser.h"
#include "dsp/PitchEngine.h"

namespace
{
    // ---- a small JSON reader

    struct Json
    {
        enum Kind { null, boolean, number, string, array, object } kind = null;
        double num = 0.0;
        bool b = false;
        std::string str;
        std::vector<Json> items;
        std::vector<std::string> keys;   // an object's member names, and their values in items

        const Json* find (const std::string& key) const
        {
            if (kind != object) return nullptr;
            for (size_t k = 0; k < keys.size(); ++k) if (keys[k] == key) return &items[k];
            return nullptr;
        }
        double get (const std::string& key, double fallback) const
        {
            const auto* v = find (key);
            return v != nullptr && v->kind == number ? v->num : fallback;
        }
        std::string text (const std::string& key) const
        {
            const auto* v = find (key);
            return v != nullptr && v->kind == string ? v->str : std::string();
        }
    };

    struct Parser
    {
        const std::string& s;
        size_t i = 0;
        bool failed = false;

        void space() { while (i < s.size() && std::strchr (" \t\r\n", s[i]) != nullptr && s[i] != 0) ++i; }
        bool eat (char c) { space(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }

        std::string readString()
        {
            std::string out;
            if (! eat ('"')) { failed = true; return out; }
            while (i < s.size() && s[i] != '"')
            {
                char c = s[i++];
                if (c == '\\' && i < s.size())
                {
                    const char e = s[i++];
                    if (e == 'n') c = '\n'; else if (e == 't') c = '\t'; else if (e == 'u') { i += 4; c = '?'; } else c = e;
                }
                out += c;
            }
            ++i;
            return out;
        }

        Json value()
        {
            Json v;
            space();
            if (i >= s.size()) { failed = true; return v; }
            const char c = s[i];
            if (c == '{')
            {
                ++i; v.kind = Json::object;
                if (eat ('}')) return v;
                do { v.keys.push_back (readString()); if (! eat (':')) failed = true; v.items.push_back (value()); } while (! failed && eat (','));
                if (! eat ('}')) failed = true;
            }
            else if (c == '[')
            {
                ++i; v.kind = Json::array;
                if (eat (']')) return v;
                do v.items.push_back (value()); while (! failed && eat (','));
                if (! eat (']')) failed = true;
            }
            else if (c == '"') { v.kind = Json::string; v.str = readString(); }
            else if (s.compare (i, 4, "true") == 0) { v.kind = Json::boolean; v.b = true; i += 4; }
            else if (s.compare (i, 5, "false") == 0) { v.kind = Json::boolean; i += 5; }
            else if (s.compare (i, 4, "null") == 0) { i += 4; }
            else
            {
                char* end = nullptr;
                v.kind = Json::number;
                v.num = std::strtod (s.c_str() + i, &end);
                if (end == s.c_str() + i) failed = true;
                i = (size_t) (end - s.c_str());
            }
            return v;
        }
    };

    bool readJson (const std::string& path, Json& out)
    {
        std::ifstream f (path, std::ios::binary);
        if (! f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        Parser p { text };
        out = p.value();
        return ! p.failed;
    }

    // ---- inputs

    // A part's fields in the order dsp/ReferenceSignals.h reads them
    const char* const partFields[] { "type", "from", "to", "l", "r", "hz", "mult", "phase", "vibHz", "vibDepth",
                                     "attack", "decay", "every", "seed", "lowpass", "highpass" };

    std::vector<double> encodeParts (const Json& parts)
    {
        std::vector<double> out;
        for (const auto& p : parts.items)
        {
            const std::string type = p.text ("type");
            const double t = type == "noise" ? 1 : type == "samples" ? 2 : type == "clip" ? 3 : 0;
            const auto* values = p.find ("values");
            for (int k = 0; k < 16; ++k)
            {
                const std::string name = partFields[k];
                double fallback = name == "to" ? -1.0 : name == "mult" || name == "seed" ? 1.0 : 0.0;
                out.push_back (k == 0 ? t : p.get (name, fallback));
            }
            out.push_back (values != nullptr ? (double) values->items.size() : 0.0);
            if (values != nullptr) for (const auto& v : values->items) out.push_back (v.num);
        }
        return out;
    }

    vs::reference::Signal makeInput (const Json& input)
    {
        const double fs = input.get ("rate", 48000);
        const auto frames = (std::int64_t) std::llround (input.get ("seconds", 1) * fs);
        const auto parts = encodeParts (*input.find ("parts"));
        return vs::reference::render (fs, frames, (int) input.get ("channels", 1), parts.data(), (int) parts.size());
    }

    std::string hex (std::uint32_t h)
    {
        char b[16];
        std::snprintf (b, sizeof (b), "%08x", h);
        return b;
    }

    // ---- results

    using Value = std::variant<double, std::string>;
    using Results = std::vector<std::pair<std::string, Value>>;

    double sumAbs (const std::vector<float>& x) { double s = 0.0; for (float v : x) s += std::abs ((double) v); return s; }
    double peakAbs (const std::vector<float>& x) { float p = 0.0f; for (float v : x) p = std::max (p, std::abs (v)); return p; }

    void fingerprint (Results& out, const std::string& name, const std::vector<float>& x)
    {
        out.push_back ({ name + ".length", (double) x.size() });
        if (x.empty()) return;
        out.push_back ({ name + ".sum", sumAbs (x) });
        out.push_back ({ name + ".peak", peakAbs (x) });
    }

    // A slice of the input: params from (seconds) and length (samples)
    std::vector<float> slice (const Json& c, const vs::reference::Signal& in, double fs)
    {
        const auto from = (size_t) std::llround (c.get ("from", 0) * fs);
        const auto n = (size_t) c.get ("length", (double) in.l.size());
        const size_t a = std::min (from, in.l.size()), b = std::min (in.l.size(), a + n);
        return std::vector<float> (in.l.begin() + (std::ptrdiff_t) a, in.l.begin() + (std::ptrdiff_t) b);
    }

    template <typename Feed>
    void blocks (std::int64_t n, int block, Feed&& feed)
    {
        for (std::int64_t s = 0; s < n; s += block) feed (s, (int) std::min<std::int64_t> (block, n - s));
    }

    // ---- helpers for the cases below

    std::vector<double> nums (const Json& c, const std::string& key)
    {
        std::vector<double> v;
        if (const auto* a = c.find (key)) for (const auto& x : a->items) v.push_back (x.num);
        return v;
    }

    using Channels = std::vector<std::vector<float>>;

    Channels copyOf (const vs::reference::Signal& in)
    {
        Channels ch { in.l };
        if (in.channels > 1) ch.push_back (in.r);
        return ch;
    }

    std::vector<float*> pointers (Channels& ch)
    {
        std::vector<float*> p;
        for (auto& c : ch) p.push_back (c.data());
        return p;
    }

    std::vector<const float*> constPointers (const Channels& ch)
    {
        std::vector<const float*> p;
        for (auto& c : ch) p.push_back (c.data());
        return p;
    }

    // Length, sum of |x| over every channel, peak and integrated loudness of a processed signal
    void fingerprintAudio (Results& out, const std::string& name, const Channels& ch, double fs)
    {
        const std::string p = name.empty() ? "" : name + ".";
        const size_t len = ch.empty() ? 0 : ch[0].size();
        out.push_back ({ p + "length", (double) len });
        if (len == 0) return;
        double sum = 0.0;
        float peak = 0.0f;
        for (const auto& c : ch) for (float v : c) { sum += std::abs ((double) v); peak = std::max (peak, std::abs (v)); }
        out.push_back ({ p + "sum", sum });
        out.push_back ({ p + "peak", (double) peak });
        vs::LoudnessMeter m;
        m.prepare (fs);
        for (size_t s = 0; s < len; s += 4096)
        {
            const int k = (int) std::min<size_t> (4096, len - s);
            m.process (ch[0].data() + s, ch.size() > 1 ? ch[1].data() + s : nullptr, k);
        }
        out.push_back ({ p + "lufs", (double) m.integrated() });
    }

    // Sum and largest of the finite values, where the largest is, and the values at the bins asked for
    template <typename T>
    void arrayStats (Results& out, const std::string& name, const T* x, int n, const std::vector<double>& picks)
    {
        double sum = 0.0, best = -1e300;
        int at = -1;
        for (int i = 0; i < n; ++i)
        {
            const double v = (double) x[i];
            if (! std::isfinite (v)) continue;
            sum += v;
            if (v > best) { best = v; at = i; }
        }
        out.push_back ({ name + ".count", (double) n });
        out.push_back ({ name + ".sum", sum });
        out.push_back ({ name + ".max", best });
        out.push_back ({ name + ".maxAt", (double) at });
        for (double b : picks)
            if ((int) b < n) out.push_back ({ name + ".at" + std::to_string ((int) b), (double) x[(int) b] });
    }

    template <typename Process>
    void inBlocks (Channels& ch, int block, Process&& process)
    {
        const int len = ch.empty() ? 0 : (int) ch[0].size();
        for (int s = 0; s < len; s += block)
        {
            const int k = std::min (block, len - s);
            float* p[2] { ch[0].data() + s, ch.size() > 1 ? ch[1].data() + s : nullptr };
            process (p, (int) ch.size(), k);
        }
    }

    const char* const analysisNames[] {
        "empty", "duration", "samplePeakDb", "samplePeakAt", "peakLDb", "peakRDb", "dcL", "dcR", "oversFloat", "corr",
        "monoLossDb", "sideToMidDb", "identical", "maxDiff", "silentL", "silentR", "silent", "lufs", "shortTermMax",
        "shortTermMaxAt", "lra", "balanceDb", "truePeakDb", "truePeakAt", "tpOverCount", "tpOverZeroCount", "clipRuns",
        "clipEvents", "clipLongest", "clipHasLevel", "clipLevelDb", "lowCorr", "lowMonoLossDb", "lowShareDb", "leadSec",
        "tailSec", "firstSampleDb", "lastSampleDb", "headRmsDb", "endRmsDb", "endDecayDb", "hasSpectrum", "cliffHz",
        "cliffDrop", "hasPadded16", "padded16" };

    double analysisValue (const vs::analysis::Measurements& m, int what)
    {
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

    // A spelled note as text
    std::string spelled (vs::music::Note n) { return vs::music::noteText (n); }

    bool runMore (const std::string& op, const Json& c, const vs::reference::Signal* in, double fs, int block, Results& out)
    {
        const std::int64_t n = in != nullptr ? (std::int64_t) in->l.size() : 0;
        const bool stereo = in != nullptr && in->channels > 1;

        // ---- measurement
        if (op == "file-analysis")
        {
            vs::analysis::Options o;
            o.bitDepth = (int) c.get ("bitDepth", 0);
            o.isFloat = c.get ("isFloat", 0) != 0;
            vs::FileAnalysis a;
            a.prepare (fs, in->channels, n, o);
            do blocks (n, block, [&] (std::int64_t s, int k) { a.process (in->l.data() + s, stereo ? in->r.data() + s : in->l.data() + s, k); });
            while (a.nextPass());
            const auto m = a.finish();
            for (int k = 0; k < 46; ++k) out.push_back ({ analysisNames[k], analysisValue (m, k) });
            static const char* const bandFields[] { "corr", "monoLossDb", "shareDb" };
            for (int b = 0; b < vs::analysis::numBands; ++b)
                for (int k = 0; k < 3; ++k)
                    out.push_back ({ "band" + std::to_string (b) + "." + bandFields[k], k == 0 ? m.band[b].corr : k == 1 ? m.band[b].monoLossDb : m.band[b].shareDb });
            const std::vector<double> lists[] { { m.timeline.begin(), m.timeline.end() }, { m.tpOvers.begin(), m.tpOvers.end() },
                                                { m.tpOverZero.begin(), m.tpOverZero.end() }, { m.clip.times.begin(), m.clip.times.end() },
                                                { m.low.outTimes.begin(), m.low.outTimes.end() } };
            static const char* const listNames[] { "timeline", "tpOvers", "tpOverZero", "clipTimes", "lowOut" };
            for (int l = 0; l < 5; ++l)
            {
                out.push_back ({ std::string (listNames[l]) + ".count", (double) lists[l].size() });
                for (size_t i = 0; i < lists[l].size(); ++i) out.push_back ({ std::string (listNames[l]) + "." + std::to_string (i), lists[l][i] });
            }
        }
        else if (op == "spectrum")
        {
            vs::SpectrumAnalyser::Options o;
            if (c.get ("webAudio", 0) != 0) o = vs::SpectrumAnalyser::webAudio ((int) c.get ("size", 2048), (float) c.get ("smoothing", 0.8));
            else
            {
                o.size = (int) c.get ("size", 4096);
                o.history = (int) c.get ("history", 8192);
                const int w = (int) c.get ("window", 0);
                o.window = w == 2 ? vs::SpectrumAnalyser::Window::blackman : w == 1 ? vs::SpectrumAnalyser::Window::hannPeriodic : vs::SpectrumAnalyser::Window::hannSymmetric;
                o.scale = (float) c.get ("scale", 1024);
                o.attack = (float) c.get ("attack", 0.5);
                o.release = (float) c.get ("release", 0.15);
                o.powerSmoothing = (float) c.get ("powerSmoothing", 0.04);
                o.smoothing = (float) c.get ("smoothing", 0);
            }
            vs::SpectrumAnalyser a;
            a.prepare (o);
            const bool update = c.text ("mode") == "update";
            blocks (n, block, [&] (std::int64_t s, int k) { a.push (in->l.data() + s, k); if (update) a.update(); else a.analyse(); });
            const auto picks = nums (c, "bins");
            arrayStats (out, "magnitude", a.magnitude().data(), a.bins(), picks);
            if (update)
            {
                arrayStats (out, "levels", a.levelsDb().data(), a.bins(), picks);
                arrayStats (out, "power", a.power().data(), a.bins(), picks);
                for (double hz : nums (c, "levelAt")) out.push_back ({ "levelAt" + std::to_string ((int) hz), (double) a.levelAt (hz, fs, 3.0f, 1000.0f) });
            }
            else arrayStats (out, "decibels", a.decibels().data(), a.bins(), picks);
        }
        else if (op == "welch")
        {
            const float* ch[2] { in->l.data(), stereo ? in->r.data() : nullptr };
            const int size = (int) c.get ("size", 4096);
            const auto starts = nums (c, "starts");
            vs::AveragedSpectrum s;
            if (starts.empty()) s = vs::welch (ch, in->channels, n, fs, size, (int) c.get ("maxSegments", 64));
            else
            {
                std::vector<std::int64_t> at;
                for (double v : starts) at.push_back ((std::int64_t) v);
                s = vs::welchAt (ch, in->channels, fs, size, at);
            }
            arrayStats (out, "power", s.power.data(), (int) s.power.size(), nums (c, "bins"));
        }
        else if (op == "tonal-balance")
        {
            const auto t = vs::tonalBalance (in->l.data(), n, fs);
            for (int b = 0; b < vs::TonalBalance::numBands; ++b)
            {
                out.push_back ({ "level" + std::to_string (b), t.levelDb[b] });
                out.push_back ({ "rel" + std::to_string (b), t.relDb[b] });
            }
            out.push_back ({ "lows", t.lows });
            out.push_back ({ "mud", t.mud });
            out.push_back ({ "harsh", t.harsh });
            out.push_back ({ "top", t.top });
        }
        else if (op == "waveform")
        {
            vs::waveform::Overview::Options o;
            o.columns = (int) c.get ("columns", 16);
            o.stride = (int) c.get ("stride", 1);
            o.rms = c.get ("rms", 0) != 0;
            o.bands = c.get ("bands", 0) != 0;
            vs::waveform::Overview w;
            w.prepare (fs, n, o);
            blocks (n, block, [&] (std::int64_t s, int k) { w.process (in->l.data() + s, stereo ? in->r.data() + s : nullptr, k); });
            const std::vector<double> picks { 0, (double) (o.columns / 2), (double) (o.columns - 1) };
            auto stat = [&] (const char* name, const std::vector<float>& v) { arrayStats (out, name, v.data(), (int) v.size(), picks); };
            stat ("peak", w.peak());
            stat ("peakL", w.peakL());
            stat ("peakR", w.peakR());
            if (o.rms) stat ("rms", w.rms());
            if (o.bands) stat ("bands", w.bands());
        }
        else if (op == "block-peaks")
        {
            vs::waveform::BlockPeaks b ((int) c.get ("blockSize", 100));
            blocks (n, block, [&] (std::int64_t s, int k) { b.process (in->l.data() + s, k); });
            const auto peaks = b.peaks();
            arrayStats (out, "peaks", peaks.data(), (int) peaks.size(), {});
            const auto shape = vs::waveform::shape (peaks.data(), (int) peaks.size(), (int) c.get ("points", 200));
            const int pts = (int) shape.size();
            arrayStats (out, "shape", shape.data(), pts, { 0, (double) (pts / 2), (double) std::max (0, pts - 1) });
            const float* ch[2] { in->l.data(), stereo ? in->r.data() : nullptr };
            const auto px = vs::waveform::pixelPeaks (ch, in->channels, n, (int) c.get ("width", 64));
            arrayStats (out, "pixels", px.data(), (int) px.size(), {});
        }
        else if (op == "clip-runs")
        {
            const float* ch[2] { in->l.data(), stereo ? in->r.data() : nullptr };
            const double tol = vs::FlatRuns::tolerance ((int) c.get ("bitDepth", 0), c.get ("isFloat", 0) != 0);
            std::vector<vs::FlatRuns::Event> all;
            int runs = 0, longest = 0;
            for (int ci = 0; ci < in->channels; ++ci)
            {
                float peak = 0.0f;
                for (std::int64_t i = 0; i < n; ++i) peak = std::max (peak, std::abs (ch[ci][i]));
                if (peak < 0.5f) continue;
                vs::FlatRuns fr;
                fr.prepare (peak, tol, fs);
                fr.process (ch[ci], (int) n);
                fr.finish();
                runs += fr.runs();
                longest = std::max (longest, fr.longest());
                all.insert (all.end(), fr.events().begin(), fr.events().end());
            }
            const auto spots = vs::FlatRuns::merge (all);
            out.push_back ({ "runs", (double) runs });
            out.push_back ({ "longest", (double) longest });
            out.push_back ({ "spots", (double) spots.size() });
            for (size_t k = 0; k < spots.size(); ++k) out.push_back ({ "spot" + std::to_string (k), spots[k].start });
        }

        // ---- processing
        else if (op == "resample")
        {
            const double to = c.get ("to", 48000);
            vs::Resampler r (fs / to, in->channels);
            Channels res ((size_t) in->channels);
            auto take = [&] (int got) { for (int ci = 0; ci < in->channels; ++ci) res[(size_t) ci].insert (res[(size_t) ci].end(), r.output (ci), r.output (ci) + got); };
            blocks (n, block, [&] (std::int64_t s, int k)
            {
                const float* ch[2] { in->l.data() + s, stereo ? in->r.data() + s : nullptr };
                take (r.push (ch, k));
            });
            take (r.flush());
            out.push_back ({ "resampledLength", (double) vs::resampledLength ((int) n, fs, to) });
            fingerprintAudio (out, "", res, to);
        }
        else if (op == "fracdelay")
        {
            auto ch = copyOf (*in);
            vs::FracDelay d (nums (c, "delays"));
            for (int ci = 0; ci < (int) ch.size(); ++ci)
                for (std::int64_t s = 0; s < n; s += block) d.process (ci, ch[(size_t) ci].data() + s, (int) std::min<std::int64_t> (block, n - s));
            out.push_back ({ "latency", (double) d.latency() });
            fingerprintAudio (out, "", ch, fs);
        }
        else if (op == "decimate")
        {
            const int factor = (int) c.get ("factor", 2);
            vs::Decimator d (fs, factor);
            std::vector<float> res, buf ((size_t) d.maxOutput (block));
            blocks (n, block, [&] (std::int64_t s, int k) { const int got = d.push (in->l.data() + s, k, buf.data()); res.insert (res.end(), buf.begin(), buf.begin() + got); });
            fingerprintAudio (out, "", { res }, fs / factor);
        }
        else if (op == "quantize")
        {
            const int bits = (int) c.get ("bits", 16);
            const int mode = (int) c.get ("mode", 0);
            vs::Quantizer q (bits, mode == 1 ? vs::Quantizer::Dither::always : mode == 2 ? vs::Quantizer::Dither::never : vs::Quantizer::Dither::automatic,
                             (std::uint32_t) c.get ("seed", 1));
            const auto ch = copyOf (*in);
            const auto src = constPointers (ch);
            std::vector<std::vector<std::int32_t>> ints ((size_t) in->channels, std::vector<std::int32_t> ((size_t) n));
            for (std::int64_t s = 0; s < n; s += block)
            {
                const int k = (int) std::min<std::int64_t> (block, n - s);
                const float* inp[2] { src[0] + s, in->channels > 1 ? src[1] + s : nullptr };
                std::int32_t* o[2] { ints[0].data() + s, in->channels > 1 ? ints[1].data() + s : nullptr };
                q.process (inp, in->channels, k, o);
            }
            double sum = 0.0, peak = 0.0;
            std::uint32_t h = 2166136261u;
            for (const auto& ci : ints)
                for (auto v : ci)
                {
                    sum += std::abs ((double) v);
                    peak = std::max (peak, std::abs ((double) v));
                    for (int k = 0; k < 4; ++k) { h ^= ((std::uint32_t) v >> (8 * k)) & 0xffu; h *= 16777619u; }
                }
            out.push_back ({ "dithering", q.isDithering() ? 1.0 : 0.0 });
            out.push_back ({ "onGrid", vs::Quantizer::onGrid (src.data(), in->channels, (int) n, bits) ? 1.0 : 0.0 });
            out.push_back ({ "sum", sum });
            out.push_back ({ "peak", peak });
            out.push_back ({ "hash", hex (h) });
        }
        else if (op == "fades")
        {
            using namespace vs::channels;
            auto ch = copyOf (*in);
            auto p = pointers (ch);
            const auto curve = (FadeCurve) (int) c.get ("curve", 0);
            const auto kind = c.text ("kind");
            if (kind == "seconds") fades (p.data(), (int) p.size(), (int) n, fs, c.get ("in", 0), c.get ("out", 0), curve);
            else if (kind == "samples") fadeSamples (p.data(), (int) p.size(), (int) n, (int) c.get ("in", 0), (int) c.get ("out", 0), curve, (FadePoint) (int) c.get ("point", 0));
            else
            {
                const int len = (int) c.get ("length", 1000);
                Channels into, from;
                for (auto& x : ch) { into.emplace_back (x.begin(), x.begin() + len); from.emplace_back (x.begin() + len, x.begin() + 2 * len); }
                auto pi = pointers (into);
                auto pf = constPointers (from);
                crossfade (pi.data(), pf.data(), (int) into.size(), len, curve, (FadePoint) (int) c.get ("point", 0));
                ch = into;
            }
            fingerprintAudio (out, "", ch, fs);
        }
        else if (op == "channels")
        {
            using namespace vs::channels;
            const auto ch = copyOf (*in);
            const auto src = constPointers (ch);
            static const char* const maps[] { "keep", "left", "right", "mono", "stereo" };
            for (int m = 0; m < 5; ++m)
            {
                const int nOut = mappedChannels ((Map) m, in->channels);
                Channels o ((size_t) nOut, std::vector<float> ((size_t) n));
                auto po = pointers (o);
                map ((Map) m, src.data(), in->channels, (int) n, po.data());
                out.push_back ({ std::string (maps[m]) + ".channels", (double) nOut });
                fingerprintAudio (out, maps[m], o, fs);
            }
            out.push_back ({ "peakOf", (double) peakOf (src.data(), in->channels, (int) n) });
            const auto t = nums (c, "trim");
            for (size_t k = 0; k + 1 < t.size(); k += 2)
            {
                const auto r = trimRange ((int) n, fs, t[k], t[k + 1]);
                out.push_back ({ "trim" + std::to_string (k / 2) + ".first", (double) r.first });
                out.push_back ({ "trim" + std::to_string (k / 2) + ".last", (double) r.last });
            }
            for (int mode = 1; mode <= 2; ++mode)
            {
                auto x = copyOf (*in);
                auto px = pointers (x);
                const auto r = normalize (px.data(), in->channels, (int) n, fs, (Normalize) mode);
                const std::string p = mode == 1 ? "normalizePeak" : "normalizeLufs";
                out.push_back ({ p + ".gainDb", r.gainDb });
                out.push_back ({ p + ".outcome", (double) r.outcome });
                out.push_back ({ p + ".measuredLufs", (double) r.lufs });
                out.push_back ({ p + ".reachedLufs", r.reachedLufs });
                fingerprintAudio (out, p, x, fs);
            }
        }
        else if (op == "devices")
        {
            const auto freqs = nums (c, "freqs");
            for (int d = 0; d < (int) vs::device::devices().size(); ++d)
            {
                vs::DeviceSim sim;
                sim.prepare (fs);
                sim.setDevice (d);
                auto ch = copyOf (*in);
                inBlocks (ch, block, [&] (float** p, int, int k) { sim.process (p[0], p[1], k); });
                const std::string id = vs::device::devices()[(size_t) d].id;
                fingerprintAudio (out, id, ch, fs);
                for (double f : freqs) out.push_back ({ id + ".magnitude" + std::to_string ((int) f), sim.magnitudeDb (f) });
            }
        }
        else if (op == "vocal-eq")
        {
            vs::VocalEq eq;
            eq.prepare (fs);
            vs::VocalEq::Settings s;
            s.lowCutHz = (float) c.get ("lowCutHz", 20); s.lowDb = (float) c.get ("lowDb", 0); s.bodyDb = (float) c.get ("bodyDb", 0);
            s.presenceDb = (float) c.get ("presenceDb", 0); s.airDb = (float) c.get ("airDb", 0);
            eq.setSettings (s);
            auto ch = copyOf (*in);
            inBlocks (ch, block, [&] (float** p, int nc, int k) { eq.process (p, nc, k); });
            fingerprintAudio (out, "", ch, fs);
            for (double f : nums (c, "freqs")) out.push_back ({ "magnitude" + std::to_string ((int) f), eq.magnitudeDb (f) });
        }
        else if (op == "ab")
        {
            vs::AbCompare ab;
            ab.prepare (fs, block, (int) c.get ("latency", 0));
            ab.setHearOriginal (true);
            Channels res (2);
            std::vector<float> pl ((size_t) block), pr ((size_t) block);
            const std::int64_t half = n / 2;
            blocks (n, block, [&] (std::int64_t s, int k)
            {
                if (s >= half) ab.setHearOriginal (false);
                const float* r = stereo ? in->r.data() + s : in->l.data() + s;
                ab.takeOriginal (in->l.data() + s, r, k);
                for (int i = 0; i < k; ++i) { pl[(size_t) i] = in->l[(size_t) (s + i)] * 0.5f; pr[(size_t) i] = r[i] * 0.5f; }
                ab.mix (pl.data(), pr.data(), k);
                res[0].insert (res[0].end(), pl.begin(), pl.begin() + k);
                res[1].insert (res[1].end(), pr.begin(), pr.begin() + k);
            });
            out.push_back ({ "matchDb", (double) ab.matchGainDb() });
            fingerprintAudio (out, "", res, fs);
        }

        // ---- music
        else if (op == "music")
        {
            using namespace vs::music;
            for (int m = 0; m < 128; ++m) out.push_back ({ "name" + std::to_string (m), noteName (m, true) });
            out.push_back ({ "name-1", noteName (-1, true) });
            for (int letter = 0; letter < 7; ++letter)
                for (int acc = -1; acc <= 1; ++acc)
                    for (int minor = 0; minor < 2; ++minor)
                    {
                        const Key k { note (letter, acc), minor ? Mode::minor : Mode::major };
                        const auto id = noteText (note (letter, acc)) + (minor ? "m" : "");
                        out.push_back ({ id + ".name", keyName (k) });
                        out.push_back ({ id + ".signature", (double) signature (k) });
                        std::string scale;
                        for (auto nn : scaleNotes (k)) scale += (scale.empty() ? "" : " ") + spelled (nn);
                        out.push_back ({ id + ".scale", scale });
                        out.push_back ({ id + ".midi61", noteName (61, k, true) });
                    }
            for (int pc = 0; pc < 12; ++pc)
            {
                const auto p = "pc" + std::to_string (pc);
                out.push_back ({ p + ".plain", spelled (plainNote (pc)) });
                std::string all;
                for (auto s : spellings (pc)) all += (all.empty() ? "" : " ") + spelled (s);
                out.push_back ({ p + ".spellings", all });
                out.push_back ({ p + ".sharp", spelled (spellNote (pc, nullptr, 1)) });
                out.push_back ({ p + ".flat", spelled (spellNote (pc, nullptr, -1)) });
                out.push_back ({ p + ".inF#", spelled (inKey (pc, note (3, 1))) });
                out.push_back ({ p + ".inDb", spelled (inKey (pc, note (1, -1))) });
            }
            for (const auto& t : c.find ("parse")->items)
            {
                const auto nn = parseNote (t.str);
                out.push_back ({ "note:" + t.str, nn ? spelled (*nn) : std::string ("-") });
                const auto k = parseKey (t.str);
                out.push_back ({ "key:" + t.str, k ? keyName (*k) : std::string ("-") });
            }
            for (double hz : nums (c, "hz")) out.push_back ({ "hzToMidi" + std::to_string ((int) hz), hzToMidi (hz, 440.0) });
            for (double m : nums (c, "midi")) out.push_back ({ "midiToHz" + std::to_string ((int) m), midiToHz (m, 442.0) });
            for (int d = 27; d <= 87; ++d)
            {
                const char* g = gmDrumName (d);
                out.push_back ({ "drum" + std::to_string (d), std::string (g != nullptr ? g : "") });
                out.push_back ({ "pad" + std::to_string (d), (double) padForNote (d) });
            }
        }
        else if (op == "scale-library")
        {
            for (const auto& s : vs::scales::library)
            {
                std::string semis;
                for (int v : s.semis) semis += (semis.empty() ? "" : " ") + std::to_string (v);
                out.push_back ({ std::string (s.id) + ".name", std::string (s.name) });
                out.push_back ({ std::string (s.id) + ".group", std::string (s.group) });
                out.push_back ({ std::string (s.id) + ".semis", semis });
            }
        }
        else return false;
        return true;
    }

    // ---- processors the browser build has no functions for (the native and JUCE builds only)

    vs::MasterChain::Learned lockedLearned()
    {
        vs::MasterChain::Learned l;
        l.valid = true;
        l.preGainDb = -3.0f;
        l.driveDb = 6.0f;
        const float tone[5] { 1.0f, -0.5f, 0.0f, 0.5f, -1.0f };
        for (int b = 0; b < 5; ++b) l.toneMatch[b] = tone[b];
        return l;
    }

    bool runProcessor (const std::string& op, const Json& c, const vs::reference::Signal* in, double fs, int block, Results& out)
    {
        if (op != "processor" && op != "follow") return false;
        const std::string kind = c.text ("kind");
        auto ch = in != nullptr ? copyOf (*in) : Channels {};
        if (ch.size() == 1) ch.push_back (ch[0]); // processors run in stereo
        const int nCh = (int) ch.size();
        const float amount = (float) c.get ("amount", 0.5);

        if (op == "follow")
        {
            vs::PitchDetector d;
            d.prepare (fs);
            const int w = d.windowSize();
            const int hop = (int) c.get ("hop", 4800);
            int k = 0;
            for (std::int64_t s = 0; s + w <= (std::int64_t) in->l.size(); s += hop, ++k)
            {
                const auto r = d.analyse (in->l.data() + s);
                out.push_back ({ "f0." + std::to_string (k), (double) r.f0 });
                out.push_back ({ "confidence." + std::to_string (k), (double) r.confidence });
            }
            return true;
        }
        if (kind == "compressor")
        {
            vs::Compressor x;
            x.prepare (fs);
            x.setParams (amount, (int) c.get ("style", 0));
            inBlocks (ch, block, [&] (float** p, int nc, int k) { x.process (p, nc, k); });
        }
        else if (kind == "gate")
        {
            vs::Gate x;
            x.prepare (fs);
            x.setThreshold ((float) c.get ("threshold", -40));
            inBlocks (ch, block, [&] (float** p, int nc, int k) { x.process (p, nc, k); });
        }
        else if (kind == "rider")
        {
            vs::Rider x;
            x.prepare (fs);
            x.setParams ((float) c.get ("target", -18), (float) c.get ("range", 6), (float) c.get ("speed", 0.5));
            inBlocks (ch, block, [&] (float** p, int nc, int k) { x.process (p, nc, k); });
        }
        else if (kind == "dynamic-bell")
        {
            vs::DynamicBell x;
            x.prepare (fs, (float) c.get ("freq", 3000), c.get ("q", 2));
            const float cut = (float) c.get ("cut", 6);
            int done = 0;
            inBlocks (ch, block, [&] (float** p, int nc, int k) { x.process (p, nc, 0, k, cut * (float) ((done++ % 4) + 1) / 4.0f); });
        }
        else if (kind == "eq-band")
        {
            vs::EqBand x;
            x.prepare (fs);
            vs::EqBand::Settings s;
            s.type = (int) c.get ("type", 0);
            s.freq = (float) c.get ("freq", 1000);
            s.gainDb = (float) c.get ("gain", 0);
            s.q = (float) c.get ("q", 0.7);
            s.slope = (int) c.get ("slope", 1);
            x.setTarget (s);
            inBlocks (ch, block, [&] (float** p, int nc, int k) { x.process (p, nc, 0, k); });
        }
        else if (kind == "dynamic-band")
        {
            vs::DynamicBand x;
            x.prepare (fs);
            vs::DynamicBand::Settings s;
            s.on = true;
            s.freq = (float) c.get ("freq", 3000);
            s.rangeDb = (float) c.get ("range", 6);
            s.thresholdDb = (float) c.get ("threshold", -24);
            x.setSettings (s);
            inBlocks (ch, block, [&] (float** p, int nc, int k) { x.process (p, nc, 0, k); });
        }
        else if (kind == "carve")
        {
            vs::Carve x;
            x.prepare (fs);
            vs::Carve::Settings s;
            s.on = true;
            s.depthDb = (float) c.get ("depth", 6);
            x.setSettings (s);
            const auto side = copyOf (*in); // the side-chain: the input itself, before carving
            int at = 0;
            inBlocks (ch, block, [&] (float** p, int nc, int k)
            {
                const float* sc[1] { side[0].data() + at };
                x.process (p, nc, sc, 1, 0, k);
                at += k;
            });
        }
        else if (kind == "saturator")
        {
            vs::Saturator x;
            x.prepare (fs, block);
            x.setDrive (amount);
            inBlocks (ch, block, [&] (float** p, int nc, int k) { juce::dsp::AudioBlock<float> b (p, (size_t) nc, (size_t) k); x.process (b); });
            out.push_back ({ "latency", (double) x.getLatencySamples() });
        }
        else if (kind == "doubler")
        {
            vs::Doubler x;
            x.prepare (fs);
            inBlocks (ch, block, [&] (float** p, int, int k) { x.process (p[0], p[1], k, amount); });
        }
        else if (kind == "reverb")
        {
            vs::ReverbEngine x;
            x.prepare (fs, block);
            vs::ReverbEngine::Settings s = c.find ("space") != nullptr ? vs::ReverbEngine::space ((float) c.get ("size", 0.5)) : vs::ReverbEngine::Settings {};
            if (c.find ("mode") != nullptr) s.mode = (int) c.get ("mode", 0);
            x.setSettings (s);
            Channels wet (2, std::vector<float> (ch[0].size()));
            for (size_t s0 = 0; s0 < ch[0].size(); s0 += (size_t) block)
            {
                const int k = (int) std::min<size_t> ((size_t) block, ch[0].size() - s0);
                x.process (ch[0].data() + s0, ch[1].data() + s0, wet[0].data() + s0, wet[1].data() + s0, k);
            }
            ch = wet;
        }
        else if (kind == "space")
        {
            vs::Space x;
            x.prepare ({ fs, (std::uint32_t) block, 2 });
            x.setParams (amount, (float) c.get ("size", 0.5), (float) c.get ("delay", 0.3), (float) c.get ("delaySeconds", 0.25), (float) c.get ("feedback", 0.3));
            inBlocks (ch, block, [&] (float** p, int, int k) { x.process (p, k); });
        }
        else if (kind == "squeak" || kind == "pick" || kind == "boom")
        {
            auto run = [&] (auto& x) { x.prepare (fs); inBlocks (ch, block, [&] (float** p, int nc, int k) { x.process (p, nc, k, amount, true); }); };
            if (kind == "squeak") { vs::SqueakTamer x; run (x); }
            else if (kind == "pick") { vs::PickTamer x; run (x); }
            else { vs::BoomTamer x; run (x); }
        }
        else if (kind == "master-chain")
        {
            auto chain = std::make_unique<vs::MasterChain>();
            chain->prepare (fs, block);
            vs::MasterChain::Settings s;
            s.style = (int) c.get ("style", 0);
            chain->setSettings (s);
            chain->lockLearned (lockedLearned());
            out.push_back ({ "latency", (double) chain->getLatency() });
            inBlocks (ch, block, [&] (float** p, int, int k) { chain->process (p[0], p[1], k); });
        }
        else if (kind == "de-esser")
        {
            vs::DeEsser x;
            x.prepare ({ fs, (std::uint32_t) block, 2 });
            x.setParams (amount, (float) c.get ("freq", 6500));
            inBlocks (ch, block, [&] (float** p, int nc, int k) { x.process (p, nc, k); });
        }
        else if (kind == "pitch-engine")
        {
            vs::PitchEngine x;
            x.prepare (fs);
            vs::PitchEngine::Settings s;
            s.tuneOn = true;
            s.key = (int) c.get ("key", 0);
            s.scale = (int) c.get ("scale", 0);
            s.speedMs = (float) c.get ("speedMs", 20);
            s.harmonyOn = c.get ("harmony", 0) != 0;
            s.voice[0].interval = (int) c.get ("interval", 0);
            x.setSettings (s);
            Channels lead (1, std::vector<float> (ch[0].size())), harm (2, std::vector<float> (ch[0].size()));
            for (size_t s0 = 0; s0 < ch[0].size(); s0 += (size_t) block)
            {
                const int k = (int) std::min<size_t> ((size_t) block, ch[0].size() - s0);
                x.process (ch[0].data() + s0, lead[0].data() + s0, harm[0].data() + s0, harm[1].data() + s0, k);
            }
            fingerprintAudio (out, "harmony", harm, fs);
            ch = lead;
        }
        else if (kind == "body")
        {
            // learning: the DI is the input's left channel, the mic the same through a bell and a shelf
            vs::BodyLearner learner;
            learner.prepare (fs);
            learner.requestStart();
            Channels mic { in->l };
            vs::EqBand a, b;
            a.prepare (fs); b.prepare (fs);
            vs::EqBand::Settings sa, sb;
            sa.type = vs::EqBand::bell; sa.freq = 220.0f; sa.gainDb = 5.0f; sa.q = 1.2f;
            sb.type = vs::EqBand::highShelf; sb.freq = 4000.0f; sb.gainDb = -4.0f;
            a.setTarget (sa); b.setTarget (sb);
            inBlocks (mic, block, [&] (float** p, int, int k) { a.process (p, 1, 0, k); b.process (p, 1, 0, k); });
            for (size_t s0 = 0; s0 < in->l.size(); s0 += (size_t) block)
                learner.process (in->l.data() + s0, mic[0].data() + s0, (int) std::min<size_t> ((size_t) block, in->l.size() - s0));
            const auto curve = learner.computeCurve();
            std::vector<float> db (curve.db.begin(), curve.db.end());
            arrayStats (out, "curve", db.data(), (int) db.size(), { 0, 40, 60, 80, 100, 120 });
            out.push_back ({ "curve.levelDb", (double) curve.levelDb });
            fingerprintAudio (out, "ir", { vs::BodyFilter::design (curve, 1.0f, fs) }, fs);
            return true;
        }
        else
        {
            out.push_back ({ "unknown processor " + kind, std::string() });
            return true;
        }
        fingerprintAudio (out, "", ch, fs);
        return true;
    }

    Results run (const Json& c, const vs::reference::Signal* in)
    {
        Results out;
        const std::string op = c.text ("op");
        const auto* input = c.find ("input");
        const double fs = input != nullptr ? input->get ("rate", 48000) : c.get ("rate", 48000);
        const int block = (int) c.get ("block", 4096);
        const std::int64_t n = in != nullptr ? (std::int64_t) in->l.size() : 0;
        const bool stereo = in != nullptr && in->channels > 1;
        auto right = [&] (std::int64_t s) { return stereo ? in->r.data() + s : nullptr; };

        if (op == "loudness-meter")
        {
            vs::LoudnessMeter m;
            m.prepare (fs);
            blocks (n, block, [&] (std::int64_t s, int k) { m.process (in->l.data() + s, right (s), k); });
            out.push_back ({ "integrated", (double) m.integrated() });
        }
        else if (op == "loudness-report")
        {
            vs::LoudnessReporter m;
            m.prepare (fs, in->channels, block);
            blocks (n, block, [&] (std::int64_t s, int k) { m.process (in->l.data() + s, right (s), k); });
            const auto r = m.finish();
            out.push_back ({ "integrated", r.integrated });
            out.push_back ({ "shortTermMax", r.shortTermMax });
            out.push_back ({ "momentaryMax", r.momentaryMax });
            out.push_back ({ "range", r.range });
            out.push_back ({ "samplePeak", r.samplePeakDb });
            out.push_back ({ "truePeak", r.truePeakDb });
            out.push_back ({ "rmsLeft", r.rmsDb[0] });
            if (stereo) { out.push_back ({ "rmsRight", r.rmsDb[1] }); out.push_back ({ "balance", r.balanceDb }); }
            out.push_back ({ "frames", (double) r.frames });
        }
        else if (op == "true-peak")
        {
            vs::TruePeakMeter m;
            m.prepare (in->channels, block);
            blocks (n, block, [&] (std::int64_t s, int k)
            {
                const float* ch[2] { in->l.data() + s, stereo ? in->r.data() + s : nullptr };
                m.process (ch, k);
            });
            m.finish();
            out.push_back ({ "truePeak", (double) m.truePeak (-1) });
            out.push_back ({ "samplePeak", (double) m.samplePeak (-1) });
            // where the loudest peak is: asked for only where one peak stands clear of the rest
            if (c.find ("position") != nullptr) out.push_back ({ "position", (double) m.truePeakPosition() });
            if (stereo) for (int ch = 0; ch < 2; ++ch) out.push_back ({ ch == 0 ? "truePeakLeft" : "truePeakRight", (double) m.truePeak (ch) });
        }
        else if (op == "tuner")
        {
            const auto x = slice (c, *in, fs);
            const float hz = vs::TunerPitch::estimate (x.data(), (int) x.size(), fs, (float) c.get ("minHz", 25), (float) c.get ("maxHz", 2100));
            out.push_back ({ "hz", (double) hz });
            const auto r = vs::TunerReading::of (hz, 440.0, {}, {}, 0);
            out.push_back ({ "note", (double) r.note });
            if (hz > 0) out.push_back ({ "cents", (double) r.cents });
        }
        else if (op == "voice")
        {
            const auto x = slice (c, *in, fs);
            vs::Yin yin (vs::Yin::voice (c.get ("minHz", 65), c.get ("maxHz", 1100)));
            const auto& r = yin.analyse (x.data(), (int) x.size(), fs);
            out.push_back ({ "hz", r.hz });
            out.push_back ({ "clarity", r.clarity });
        }
        else if (op == "tempo")
        {
            vs::tempo::TempoFinder f (fs);
            blocks (n, block, [&] (std::int64_t s, int k) { f.push (in->l.data() + s, k); });
            const auto e = f.estimate (c.get ("minBpm", 60), c.get ("maxBpm", 200));
            out.push_back ({ "valid", e.valid ? 1.0 : 0.0 });
            out.push_back ({ "bpm", e.bpm });
            out.push_back ({ "others", (double) e.others.size() });
            for (size_t i = 0; i < e.others.size(); ++i) out.push_back ({ "other" + std::to_string (i), e.others[i] });
        }
        else if (op == "onsets")
        {
            vs::OnsetDetector d (fs, c.get ("sensitivity", 50), c.get ("refractoryMs", 45));
            std::vector<double> found;
            blocks (n, block, [&] (std::int64_t s, int k) { d.push (in->l.data() + s, k, s, found); });
            out.push_back ({ "count", (double) found.size() });
            for (size_t i = 0; i < found.size(); ++i) out.push_back ({ "onset" + std::to_string (i), found[i] });
        }
        else if (op == "mastering")
        {
            vs::MasterChain::Settings st;
            st.style = (int) c.get ("style", 0);
            st.intensity = (float) c.get ("intensity", 0.5);
            st.targetLufs = (float) c.get ("target", -14);
            st.ceilingDb = (float) c.get ("ceiling", -1);
            vs::Mastering m;
            m.begin (fs, st, n);
            blocks (n, block, [&] (std::int64_t s, int k) { m.feedEnergy (in->l.data() + s, right (s), k); });
            const auto w = m.learningWindow();
            blocks (w.length, block, [&] (std::int64_t s, int k) { m.feedWindow (in->l.data() + w.from + s, right (w.from + s), k); });
            m.learn();
            std::vector<float> ol ((size_t) n + 1), orr ((size_t) n + 1);
            std::vector<float> bl ((size_t) (block + 4 * vs::Mastering::block + 16384)), br (bl.size());
            do
            {
                m.beginRender();
                std::int64_t o = 0;
                auto keep = [&] (int k) { std::copy (bl.begin(), bl.begin() + k, ol.begin() + o); std::copy (br.begin(), br.begin() + k, orr.begin() + o); o += k; };
                blocks (n, block, [&] (std::int64_t s, int k) { keep (m.render (in->l.data() + s, right (s), k, bl.data(), br.data())); });
                keep (m.finishRender (bl.data(), br.data()));
            }
            while (m.nextPass());
            out.push_back ({ "lufs", (double) m.renderedLufs() });
            out.push_back ({ "preGain", (double) m.learned().preGainDb });
            out.push_back ({ "drive", (double) m.learned().driveDb });
            vs::TruePeakMeter tp;
            tp.prepare (2, block);
            blocks (n, block, [&] (std::int64_t s, int k) { const float* ch[2] { ol.data() + s, orr.data() + s }; tp.process (ch, k); });
            tp.finish();
            out.push_back ({ "truePeak", (double) tp.truePeak (-1) });
        }
        else if (op == "click-sounds")
        {
            static const char* const roles[] { "accent", "beat", "sub", "poly", "alt" };
            for (const auto& set : vs::metronome::soundSets)
            {
                const auto s = vs::metronome::makeSounds (set.id, fs);
                const std::vector<float>* r[] { &s.accent, &s.beat, &s.sub, &s.poly, &s.alt };
                for (int k = 0; k < 5; ++k) fingerprint (out, std::string (set.id) + "." + roles[k], *r[k]);
            }
        }
        else if (op == "drum-sounds")
        {
            for (const auto& d : vs::drumsynth::sounds()) fingerprint (out, d.id, vs::drumsynth::renderSound (d.id, fs));
        }
        else if (op == "drum-kits")
        {
            for (const auto& k : vs::drumsynth::kits())
                for (size_t p = 0; p < k.pads.size(); ++p)
                {
                    const auto name = std::string (k.id) + "." + std::to_string (p);
                    out.push_back ({ name + ".sound", std::string (k.pads[p].sound) });
                    out.push_back ({ name + ".slot", (double) k.pads[p].slot });
                    out.push_back ({ name + ".note", (double) k.pads[p].note });
                    out.push_back ({ name + ".choke", (double) k.pads[p].choke });
                }
        }
        else if (! runMore (op, c, in, fs, block, out) && ! runProcessor (op, c, in, fs, block, out))
            out.push_back ({ "unknown op " + op, std::string() });
        return out;
    }

    // ---- writing results (tools/reference.py)

    std::string quote (const std::string& s)
    {
        std::string o = "\"";
        for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; o += ch; }
        return o + "\"";
    }

    int compute (const std::string& casesPath, const std::string& outPath)
    {
        Json cases;
        if (! readJson (casesPath, cases)) { std::fprintf (stderr, "can't read %s\n", casesPath.c_str()); return 1; }
        std::ofstream o (outPath, std::ios::binary);
        o << "[\n";
        bool first = true;
        for (const auto& c : cases.find ("cases")->items)
        {
            std::unique_ptr<vs::reference::Signal> in;
            if (const auto* input = c.find ("input")) in = std::make_unique<vs::reference::Signal> (makeInput (*input));
            const auto results = run (c, in.get());
            o << (first ? "" : ",\n") << "{\"id\": " << quote (c.text ("id"));
            first = false;
            if (in) o << ", \"inputHash\": \"" << hex (vs::reference::hash (in->l.data(), in->channels > 1 ? in->r.data() : nullptr, (std::int64_t) in->l.size())) << "\"";
            o << ", \"results\": [";
            for (size_t i = 0; i < results.size(); ++i)
            {
                o << (i > 0 ? ", " : "") << "[" << quote (results[i].first) << ", ";
                if (const auto* d = std::get_if<double> (&results[i].second))
                {
                    char b[40];
                    if (std::isfinite (*d)) std::snprintf (b, sizeof (b), "%.17g", *d); else std::snprintf (b, sizeof (b), "null");
                    o << b;
                }
                else o << quote (std::get<std::string> (results[i].second));
                o << "]";
            }
            o << "]}";
        }
        o << "\n]\n";
        return 0;
    }

    // ---- checking

    int failures = 0, checks = 0;

    void report (bool ok, const std::string& name, const std::string& detail)
    {
        ++checks;
        if (! ok) ++failures;
        if (! ok) std::printf ("FAIL %-62s %s\n", name.c_str(), detail.c_str());
    }

    int check (const std::string& path)
    {
        Json file;
        if (! readJson (path, file)) { std::printf ("FAIL can't read %s\n", path.c_str()); return 1; }
        const auto* tolerances = file.find ("tolerances");
        const auto* cases = file.find ("cases");
        if (tolerances == nullptr || cases == nullptr || cases->items.empty()) { std::printf ("FAIL %s has no cases\n", path.c_str()); return 1; }
        std::map<std::string, std::pair<double, double>> worst; // tolerance -> largest difference, its allowance
        std::map<std::string, std::string> worstAt;
        for (const auto& c : cases->items)
        {
            const std::string id = c.text ("id");
            const int before = failures, checksBefore = checks;
            std::unique_ptr<vs::reference::Signal> in;
            if (const auto* input = c.find ("input"))
            {
                in = std::make_unique<vs::reference::Signal> (makeInput (*input));
                const auto h = hex (vs::reference::hash (in->l.data(), in->channels > 1 ? in->r.data() : nullptr, (std::int64_t) in->l.size()));
                report (h == c.text ("inputHash"), id + " input", "made " + h + ", the file names " + c.text ("inputHash"));
            }
            const auto got = run (c, in.get());
            const auto* expected = c.find ("results");
            std::vector<bool> used (got.size(), false);
            for (size_t m = 0; m < expected->keys.size(); ++m)
            {
                const auto& name = expected->keys[m];
                const auto& e = expected->items[m];
                const auto& value = e.items.at (0);
                const std::string tol = e.items.at (1).str;
                const auto* t = tolerances->find (tol);
                size_t k = 0;
                while (k < got.size() && got[k].first != name) ++k;
                if (k == got.size()) { report (false, id + " " + name, "not produced by this build"); continue; }
                used[k] = true;
                if (t == nullptr) { report (false, id + " " + name, "unknown tolerance " + tol); continue; }
                if (value.kind == Json::string)
                {
                    const auto* s = std::get_if<std::string> (&got[k].second);
                    report (s != nullptr && *s == value.str, id + " " + name, "expected " + value.str + ", got " + (s ? *s : std::string ("a number")));
                    continue;
                }
                const auto* d = std::get_if<double> (&got[k].second);
                if (d == nullptr) { report (false, id + " " + name, "got text, expected a number"); continue; }
                const double allow = t->get ("abs", 0) + t->get ("rel", 0) * std::abs (value.num);
                const double diff = *d == value.num ? 0.0 : std::abs (*d - value.num);
                auto& w = worst[tol];
                if (diff > w.first) { w = { diff, allow }; worstAt[tol] = id + " " + name; }
                char b[160];
                std::snprintf (b, sizeof (b), "expected %.17g, got %.17g (off by %.3g, %s allows %.3g)", value.num, *d, diff, tol.c_str(), allow);
                report (allow > 0 ? diff <= allow : diff == 0.0, id + " " + name, b);
            }
            for (size_t k = 0; k < got.size(); ++k)
                if (! used[k]) report (false, id + " " + got[k].first, "produced but not in the file: regenerate it (tools/reference.py)");
            std::printf ("%s %-62s %d checks\n", failures == before ? "PASS" : "FAIL", id.c_str(), checks - checksBefore);
        }
        std::printf ("\nLargest difference from the file, by tolerance:\n");
        for (const auto& [t, w] : worst) std::printf ("  %-16s %.3g (allowed %.3g)  %s\n", t.c_str(), w.first, w.second, w.first > 0 ? worstAt[t].c_str() : "");
        std::printf ("\n%s (%d checks, %d failed)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", checks, failures);
        return failures == 0 ? 0 : 1;
    }
}

int main (int argc, char** argv)
{
    if (argc == 4 && std::string (argv[1]) == "--compute") return compute (argv[2], argv[3]);
#ifdef ENCORE_AUDIO_ROOT_DIR
    std::string path = std::string (ENCORE_AUDIO_ROOT_DIR) + "/data/reference.json";
#else
    std::string path = "data/reference.json";
#endif
    if (argc == 2) path = argv[1];
    return check (path);
}
