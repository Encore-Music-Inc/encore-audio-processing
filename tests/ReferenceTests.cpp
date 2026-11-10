// The reference set (data/reference.json): makes each case's input with
// dsp/ReferenceSignals.h, runs it through the audio processing natively and
// checks every expected result within its tolerance. Fails when a result is
// off, when an input is not the one the file names (its hash), when the file
// has a result this run did not produce, or when this run produced one the
// file does not have. wasm/reference.mjs checks the same file through
// master.wasm.
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
#include "dsp/DrumSynth.h"
#include "dsp/Loudness.h"
#include "dsp/LoudnessReport.h"
#include "dsp/Mastering.h"
#include "dsp/Metronome.h"
#include "dsp/Onsets.h"
#include "dsp/ReferenceSignals.h"
#include "dsp/Tempo.h"
#include "dsp/TruePeak.h"
#include "dsp/TunerPitch.h"
#include "dsp/Yin.h"

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
            const double t = type == "noise" ? 1 : type == "samples" ? 2 : 0;
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
        else out.push_back ({ "unknown op " + op, std::string() });
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
                if (diff > w.first) w = { diff, allow };
                char b[160];
                std::snprintf (b, sizeof (b), "expected %.17g, got %.17g (off by %.3g, %s allows %.3g)", value.num, *d, diff, tol.c_str(), allow);
                report (allow > 0 ? diff <= allow : diff == 0.0, id + " " + name, b);
            }
            for (size_t k = 0; k < got.size(); ++k)
                if (! used[k]) report (false, id + " " + got[k].first, "produced but not in the file: regenerate it (tools/reference.py)");
            std::printf ("%s %-62s %d checks\n", failures == before ? "PASS" : "FAIL", id.c_str(), checks - checksBefore);
        }
        std::printf ("\nLargest difference from the file, by tolerance:\n");
        for (const auto& [t, w] : worst) std::printf ("  %-16s %.3g (allowed %.3g)\n", t.c_str(), w.first, w.second);
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
