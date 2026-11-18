// The browser's live recordings (lv_*): finding the songs in a live set
// (dsp/LiveSet.h), the slow ride's gain track (LevelRider, dsp/Rider.h), and
// the inputs: how they're routed to the two channels heard and what's on each
// (dsp/Channels.h). Plain C functions; audio passes through float buffers in
// the module's memory (each object's input (c) holds up to the maxBlock
// samples it was made for). Reports cross as JSON text with the same names
// as the headers' structs: a function writing JSON to out (cap bytes) returns
// its length, or minus the room it needs (its length plus one) when cap is too
// small, so the call can be repeated. Built with the other wasm/api_*.cpp
// files into master.wasm by wasm/build.sh.
#include <emscripten/emscripten.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "dsp/Channels.h"
#include "dsp/Json.h"
#include "dsp/LiveSet.h"
#include "dsp/Rider.h"

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

    std::string setJson (const vs::liveset::Set& s)
    {
        using vs::liveset::kindName;
        Out o;
        o.open ('{').key ("seconds").num (s.seconds).key ("judged").boolean (s.judged);
        if (s.judged) o.key ("threshold").num (s.threshold).key ("peak").num (s.peak);
        o.key ("songs").open ('[');
        for (const auto& x : s.songs)
            o.next().open ('{').key ("n").num (x.n).key ("start").num (x.start).key ("end").num (x.end)
             .key ("confidence").num (x.confidence).key ("level").num (x.level).close ('}');
        o.close (']').key ("gaps").open ('[');
        for (const auto& g : s.gaps)
        {
            o.next().open ('{').key ("start").num (g.start).key ("end").num (g.end).key ("kind").str (kindName (g.kind)).key ("detail").open ('{');
            for (const auto& c : g.counts) o.key (kindName (c.first)).num (c.second);
            o.close ('}').close ('}');
        }
        o.close (']').key ("timeline").open ('[');
        for (auto k : s.timeline) o.next().str (kindName (k));
        o.close (']').close ('}');
        return o.text;
    }

    struct Set { vs::LiveSetAnalyzer a; Buffers in; };
    struct Ride { vs::LevelRider r; Buffers in; };
}

extern "C"
{
    // ---- the songs in a live set

    EMSCRIPTEN_KEEPALIVE void* lv_set_create (double fs, int maxBlock) { return new Set { vs::LiveSetAnalyzer (fs), buffers (2, maxBlock) }; }
    EMSCRIPTEN_KEEPALIVE void lv_set_destroy (void* p) { delete static_cast<Set*> (p); }
    EMSCRIPTEN_KEEPALIVE float* lv_set_input (void* p, int c) { return static_cast<Set*> (p)->in[(size_t) c].data(); }
    // The next n samples of each channel's input (stereo 0: the left alone)
    EMSCRIPTEN_KEEPALIVE void lv_set_push (void* p, int n, int stereo)
    {
        auto* h = static_cast<Set*> (p);
        h->a.push (h->in[0].data(), stereo ? h->in[1].data() : nullptr, n);
    }
    // The songs, the gaps between them and each second's kind, as JSON
    EMSCRIPTEN_KEEPALIVE int lv_set_finish (void* p, double minSongSec, double mergeGapSec, char* out, int cap)
    {
        vs::liveset::Options o;
        o.minSongSec = minSongSec;
        o.mergeGapSec = mergeGapSec;
        return give (setJson (static_cast<Set*> (p)->a.finish (o)), out, cap);
    }

    // ---- the slow ride: a gain track planned over a whole recording

    EMSCRIPTEN_KEEPALIVE void* lv_ride_create (double fs, double targetDb, double rangeDb, int maxBlock) { return new Ride { vs::LevelRider (fs, targetDb, rangeDb), buffers (2, maxBlock) }; }
    EMSCRIPTEN_KEEPALIVE void lv_ride_destroy (void* p) { delete static_cast<Ride*> (p); }
    EMSCRIPTEN_KEEPALIVE float* lv_ride_input (void* p, int c) { return static_cast<Ride*> (p)->in[(size_t) c].data(); }
    EMSCRIPTEN_KEEPALIVE void lv_ride_push (void* p, int n, int stereo)
    {
        auto* h = static_cast<Ride*> (p);
        h->r.push (h->in[0].data(), stereo ? h->in[1].data() : nullptr, n);
    }
    // the gain (dB) for each 0.1 s so far: how many, and where they are
    EMSCRIPTEN_KEEPALIVE int lv_ride_count (void* p) { return (int) static_cast<Ride*> (p)->r.track().size(); }
    EMSCRIPTEN_KEEPALIVE const float* lv_ride_track (void* p) { return static_cast<Ride*> (p)->r.track().data(); }
    EMSCRIPTEN_KEEPALIVE double lv_ride_level_db (void* p) { return static_cast<Ride*> (p)->r.levelDb(); }

    // ---- the inputs heard as two channels (raw: numCh pointers to count + delay frames each)

    EMSCRIPTEN_KEEPALIVE void lv_route (const float* const* raw, int numCh, int count, int delay, int mainL, int mainR, int mono,
                                        int roomL, int roomR, double roomDb, float* outL, float* outR)
    {
        vs::channels::routeLive (raw, numCh, count, delay, mainL, mainR, mono != 0, roomL, roomR, roomDb, outL, outR);
    }

    // ---- what's on each input, from a dozen windows spread across the recording

    EMSCRIPTEN_KEEPALIVE int lv_survey_spots() { return vs::channels::InputSurvey::spots; }
    EMSCRIPTEN_KEEPALIVE double lv_survey_window_start (int k, double frames, double fs) { return (double) vs::channels::InputSurvey::windowStart (k, (std::int64_t) frames, fs); }
    EMSCRIPTEN_KEEPALIVE int lv_survey_window_length (double frames, double fs) { return vs::channels::InputSurvey::windowLength ((std::int64_t) frames, fs); }
    EMSCRIPTEN_KEEPALIVE void* lv_survey_create (int numCh) { return new vs::channels::InputSurvey (numCh); }
    EMSCRIPTEN_KEEPALIVE void lv_survey_destroy (void* p) { delete static_cast<vs::channels::InputSurvey*> (p); }
    EMSCRIPTEN_KEEPALIVE void lv_survey_add (void* p, const float* const* raw, int n) { static_cast<vs::channels::InputSurvey*> (p)->add (raw, n); }
    EMSCRIPTEN_KEEPALIVE double lv_survey_level_db (void* p, int c) { return static_cast<vs::channels::InputSurvey*> (p)->levelDb (c); }
    EMSCRIPTEN_KEEPALIVE double lv_survey_next_corr (void* p, int c) { return static_cast<vs::channels::InputSurvey*> (p)->nextCorr (c); }
}
