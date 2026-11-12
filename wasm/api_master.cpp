// The browser's mastering chain (mc_*): plain C functions around
// vs::MasterChain. Audio passes through two float buffers that live inside
// the module's memory. Built with the other wasm/api_*.cpp files into
// master.wasm by wasm/build.sh.
#include <emscripten/emscripten.h>
#include "dsp/MasterChain.h"

namespace
{
    struct Instance
    {
        vs::MasterChain chain;
        std::vector<float> l, r;
    };
}

extern "C"
{
    EMSCRIPTEN_KEEPALIVE void* mc_create (double sampleRate, int maxBlock)
    {
        auto* in = new Instance();
        in->l.assign ((size_t) maxBlock, 0.0f);
        in->r.assign ((size_t) maxBlock, 0.0f);
        in->chain.prepare (sampleRate, maxBlock);
        return in;
    }

    EMSCRIPTEN_KEEPALIVE void mc_destroy (void* p) { delete static_cast<Instance*> (p); }
    EMSCRIPTEN_KEEPALIVE float* mc_left (void* p)  { return static_cast<Instance*> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* mc_right (void* p) { return static_cast<Instance*> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE int mc_latency (void* p)  { return static_cast<Instance*> (p)->chain.getLatency(); }

    // Broad user EQ (low shelf 160 Hz, mid 1.2 kHz, high shelf 5 kHz), dB; call after mc_settings
    EMSCRIPTEN_KEEPALIVE void mc_user_eq (void* p, float low, float mid, float high)
    {
        auto& chain = static_cast<Instance*> (p)->chain;
        auto s = chain.getSettings();
        s.userEq[0] = low; s.userEq[1] = mid; s.userEq[2] = high;
        chain.setSettings (s);
    }

    EMSCRIPTEN_KEEPALIVE void mc_settings (void* p, int style, float intensity, float inputDb, float targetLufs, float ceilingDb,
                                           float glue, float width, float sat, float t0, float t1, float t2, float t3, float t4)
    {
        vs::MasterChain::Settings s;
        s.style = style; s.intensity = intensity; s.inputGainDb = inputDb; s.targetLufs = targetLufs; s.ceilingDb = ceilingDb;
        s.glue = glue; s.widthTrim = width; s.satTrim = sat;
        const float t[] { t0, t1, t2, t3, t4 };
        for (int b = 0; b < vs::MasterChain::numTone; ++b) s.toneTrim[b] = t[b];
        static_cast<Instance*> (p)->chain.setSettings (s);
    }

    // Processes n samples of the instance's own buffers in place
    EMSCRIPTEN_KEEPALIVE void mc_process (void* p, int n)
    {
        auto* in = static_cast<Instance*> (p);
        in->chain.process (in->l.data(), in->r.data(), n);
    }

    EMSCRIPTEN_KEEPALIVE void mc_learn (void* p)           { static_cast<Instance*> (p)->chain.startLearning(); }
    EMSCRIPTEN_KEEPALIVE void mc_finish_learning (void* p) { static_cast<Instance*> (p)->chain.finishLearningNow(); }

    // 0 out short-term, 1 out momentary, 2 out integrated, 3 in short-term,
    // 4 limiter gain reduction, 5 learn progress, 6 learning, 7 settling,
    // 8 learned valid, 9..13 current tone moves (dB)
    EMSCRIPTEN_KEEPALIVE float mc_get (void* p, int what)
    {
        auto& c = static_cast<Instance*> (p)->chain;
        switch (what)
        {
            case 0: return c.outMeter.shortTerm.load();
            case 1: return c.outMeter.momentary.load();
            case 2: return c.outMeter.integrated();
            case 3: return c.inMeter.shortTerm.load();
            case 4: return c.limiter.grDb.load();
            case 5: return c.learnProgress();
            case 6: return c.isLearning() ? 1.0f : 0.0f;
            case 7: return c.isSettling() ? 1.0f : 0.0f;
            case 8: return c.getLearned().valid ? 1.0f : 0.0f;
            default: return (what >= 9 && what < 14) ? c.toneDbNow (what - 9) : 0.0f;
        }
    }

    // Learned values: 0 valid, 1 pre-gain, 2 drive, 3..7 tone match, 8..10 band levels
    EMSCRIPTEN_KEEPALIVE float mc_learned (void* p, int i)
    {
        const auto& l = static_cast<Instance*> (p)->chain.getLearned();
        if (i == 0) return l.valid ? 1.0f : 0.0f;
        if (i == 1) return l.preGainDb;
        if (i == 2) return l.driveDb;
        if (i >= 3 && i < 8) return l.toneMatch[i - 3];
        if (i >= 8 && i < 11) return l.bandLevelDb[i - 8];
        return 0.0f;
    }

    // Calibration: change a style's voicing for every instance
    EMSCRIPTEN_KEEPALIVE void mc_override_style (int style, float t0, float t1, float t2, float t3, float t4,
                                                 float c0, float c1, float c2, float sat, float width, float monoHz)
    {
        const float t[] { t0, t1, t2, t3, t4 }, c[] { c0, c1, c2 };
        vs::MasterChain::overrideStyle (style, t, c, sat, width, monoHz);
    }

    EMSCRIPTEN_KEEPALIVE void mc_lock_learned (void* p, float pre, float drive, float t0, float t1, float t2, float t3, float t4,
                                               float b0, float b1, float b2)
    {
        vs::MasterChain::Learned l;
        l.valid = true; l.preGainDb = pre; l.driveDb = drive;
        const float t[] { t0, t1, t2, t3, t4 }, b[] { b0, b1, b2 };
        for (int k = 0; k < 5; ++k) l.toneMatch[k] = t[k];
        for (int k = 0; k < 3; ++k) l.bandLevelDb[k] = b[k];
        static_cast<Instance*> (p)->chain.lockLearned (l);
    }

    // ---- The loudness targets: how many, each one's id and LUFS
    EMSCRIPTEN_KEEPALIVE int mc_target_count() { return vs::MasterChain::numTargets; }
    EMSCRIPTEN_KEEPALIVE const char* mc_target_id (int i) { return vs::MasterChain::targets[std::clamp (i, 0, vs::MasterChain::numTargets - 1)].id; }
    EMSCRIPTEN_KEEPALIVE float mc_target_lufs (int i) { return vs::MasterChain::targetLufs (i); }
}
