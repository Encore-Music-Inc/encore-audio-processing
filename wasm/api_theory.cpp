// The browser's chord and scale theory (th_*): one plain C function around
// dsp/TheoryCalls.h, so the web names chords, maps keys, voices chords and
// fits scales with the same code as the desktop products. Built with the
// other wasm/api_*.cpp files into master.wasm by wasm/build.sh. The chord
// types and the scale library are data files (data/chords.json,
// data/scales.json), not functions.
#include <cstring>
#include <string>
#include <emscripten/emscripten.h>
#include "dsp/TheoryCalls.h"

extern "C"
{
    // Answers one request (dsp/TheoryCalls.h lists them): request is JSON text ending
    // with a 0 byte; the answer, JSON text ending with a 0 byte, goes to out (memory
    // from malloc, cap bytes). Returns the answer's length, or minus the room it
    // needs (its length plus one) when cap is too small, so the call can be repeated.
    EMSCRIPTEN_KEEPALIVE int th_call (const char* request, char* out, int cap)
    {
        static std::string answer;
        static std::string asked;
        // a repeated request (the second try with more room) isn't worked out again
        if (asked.empty() || asked != request) { asked = request; answer = vs::theorycalls::call (asked); }
        const int need = (int) answer.size() + 1;
        if (need > cap) { if (cap > 0) out[0] = 0; return -need; }
        std::memcpy (out, answer.c_str(), (size_t) need);
        asked.clear();
        return need - 1;
    }
}
