// The browser's loudness meter (lm_*): plain C functions around
// vs::LoudnessMeter, the BS.1770 meter the mastering chain uses. Built with
// the other wasm/api_*.cpp files into master.wasm by wasm/build.sh.
#include <emscripten/emscripten.h>
#include <vector>
#include "dsp/Loudness.h"

namespace
{
    struct Meter
    {
        vs::LoudnessMeter meter;
        std::vector<float> l, r;
    };
}

extern "C"
{
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
}
