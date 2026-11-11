// The browser's tuner (tp_*): plain C functions around vs::TunerPitch,
// vs::TunerReading and the block averaging before a reading
// (vs::boxDownsample). Built with the other wasm/api_*.cpp files into
// master.wasm by wasm/build.sh.
#include <algorithm>
#include <array>
#include <vector>
#include <emscripten/emscripten.h>
#include "dsp/TunerPitch.h"

extern "C"
{
    // The tuner's pitch of x[0..n) in Hz, or 0 when there's no clear note.
    // x is memory from malloc.
    EMSCRIPTEN_KEEPALIVE float tp_estimate (const float* x, int n, double sampleRate, float minHz, float maxHz)
    {
        return vs::TunerPitch::estimate (x, n, sampleRate, minHz, maxHz);
    }

    // x[0..n) averaged down by stride (each stride samples become their
    // mean, a remainder at the end dropped) into out (n / stride floats, from
    // malloc); stride 1 or less copies x. Returns how many it wrote.
    EMSCRIPTEN_KEEPALIVE int tp_downsample (const float* x, int n, int stride, float* out)
    {
        const auto y = vs::boxDownsample (x, n, stride);
        std::copy (y.begin(), y.end(), out);
        return (int) y.size();
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
