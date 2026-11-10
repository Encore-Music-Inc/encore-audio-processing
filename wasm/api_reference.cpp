// The reference set's inputs in the browser build (rf_*): a plain C function
// around vs::reference::render (dsp/ReferenceSignals.h), so wasm/reference.mjs
// and a product's own checks make the same inputs, bit for bit, as the native
// tests. Built with the other wasm/api_*.cpp files into master.wasm by
// wasm/build.sh.
#include <algorithm>
#include <cstdint>
#include <emscripten/emscripten.h>
#include "dsp/ReferenceSignals.h"

extern "C"
{
    // Renders a signal: parts holds size numbers (dsp/ReferenceSignals.h
    // describes them), frames samples per channel at sampleRate (whole hertz)
    // go to l, and to r when channels is 2 (both from malloc).
    EMSCRIPTEN_KEEPALIVE void rf_render (double sampleRate, double frames, int channels, const double* parts, int size, float* l, float* r)
    {
        const auto s = vs::reference::render (sampleRate, (std::int64_t) frames, channels, parts, size);
        std::copy (s.l.begin(), s.l.end(), l);
        if (s.channels > 1 && r != nullptr) std::copy (s.r.begin(), s.r.end(), r);
    }
}
