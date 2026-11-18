// The browser's restoration of record and tape transfers (rs_*): the
// restoration chain (dsp/Restoration.h), the analysis pass and its detectors,
// the noise profile, the channel comparison, the click count, the clip check
// and a side's level envelope (dsp/RestorationAnalysis.h). Plain C functions;
// audio passes through float buffers in the module's memory (each object's
// input (c) holds up to the maxBlock samples it was made for). Settings and
// reports cross as JSON text with the same names as the headers' structs:
// a function writing JSON to out (cap bytes) returns its length, or minus the
// room it needs (its length plus one) when cap is too small, so the call can
// be repeated. Built with the other wasm/api_*.cpp files into master.wasm by
// wasm/build.sh.
#include <emscripten/emscripten.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "dsp/Analysis.h"
#include "dsp/Json.h"
#include "dsp/Mix.h"
#include "dsp/Restoration.h"
#include "dsp/RestorationAnalysis.h"

namespace
{
    using vs::json::Out;
    using Buffers = std::vector<std::vector<float>>;
    Buffers buffers (int channels, int n) { return Buffers ((size_t) channels, std::vector<float> ((size_t) std::max (1, n), 0.0f)); }

    int give (const std::string& text, char* out, int cap)
    {
        const int need = (int) text.size() + 1;
        if (need > cap) { if (cap > 0) out[0] = 0; return -need; }
        std::memcpy (out, text.c_str(), (size_t) need);
        return need - 1;
    }

    void floats (Out& o, const std::vector<float>& v) { o.open ('['); for (float x : v) o.next().num (x); o.close (']'); }
    void optNum (Out& o, const std::optional<double>& v) { if (v) o.num (*v); else o.null(); }

    std::string clicksJson (const std::vector<vs::declick::Event>& ev)
    {
        Out o;
        o.open ('[');
        for (const auto& e : ev)
            o.next().open ('{').key ("at").num ((double) e.at).key ("len").num (e.len).key ("ch").num (e.ch).key ("kind").str (vs::declick::kindName (e.kind)).close ('}');
        o.close (']');
        return o.text;
    }

    struct Chain { std::unique_ptr<vs::restore::Chain> chain; Buffers in; };
    struct Analyze
    {
        std::unique_ptr<vs::restore::RestoreAnalyzer> a;
        Buffers in;
        std::vector<float> profile;
        int profileN = 0;
        double profileFs = 0;
        vs::restore::Wow wow;
        std::vector<float> curve;   // the last wow measured, for rs_analyzer_wow_curve
    };
    struct Profile { std::unique_ptr<vs::NoiseProfileLearner> p; Buffers in; std::vector<float> result; int n = 0; };
    struct Compare { vs::restore::ChannelCompare c; Buffers in; };
    struct Count { vs::restore::ClickCounter c; Buffers in; };
    struct Clip { vs::FlatRuns runs[2]; bool on[2] { false, false }; Buffers in; };
    struct Envelope { vs::restore::LevelEnvelope e; Buffers in; };
    struct Overview { std::unique_ptr<vs::restore::OverviewMeter> m; Buffers in; vs::restore::Overview result; };
}

