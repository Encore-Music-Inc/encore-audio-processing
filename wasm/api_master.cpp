// The browser's mastering chain (mc_*): plain C functions around
// vs::MasterChain. Audio passes through two float buffers that live inside
// the module's memory. Built with the other wasm/api_*.cpp files into
// master.wasm by wasm/build.sh.
#include <emscripten/emscripten.h>
#include "dsp/MasterChain.h"
#include "dsp/MasterMonitor.h"

namespace
{
    struct Instance
    {
        vs::MasterChain chain;
        std::vector<float> l, r;
    };

    struct Monitor
    {
        vs::MasterMonitor m;
        std::vector<float> l, r;
    };

    vs::MasterChain::Settings settingsOf (int style, float intensity, float inputDb, float targetLufs, float ceilingDb,
                                          float glue, float width, float sat, const float* tone)
    {
        vs::MasterChain::Settings s;
        s.style = style; s.intensity = intensity; s.inputGainDb = inputDb; s.targetLufs = targetLufs; s.ceilingDb = ceilingDb;
        s.glue = glue; s.widthTrim = width; s.satTrim = sat;
        for (int b = 0; b < vs::MasterChain::numTone; ++b) s.toneTrim[b] = tone[b];
        return s;
    }

    void setUserEq (vs::MasterChain& chain, float low, float mid, float high)
    {
        auto s = chain.getSettings();
        s.userEq[0] = low; s.userEq[1] = mid; s.userEq[2] = high;
        chain.setSettings (s);
    }

    vs::MasterChain::Learned learnedOf (float pre, float drive, const float* t, const float* b)
    {
        vs::MasterChain::Learned l;
        l.valid = true; l.preGainDb = pre; l.driveDb = drive;
        for (int k = 0; k < 5; ++k) l.toneMatch[k] = t[k];
        for (int k = 0; k < 3; ++k) l.bandLevelDb[k] = b[k];
        return l;
    }

    float meterOf (const vs::MasterChain& c, int what)
    {
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
        setUserEq (static_cast<Instance*> (p)->chain, low, mid, high);
    }

    EMSCRIPTEN_KEEPALIVE void mc_settings (void* p, int style, float intensity, float inputDb, float targetLufs, float ceilingDb,
                                           float glue, float width, float sat, float t0, float t1, float t2, float t3, float t4)
    {
        const float t[] { t0, t1, t2, t3, t4 };
        static_cast<Instance*> (p)->chain.setSettings (settingsOf (style, intensity, inputDb, targetLufs, ceilingDb, glue, width, sat, t));
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
    EMSCRIPTEN_KEEPALIVE float mc_get (void* p, int what) { return meterOf (static_cast<Instance*> (p)->chain, what); }

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
        const float t[] { t0, t1, t2, t3, t4 }, b[] { b0, b1, b2 };
        static_cast<Instance*> (p)->chain.lockLearned (learnedOf (pre, drive, t, b));
    }

    // ---- The loudness targets: how many, each one's id and LUFS
    EMSCRIPTEN_KEEPALIVE int mc_target_count() { return vs::MasterChain::numTargets; }
    EMSCRIPTEN_KEEPALIVE const char* mc_target_id (int i) { return vs::MasterChain::targets[std::clamp (i, 0, vs::MasterChain::numTargets - 1)].id; }
    EMSCRIPTEN_KEEPALIVE float mc_target_lufs (int i) { return vs::MasterChain::targetLufs (i); }

    // ---- The master as you listen (mm_*, vs::MasterMonitor): the chain, "hear
    // the original", a streaming service's level and a playback device, with
    // a live spectrum. Per block: the song's samples in mm_left/right,
    // mm_process (h, n), what to play back in mm_left/right. Settings (then the
    // user EQ), learned values and meters as the mc_* functions; mm_get also
    // reads 14, the original's match gain (dB).
    EMSCRIPTEN_KEEPALIVE void* mm_create (double sampleRate, int maxBlock)
    {
        auto* m = new Monitor();
        m->l.assign ((size_t) maxBlock, 0.0f);
        m->r.assign ((size_t) maxBlock, 0.0f);
        m->m.prepare (sampleRate, maxBlock);
        return m;
    }
    EMSCRIPTEN_KEEPALIVE void mm_destroy (void* p) { delete static_cast<Monitor*> (p); }
    EMSCRIPTEN_KEEPALIVE float* mm_left (void* p)  { return static_cast<Monitor*> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* mm_right (void* p) { return static_cast<Monitor*> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE int mm_latency (void* p) { return static_cast<Monitor*> (p)->m.chain.getLatency(); }
    EMSCRIPTEN_KEEPALIVE void mm_settings (void* p, int style, float intensity, float inputDb, float targetLufs, float ceilingDb,
                                           float glue, float width, float sat, float t0, float t1, float t2, float t3, float t4)
    {
        const float t[] { t0, t1, t2, t3, t4 };
        static_cast<Monitor*> (p)->m.chain.setSettings (settingsOf (style, intensity, inputDb, targetLufs, ceilingDb, glue, width, sat, t));
    }
    EMSCRIPTEN_KEEPALIVE void mm_user_eq (void* p, float low, float mid, float high) { setUserEq (static_cast<Monitor*> (p)->m.chain, low, mid, high); }
    EMSCRIPTEN_KEEPALIVE void mm_lock_learned (void* p, float pre, float drive, float t0, float t1, float t2, float t3, float t4,
                                               float b0, float b1, float b2)
    {
        const float t[] { t0, t1, t2, t3, t4 }, b[] { b0, b1, b2 };
        static_cast<Monitor*> (p)->m.chain.lockLearned (learnedOf (pre, drive, t, b));
    }
    EMSCRIPTEN_KEEPALIVE void mm_hear_original (void* p, int on) { static_cast<Monitor*> (p)->m.setHearOriginal (on != 0); }
    // on 0: the original matched to the master live, no level change; on 1:
    // the master at masterDb and the original at originalDb
    EMSCRIPTEN_KEEPALIVE void mm_playback_level (void* p, int on, float masterDb, float originalDb)
    {
        static_cast<Monitor*> (p)->m.setPlaybackLevel (on != 0, masterDb, originalDb);
    }
    // A device by its place in the pr_device_* list (0 = studio monitors)
    EMSCRIPTEN_KEEPALIVE void mm_device (void* p, int device) { static_cast<Monitor*> (p)->m.setDevice (device); }
    EMSCRIPTEN_KEEPALIVE void mm_process (void* p, int n)
    {
        auto* m = static_cast<Monitor*> (p);
        m->m.process (m->l.data(), m->r.data(), n);
    }
    EMSCRIPTEN_KEEPALIVE float mm_get (void* p, int what)
    {
        auto& m = static_cast<Monitor*> (p)->m;
        return what == 14 ? m.ab.matchGainDb() : meterOf (m.chain, what);
    }
    // The spectrum of what plays now (its levels in dB with the analyser's
    // ballistics): mm_spectrum_bins of them, from 0 Hz to half the rate
    EMSCRIPTEN_KEEPALIVE int mm_spectrum_bins (void* p) { return static_cast<Monitor*> (p)->m.spectrum.bins(); }
    EMSCRIPTEN_KEEPALIVE const float* mm_spectrum (void* p)
    {
        auto& s = static_cast<Monitor*> (p)->m.spectrum;
        s.update();
        return s.levelsDb().data();
    }
}
