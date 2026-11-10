// The browser's whole-song mastering (mt_*): plain C functions around
// vs::Mastering, which learns on the song's loudest 30 s and renders it to its
// loudness target with vs::MasterChain. A song is fed block by block through
// the instance's buffers, so its length isn't limited by the module's memory.
// Built with the other wasm/api_*.cpp files into master.wasm by wasm/build.sh.
//
// Order: mt_create, mt_settings (and mt_user_eq), then
//   pass 1: fill mt_left / mt_right, mt_energy; repeat over the whole song
//   pass 2: mt_window_from / mt_window_length name a stretch: feed it with
//           mt_window, then mt_learn (or mt_set_learned instead of passes 1 and 2)
//   pass 3: mt_begin_render; feed the whole song with mt_render, each call's
//           output (its return value: how many samples) in mt_out_left /
//           mt_out_right; mt_finish_render for the rest; then mt_next_pass:
//           1 means render again (the last render is the master).
#include <emscripten/emscripten.h>
#include <vector>
#include "dsp/Mastering.h"

namespace
{
    struct Instance
    {
        vs::Mastering m;
        vs::MasterChain::Settings settings;
        double fs = 48000.0, length = 0.0;
        std::vector<float> l, r, outL, outR;

        void restart() { m.begin (fs, settings, (long long) length); }
    };

    Instance* get (void* p) { return static_cast<Instance*> (p); }
}

extern "C"
{
    // length: the song's samples per channel; maxBlock: the most samples fed at once
    EMSCRIPTEN_KEEPALIVE void* mt_create (double sampleRate, double length, int maxBlock)
    {
        auto* in = new Instance();
        in->fs = sampleRate;
        in->length = length;
        in->l.assign ((size_t) maxBlock, 0.0f);
        in->r.assign ((size_t) maxBlock, 0.0f);
        // render() writes at most maxBlock + one chain block; finishing writes the chain's latency and two blocks
        const size_t out = (size_t) maxBlock + 4 * vs::Mastering::block + 16384;
        in->outL.assign (out, 0.0f);
        in->outR.assign (out, 0.0f);
        in->restart();
        return in;
    }

    EMSCRIPTEN_KEEPALIVE void mt_destroy (void* p)        { delete get (p); }
    EMSCRIPTEN_KEEPALIVE float* mt_left (void* p)         { return get (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* mt_right (void* p)        { return get (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE float* mt_out_left (void* p)     { return get (p)->outL.data(); }
    EMSCRIPTEN_KEEPALIVE float* mt_out_right (void* p)    { return get (p)->outR.data(); }

    // The chain's settings (as mc_settings); starts over from pass 1
    EMSCRIPTEN_KEEPALIVE void mt_settings (void* p, int style, float intensity, float inputDb, float targetLufs, float ceilingDb,
                                           float glue, float width, float sat, float t0, float t1, float t2, float t3, float t4)
    {
        auto& s = get (p)->settings;
        s.style = style; s.intensity = intensity; s.inputGainDb = inputDb; s.targetLufs = targetLufs; s.ceilingDb = ceilingDb;
        s.glue = glue; s.widthTrim = width; s.satTrim = sat;
        const float t[] { t0, t1, t2, t3, t4 };
        for (int b = 0; b < vs::MasterChain::numTone; ++b) s.toneTrim[b] = t[b];
        get (p)->restart();
    }

    // Broad user EQ (as mc_user_eq), dB; call after mt_settings; starts over from pass 1
    EMSCRIPTEN_KEEPALIVE void mt_user_eq (void* p, float low, float mid, float high)
    {
        auto& s = get (p)->settings;
        s.userEq[0] = low; s.userEq[1] = mid; s.userEq[2] = high;
        get (p)->restart();
    }

    // stereo 0: only mt_left is read (a mono song)
    EMSCRIPTEN_KEEPALIVE void mt_energy (void* p, int n, int stereo)
    {
        auto* in = get (p);
        in->m.feedEnergy (in->l.data(), stereo != 0 ? in->r.data() : nullptr, n);
    }

    EMSCRIPTEN_KEEPALIVE double mt_window_from (void* p)   { return (double) get (p)->m.learningWindow().from; }
    EMSCRIPTEN_KEEPALIVE double mt_window_length (void* p) { return (double) get (p)->m.learningWindow().length; }

    EMSCRIPTEN_KEEPALIVE void mt_window (void* p, int n, int stereo)
    {
        auto* in = get (p);
        in->m.feedWindow (in->l.data(), stereo != 0 ? in->r.data() : nullptr, n);
    }

    EMSCRIPTEN_KEEPALIVE void mt_learn (void* p) { get (p)->m.learn(); }

    // Learned values: 0 valid, 1 pre-gain, 2 drive, 3..7 tone match, 8..10 band levels (as mc_learned)
    EMSCRIPTEN_KEEPALIVE float mt_learned (void* p, int i)
    {
        const auto& l = get (p)->m.learned();
        if (i == 0) return l.valid ? 1.0f : 0.0f;
        if (i == 1) return l.preGainDb;
        if (i == 2) return l.driveDb;
        if (i >= 3 && i < 8) return l.toneMatch[i - 3];
        if (i >= 8 && i < 11) return l.bandLevelDb[i - 8];
        return 0.0f;
    }

    EMSCRIPTEN_KEEPALIVE void mt_set_learned (void* p, float pre, float drive, float t0, float t1, float t2, float t3, float t4,
                                              float b0, float b1, float b2)
    {
        vs::MasterChain::Learned l;
        l.preGainDb = pre; l.driveDb = drive;
        const float t[] { t0, t1, t2, t3, t4 }, b[] { b0, b1, b2 };
        for (int k = 0; k < 5; ++k) l.toneMatch[k] = t[k];
        for (int k = 0; k < 3; ++k) l.bandLevelDb[k] = b[k];
        get (p)->m.setLearned (l);
    }

    EMSCRIPTEN_KEEPALIVE void mt_begin_render (void* p) { get (p)->m.beginRender(); }
    EMSCRIPTEN_KEEPALIVE int mt_latency (void* p)       { return get (p)->m.getLatency(); }

    EMSCRIPTEN_KEEPALIVE int mt_render (void* p, int n, int stereo)
    {
        auto* in = get (p);
        return in->m.render (in->l.data(), stereo != 0 ? in->r.data() : nullptr, n, in->outL.data(), in->outR.data());
    }

    EMSCRIPTEN_KEEPALIVE int mt_finish_render (void* p)
    {
        auto* in = get (p);
        return in->m.finishRender (in->outL.data(), in->outR.data());
    }

    // The last render's integrated loudness, LUFS, and how many renders so far
    EMSCRIPTEN_KEEPALIVE float mt_lufs (void* p)    { return get (p)->m.renderedLufs(); }
    EMSCRIPTEN_KEEPALIVE int mt_renders (void* p)   { return get (p)->m.rendersDone(); }
    EMSCRIPTEN_KEEPALIVE int mt_next_pass (void* p) { return get (p)->m.nextPass() ? 1 : 0; }
}