extern "C"
{
    // ---- settings and their lists

    // A kind of transfer's starting settings ("lp", "78", "cassette", "8track", "reel", "cd", "other") as JSON.
    EMSCRIPTEN_KEEPALIVE int rs_defaults (const char* source, char* out, int cap)
    {
        Out o;
        vs::restore::settingsToJson (vs::restore::defaults (source), o);
        return give (o.text, out, cap);
    }

    // The manual speed fixes [{id, from, to, cents}] and the 78 playback curves
    // [{id, turnover, rolloff, fixed}] as JSON (their names are the product's).
    EMSCRIPTEN_KEEPALIVE int rs_lists (char* out, int cap)
    {
        Out o;
        o.open ('{').key ("rpm").open ('[');
        for (const auto& f : vs::restore::rpmFixes())
            o.next().open ('{').key ("id").str (f.id).key ("from").num (f.from).key ("to").num (f.to).key ("cents").num (vs::restore::rpmCents (f.id)).close ('}');
        o.close (']').key ("curves").open ('[');
        for (const auto& c : vs::restore::playbackCurves())
            o.next().open ('{').key ("id").str (c.id).key ("turnover").num (c.turnover).key ("rolloff").num (c.rolloff).key ("fixed").boolean (c.fixed).close ('}');
        o.close (']').close ('}');
        return give (o.text, out, cap);
    }

    // The whole-recording speed fix in cents for settings (JSON); NaN for settings it can't read.
    EMSCRIPTEN_KEEPALIVE double rs_speed_cents (const char* settings)
    {
        const auto j = vs::json::parse (settings);
        if (! j) return std::numeric_limits<double>::quiet_NaN();
        return vs::restore::speedCents (vs::restore::settingsFromJson (*j));
    }

    EMSCRIPTEN_KEEPALIVE double rs_to_restored_time (double t, double cents) { return vs::restore::toRestoredTime (t, cents); }
    EMSCRIPTEN_KEEPALIVE double rs_to_original_time (double t, double cents) { return vs::restore::toOriginalTime (t, cents); }
    EMSCRIPTEN_KEEPALIVE double rs_export_gain (double peakDb) { return vs::restore::exportGain (peakDb); }
    EMSCRIPTEN_KEEPALIVE double rs_denoise_amount_db (double amount) { return vs::denoise::amountToDb (amount); }
    EMSCRIPTEN_KEEPALIVE int rs_denoise_frame (double fs) { return vs::denoise::frameSize (fs); }
    // The level (dBFS RMS per channel, to 0.1 dB) a noise profile of frame size n describes.
    EMSCRIPTEN_KEEPALIVE double rs_profile_db_of (const float* prof, int bins, int n)
    {
        return vs::restore::rounded (vs::denoise::profileDb (std::vector<float> (prof, prof + bins), n), 1);
    }

    // ---- the restoration chain

    // A chain for settings (JSON). The run: leave out the de-clicker, de-hum or
    // noise reduction (for "what was removed"), keep the left-out ones' delay,
    // and the input sample this run starts at. Null for settings it can't read.
    EMSCRIPTEN_KEEPALIVE void* rs_chain_create (double fs, const char* settings, int offDeclick, int offDehum, int offDenoise, int keepDelays, double offset, int maxBlock)
    {
        const auto j = vs::json::parse (settings);
        if (! j || j->type != vs::json::Json::Type::object) return nullptr;
        vs::restore::Run run;
        run.offDeclick = offDeclick != 0; run.offDehum = offDehum != 0; run.offDenoise = offDenoise != 0;
        run.keepDelays = keepDelays != 0; run.offset = offset;
        return new Chain { std::make_unique<vs::restore::Chain> (fs, vs::restore::settingsFromJson (*j), run), buffers (2, maxBlock) };
    }
    EMSCRIPTEN_KEEPALIVE void rs_chain_destroy (void* p) { delete static_cast<Chain*> (p); }
    EMSCRIPTEN_KEEPALIVE float* rs_chain_input (void* p, int c) { return static_cast<Chain*> (p)->in[(size_t) c].data(); }
    // Restores the n samples in input (0) and input (1); returns how many are ready in output (c).
    EMSCRIPTEN_KEEPALIVE int rs_chain_push (void* p, int n)
    {
        auto* h = static_cast<Chain*> (p);
        return h->chain->push (h->in[0].data(), h->in[1].data(), n);
    }
    EMSCRIPTEN_KEEPALIVE int rs_chain_flush (void* p) { return static_cast<Chain*> (p)->chain->flush(); }
    EMSCRIPTEN_KEEPALIVE const float* rs_chain_output (void* p, int c) { return static_cast<Chain*> (p)->chain->output (c).data(); }
    // What the de-clicker found so far: [{at, len, ch, kind ("click" | "crackle" | "long")}].
    EMSCRIPTEN_KEEPALIVE int rs_chain_clicks (void* p, char* out, int cap) { return give (clicksJson (static_cast<Chain*> (p)->chain->clicks()), out, cap); }

    // ---- the analysis pass

    EMSCRIPTEN_KEEPALIVE void* rs_analyzer_create (double fs, double frames, int columns, int maxBlock)
    {
        return new Analyze { std::make_unique<vs::restore::RestoreAnalyzer> (fs, (std::int64_t) frames, columns), buffers (2, maxBlock), {}, 0, 0, {}, {} };
    }
    EMSCRIPTEN_KEEPALIVE void rs_analyzer_destroy (void* p) { delete static_cast<Analyze*> (p); }
    EMSCRIPTEN_KEEPALIVE float* rs_analyzer_input (void* p, int c) { return static_cast<Analyze*> (p)->in[(size_t) c].data(); }
    EMSCRIPTEN_KEEPALIVE void rs_analyzer_push (void* p, int n) { auto* h = static_cast<Analyze*> (p); h->a->push (h->in[0].data(), h->in[1].data(), n); }
    EMSCRIPTEN_KEEPALIVE void rs_analyzer_finish (void* p) { static_cast<Analyze*> (p)->a->finish(); }

    // The overview after finish: which 0 left peaks, 1 right peaks, 2 RMS; columns many.
    EMSCRIPTEN_KEEPALIVE const float* rs_analyzer_overview (void* p, int which)
    {
        const auto& ov = static_cast<Analyze*> (p)->a->overview();
        return (which == 0 ? ov.peakL : which == 1 ? ov.peakR : ov.rms).data();
    }
    EMSCRIPTEN_KEEPALIVE int rs_analyzer_overview_per (void* p) { return static_cast<Analyze*> (p)->a->overview().per; }

    // The noise profile (bins, frame size n at rate fs) the dropout check measures the hiss from.
    EMSCRIPTEN_KEEPALIVE void rs_analyzer_set_profile (void* p, const float* prof, int bins, int n, double fs)
    {
        auto* h = static_cast<Analyze*> (p);
        h->profile.assign (prof, prof + bins);
        h->profileN = n;
        h->profileFs = fs;
    }

    // One part of the report, after finish. request (JSON): {"what": ..., and its options}.
    // what: "summary" (rates, peaks, DC, the music's level, the frame size and hop,
    // per-second channel sums), "hum", "tuning" ("a4", default 440), "wow" (its curve then
    // in rs_analyzer_wow_curve), "tracks" ("minGap" 1.2 and "minSong" 20 seconds),
    // "dropouts", "quietSpots", "checkWindows".
    EMSCRIPTEN_KEEPALIVE int rs_analyzer_report (void* p, const char* request, char* out, int cap)
    {
        auto* h = static_cast<Analyze*> (p);
        const auto& a = *h->a;
        const auto q = vs::json::parse (request);
        auto option = [&] (const char* k, double fallback)
        {
            const auto* v = q ? q->get (k) : nullptr;
            return v != nullptr && v->type == vs::json::Json::Type::number ? v->n : fallback;
        };
        const auto* whatField = q ? q->get ("what") : nullptr;
        const std::string w = whatField != nullptr && whatField->type == vs::json::Json::Type::string ? whatField->s : std::string();
        const double a4 = option ("a4", 440.0);
        Out o;
        if (w == "summary")
        {
            o.open ('{').key ("fs").num (a.sampleRate()).key ("fsd").num (a.lowRate()).key ("frames").num ((double) a.framesRead())
             .key ("peakDb").num (a.peakDb()).key ("peakL").num (a.channelPeak (0)).key ("peakR").num (a.channelPeak (1))
             .key ("dc").open ('[').next().num (a.dc (0)).next().num (a.dc (1)).close (']');
            o.key ("musicDb"); optNum (o, a.musicDb());
            o.key ("N").num (a.frameSize()).key ("hop").num (a.hopSize()).key ("columns").num (a.overview().cols).key ("per").num (a.overview().per);
            o.key ("seconds1").open ('[');
            for (const auto& s : a.perSecond()) o.next().open ('{').key ("ll").num (s.ll).key ("rr").num (s.rr).key ("lr").num (s.lr).close ('}');
            o.close (']').close ('}');
        }
        else if (w == "hum")
        {
            const auto hum = vs::restore::detectHum (a.lowLeft(), a.lowRight(), a.lowRate());
            o.open ('{').key ("found").boolean (hum.found).key ("hz").num (hum.hz).key ("harmonics").open ('[');
            for (const auto& x : hum.harmonics) o.next().open ('{').key ("h").num (x.h).key ("prom").num (x.prom).key ("hz").num (x.hz).close ('}');
            o.close (']').key ("levelDb"); optNum (o, hum.levelDb);
            o.key ("drift").num (hum.drift).key ("score").num (hum.score).close ('}');
        }
        else if (w == "tuning")
        {
            const auto t = vs::restore::detectTuning (a.lowMid(), a.lowRate(), a4 > 0 ? a4 : 440.0);
            o.open ('{').key ("found").boolean (t.found).key ("cents").num (t.cents).key ("confidence").num (t.confidence)
             .key ("peaks").num (t.peaks).key ("voiced").num (t.voiced).close ('}');
        }
        else if (w == "wow")
        {
            h->wow = vs::restore::detectWow (a.lowMid(), a.lowRate());
            h->curve = h->wow.cents;
            o.open ('{').key ("found").boolean (h->wow.found).key ("rate").num (h->wow.rate).key ("length").num ((double) h->curve.size())
             .key ("confidence").num (h->wow.confidence).key ("peak").num (h->wow.peak).key ("rms").num (h->wow.rms)
             .key ("hz").num (h->wow.hz).key ("prominence").num (h->wow.prominence).close ('}');
        }
        else if (w == "tracks")
        {
            const auto t = vs::restore::findTracks (a.lowMid(), a.lowRate(), option ("minGap", 1.2), option ("minSong", 20.0));
            o.open ('{').key ("tracks").open ('[');
            for (const auto& s : t.tracks) o.next().open ('{').key ("start").num (s.start).key ("end").num (s.end).close ('}');
            o.close (']').key ("gaps").open ('[');
            for (const auto& s : t.gaps) o.next().open ('{').key ("start").num (s.start).key ("end").num (s.end).close ('}');
            o.close (']').key ("seconds").num (t.seconds);
            if (t.floorDb) o.key ("floorDb").num (*t.floorDb);
            if (t.musicDb) o.key ("musicDb").num (*t.musicDb);
            o.close ('}');
        }
        else if (w == "dropouts")
        {
            const auto d = vs::restore::detectDropouts (a.lowLeft(), a.lowRight(), a.lowRate(), h->profile.empty() ? nullptr : &h->profile, h->profileN, h->profileFs);
            o.open ('[');
            for (const auto& x : d) o.next().open ('{').key ("start").num (x.start).key ("end").num (x.end).key ("depthDb").num (x.depthDb).key ("ch").num (x.ch).close ('}');
            o.close (']');
        }
        else if (w == "quietSpots")
        {
            o.open ('[');
            for (auto at : vs::restore::quietSpots (a.hopEnergy(), a.hopSize())) o.next().num ((double) at);
            o.close (']');
        }
        else if (w == "checkWindows")
        {
            o.open ('[');
            for (const auto& x : vs::restore::checkWindows (a.framesRead(), a.sampleRate())) o.next().open ('{').key ("at").num ((double) x.at).key ("length").num (x.length).close ('}');
            o.close (']');
        }
        else o.open ('{').key ("error").str ("no such report: " + w).close ('}');
        return give (o.text, out, cap);
    }

    // The wow curve the last "wow" report measured (cents, rate values a second), its length in that report.
    EMSCRIPTEN_KEEPALIVE const float* rs_analyzer_wow_curve (void* p) { return static_cast<Analyze*> (p)->curve.data(); }

    // ---- the noise profile

    // Learns from frames of n samples per channel; humHz: the hum lines to set aside (count of them).
    EMSCRIPTEN_KEEPALIVE void* rs_profile_create (double fs, int n, const double* humHz, int hums)
    {
        return new Profile { std::make_unique<vs::NoiseProfileLearner> (fs, n, std::vector<double> (humHz, humHz + std::max (0, hums))), buffers (2, n), {}, n };
    }
    EMSCRIPTEN_KEEPALIVE void rs_profile_destroy (void* p) { delete static_cast<Profile*> (p); }
    EMSCRIPTEN_KEEPALIVE float* rs_profile_input (void* p, int c) { return static_cast<Profile*> (p)->in[(size_t) c].data(); }
    // Takes the frame in input (0) and input (1). 1 if kept, 0 if not (a click or a note, or it has enough).
    EMSCRIPTEN_KEEPALIVE int rs_profile_add (void* p) { auto* h = static_cast<Profile*> (p); return h->p->add (h->in[0].data(), h->in[1].data()) ? 1 : 0; }
    EMSCRIPTEN_KEEPALIVE int rs_profile_full (void* p) { return static_cast<Profile*> (p)->p->full() ? 1 : 0; }
    EMSCRIPTEN_KEEPALIVE int rs_profile_frames (void* p) { return static_cast<Profile*> (p)->p->framesKept(); }
    // Works the profile out: its number of bins (n/2 + 1), or 0 when too few frames were kept. Then rs_profile_data.
    EMSCRIPTEN_KEEPALIVE int rs_profile_finish (void* p)
    {
        auto* h = static_cast<Profile*> (p);
        const auto r = h->p->profile();
        h->result = r ? *r : std::vector<float> {};
        return (int) h->result.size();
    }
    EMSCRIPTEN_KEEPALIVE const float* rs_profile_data (void* p) { return static_cast<Profile*> (p)->result.data(); }

    // ---- how the channels compare (loud windows)

    EMSCRIPTEN_KEEPALIVE void* rs_channels_create (double fs, int maxBlock) { return new Compare { vs::restore::ChannelCompare (fs), buffers (2, maxBlock) }; }
    EMSCRIPTEN_KEEPALIVE void rs_channels_destroy (void* p) { delete static_cast<Compare*> (p); }
    EMSCRIPTEN_KEEPALIVE float* rs_channels_input (void* p, int c) { return static_cast<Compare*> (p)->in[(size_t) c].data(); }
    EMSCRIPTEN_KEEPALIVE void rs_channels_add (void* p, int n) { auto* h = static_cast<Compare*> (p); h->c.add (h->in[0].data(), h->in[1].data(), n); }
    // How loud the window in input is (for picking the loud ones).
    EMSCRIPTEN_KEEPALIVE double rs_window_energy (void* p, int n) { auto* h = static_cast<Compare*> (p); return vs::restore::windowEnergy (h->in[0].data(), h->in[1].data(), n); }
    EMSCRIPTEN_KEEPALIVE int rs_channels_result (void* p, char* out, int cap)
    {
        const auto r = static_cast<Compare*> (p)->c.result();
        Out o;
        o.open ('{').key ("corr").num (r.corr).key ("sideDb").num (r.sideDb).key ("balanceDb").num (r.balanceDb).key ("delay").num (r.delay)
         .key ("delayUs").num (r.delayUs).key ("delayN").num (r.delayN).key ("mono").boolean (r.mono).key ("nearlyMono").boolean (r.nearlyMono)
         .key ("flipped").boolean (r.flipped).close ('}');
        return give (o.text, out, cap);
    }

    // ---- how clicky it is (windows across the file)

    EMSCRIPTEN_KEEPALIVE void* rs_clicks_create (double fs, double clicks, double crackle, int maxBlock) { return new Count { vs::restore::ClickCounter (fs, clicks, crackle), buffers (2, maxBlock) }; }
    EMSCRIPTEN_KEEPALIVE void rs_clicks_destroy (void* p) { delete static_cast<Count*> (p); }
    EMSCRIPTEN_KEEPALIVE float* rs_clicks_input (void* p, int c) { return static_cast<Count*> (p)->in[(size_t) c].data(); }
    // The window in input, n samples starting at sample `at` of the file.
    EMSCRIPTEN_KEEPALIVE void rs_clicks_add (void* p, int n, double at) { auto* h = static_cast<Count*> (p); h->c.add (h->in[0].data(), h->in[1].data(), n, (std::int64_t) at); }
    EMSCRIPTEN_KEEPALIVE int rs_clicks_result (void* p, char* out, int cap)
    {
        const auto r = static_cast<Count*> (p)->c.result();
        Out o;
        o.open ('{').key ("seconds").num (r.seconds);
        o.key ("left").open ('{').key ("click").num (r.click[0]).key ("crackle").num (r.crackle[0]).close ('}');
        o.key ("right").open ('{').key ("click").num (r.click[1]).key ("crackle").num (r.crackle[1]).close ('}');
        o.key ("perMinute").open ('[').next().num (r.perMinute[0]).next().num (r.perMinute[1]).close (']');
        o.key ("positions").open ('[');
        for (auto x : r.positions) o.next().num ((double) x);
        o.close (']').close ('}');
        return give (o.text, out, cap);
    }

    // ---- clipping (a second pass, each channel's peak known from the first)

    EMSCRIPTEN_KEEPALIVE void* rs_clip_create (double fs, float peakL, float peakR, int bitDepth, int isFloat, int maxBlock)
    {
        auto* h = new Clip;
        const double tol = vs::FlatRuns::tolerance (bitDepth, isFloat != 0);
        const float peaks[2] { peakL, peakR };
        for (int c = 0; c < 2; ++c)
            if ((h->on[c] = vs::FlatRuns::checks (peaks[c]))) h->runs[c].prepare (peaks[c], tol, fs);
        h->in = buffers (2, maxBlock);
        return h;
    }
    EMSCRIPTEN_KEEPALIVE void rs_clip_destroy (void* p) { delete static_cast<Clip*> (p); }
    EMSCRIPTEN_KEEPALIVE float* rs_clip_input (void* p, int c) { return static_cast<Clip*> (p)->in[(size_t) c].data(); }
    EMSCRIPTEN_KEEPALIVE void rs_clip_push (void* p, int n)
    {
        auto* h = static_cast<Clip*> (p);
        for (int c = 0; c < 2; ++c) if (h->on[c]) h->runs[c].process (h->in[(size_t) c].data(), n);
    }
    // After the last push: {runs, longest, events: [{start, end, n}] (seconds, both channels as one list)}.
    EMSCRIPTEN_KEEPALIVE int rs_clip_result (void* p, char* out, int cap)
    {
        auto* h = static_cast<Clip*> (p);
        std::vector<vs::FlatRuns::Event> all;
        int runs = 0, longest = 0;
        for (int c = 0; c < 2; ++c)
            if (h->on[c])
            {
                h->runs[c].finish();
                runs += h->runs[c].runs();
                longest = std::max (longest, h->runs[c].longest());
                all.insert (all.end(), h->runs[c].events().begin(), h->runs[c].events().end());
                h->on[c] = false;
            }
        const auto merged = vs::FlatRuns::merge (std::move (all));
        Out o;
        o.open ('{').key ("runs").num (runs).key ("longest").num (longest).key ("events").open ('[');
        for (const auto& e : merged) o.next().open ('{').key ("start").num (e.start).key ("end").num (e.end).key ("n").num (e.n).close ('}');
        o.close (']').close ('}');
        return give (o.text, out, cap);
    }

    // ---- a side's level envelope, and the gaps in it

    EMSCRIPTEN_KEEPALIVE void* rs_envelope_create (double fs, double hopSeconds, int maxBlock) { return new Envelope { vs::restore::LevelEnvelope (fs, hopSeconds), buffers (2, maxBlock) }; }
    EMSCRIPTEN_KEEPALIVE void rs_envelope_destroy (void* p) { delete static_cast<Envelope*> (p); }
    EMSCRIPTEN_KEEPALIVE float* rs_envelope_input (void* p, int c) { return static_cast<Envelope*> (p)->in[(size_t) c].data(); }
    EMSCRIPTEN_KEEPALIVE void rs_envelope_push (void* p, int n) { auto* h = static_cast<Envelope*> (p); h->e.push (h->in[0].data(), h->in[1].data(), n); }
    EMSCRIPTEN_KEEPALIVE int rs_envelope_count (void* p) { return (int) static_cast<Envelope*> (p)->e.levels().size(); }
    EMSCRIPTEN_KEEPALIVE const float* rs_envelope_levels (void* p) { return static_cast<Envelope*> (p)->e.levels().data(); }
    EMSCRIPTEN_KEEPALIVE double rs_envelope_hop (void* p) { return static_cast<Envelope*> (p)->e.hopSeconds(); }

    // The quiet dips in a level envelope (n levels in dB, hop seconds apart):
    // {cands: [{start, end, at, min, depth, score}], musicStart, musicEnd, seconds, floor, music}.
    EMSCRIPTEN_KEEPALIVE int rs_gap_candidates (const float* levels, int n, double hop, char* out, int cap)
    {
        const auto g = vs::restore::gapCandidates (std::vector<float> (levels, levels + n), hop);
        Out o;
        o.open ('{').key ("cands").open ('[');
        for (const auto& c : g.cands)
            o.next().open ('{').key ("start").num (c.start).key ("end").num (c.end).key ("at").num (c.at).key ("min").num (c.min)
             .key ("depth").num (c.depth).key ("score").num (c.score).close ('}');
        o.close (']').key ("musicStart").num (g.musicStart).key ("musicEnd").num (g.musicEnd).key ("seconds").num (g.seconds)
         .key ("floor").num (g.floor).key ("music").num (g.music).close ('}');
        return give (o.text, out, cap);
    }

    // ---- a waveform overview of a stretch of known length (the restored file's)

    EMSCRIPTEN_KEEPALIVE void* rs_overview_create (double frames, int columns, int maxBlock)
    {
        return new Overview { std::make_unique<vs::restore::OverviewMeter> ((std::int64_t) frames, columns), buffers (2, maxBlock), {} };
    }
    EMSCRIPTEN_KEEPALIVE void rs_overview_destroy (void* p) { delete static_cast<Overview*> (p); }
    EMSCRIPTEN_KEEPALIVE float* rs_overview_input (void* p, int c) { return static_cast<Overview*> (p)->in[(size_t) c].data(); }
    EMSCRIPTEN_KEEPALIVE void rs_overview_push (void* p, int n) { auto* h = static_cast<Overview*> (p); h->m->push (h->in[0].data(), h->in[1].data(), n); }
    EMSCRIPTEN_KEEPALIVE double rs_overview_peak_db (void* p) { return static_cast<Overview*> (p)->m->peakDb(); }
    EMSCRIPTEN_KEEPALIVE int rs_overview_per (void* p) { auto* h = static_cast<Overview*> (p); h->result = h->m->result(); return h->result.per; }
    // After rs_overview_per: which 0 left peaks, 1 right peaks, 2 RMS.
    EMSCRIPTEN_KEEPALIVE const float* rs_overview_data (void* p, int which)
    {
        const auto& r = static_cast<Overview*> (p)->result;
        return (which == 0 ? r.peakL : which == 1 ? r.peakR : r.rms).data();
    }
}
