// WebAssembly bridge for the browser: the ENCORE. mastering chain (mc_*),
// the loudness meter (lm_*) and the tuner's pitch detector (tp_*). The
// browser calls these plain C functions; audio is passed through float
// buffers that live inside the module's memory.
#include <emscripten/emscripten.h>
#include "dsp/MasterChain.h"
#include "dsp/TunerPitch.h"

namespace
{
    struct Instance
    {
        vs::MasterChain chain;
        std::vector<float> l, r;
    };

    struct Meter
    {
        vs::LoudnessMeter meter;
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

    // Loudness meter (BS.1770, the one the mastering chain uses): fill lm_left
    // and lm_right with up to maxBlock samples, lm_process them, repeat, then
    // read lm_integrated. A song is measured block by block, so its length
    // isn't limited by the module's memory; the meter counts its first hour.
    EMSCRIPTEN_KEEPALIVE void* lm_create (double sampleRate, int maxBlock)
    {
        auto* m = new Meter();
        m->l.assign ((size_t) maxBlock, 0.0f);
        m->r.assign ((size_t) maxBlock, 0.0f);
        m->meter.prepare (sampleRate);
        return m;
    }

    EMSCRIPTEN_KEEPALIVE void lm_destroy (void* p) { delete static_cast<Meter*> (p); }
    EMSCRIPTEN_KEEPALIVE float* lm_left (void* p)  { return static_cast<Meter*> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* lm_right (void* p) { return static_cast<Meter*> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE void lm_reset (void* p)   { static_cast<Meter*> (p)->meter.reset(); }

    // Measures n samples of the meter's buffers: stereo 0 reads only lm_left (mono)
    EMSCRIPTEN_KEEPALIVE void lm_process (void* p, int n, int stereo)
    {
        auto* m = static_cast<Meter*> (p);
        m->meter.process (m->l.data(), stereo != 0 ? m->r.data() : nullptr, n);
    }

    // Gated integrated loudness so far, LUFS (-100 for silence)
    EMSCRIPTEN_KEEPALIVE float lm_integrated (void* p) { return static_cast<Meter*> (p)->meter.integrated(); }

    // Block energies, for a report made after measuring: lm_record (p, 1)
    // before the first lm_process, then lm_blocks counts the 100 ms blocks so
    // far and lm_block_energy (p, channel) points at that many doubles, each
    // block's K-weighted sum of squares (lm_block_length samples per block).
    // Read them before the next lm_process: they move as they grow.
    EMSCRIPTEN_KEEPALIVE void lm_record (void* p, int on) { static_cast<Meter*> (p)->meter.setRecordBlocks (on != 0); }
    EMSCRIPTEN_KEEPALIVE int lm_blocks (void* p) { return (int) static_cast<Meter*> (p)->meter.blockEnergy (0).size(); }
    EMSCRIPTEN_KEEPALIVE int lm_block_length (void* p) { return static_cast<Meter*> (p)->meter.blockLength(); }
    EMSCRIPTEN_KEEPALIVE const double* lm_block_energy (void* p, int channel)
    {
        return static_cast<Meter*> (p)->meter.blockEnergy (channel).data();
    }

    // The tuner's pitch of x[0..n) in Hz, or 0 when there's no clear note.
    // x is memory from malloc.
    EMSCRIPTEN_KEEPALIVE float tp_estimate (const float* x, int n, double sampleRate, float minHz, float maxHz)
    {
        return vs::TunerPitch::estimate (x, n, sampleRate, minHz, maxHz);
    }

    // What hz means on the instrument (vs::TunerReading). strings: nStrings
    // MIDI notes (0 for chromatic); offsets: the temperament's 12 cents on C;
    // out: 4 floats, the note, the string (-1: none), the cents, the target Hz.
    EMSCRIPTEN_KEEPALIVE void tp_reading (float hz, double a4, const int* strings, int nStrings, const float* offsets, int root, float* out)
    {
        std::array<float, 12> o {};
        for (size_t k = 0; k < 12; ++k) o[k] = offsets[k];
        const auto r = vs::TunerReading::of (hz, a4, std::vector<int> (strings, strings + std::max (0, nStrings)), o, root);
        out[0] = (float) r.note; out[1] = (float) r.string; out[2] = r.cents; out[3] = r.targetHz;
    }
}
