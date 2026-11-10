#pragma once

#include <cmath>
#include <cstdint>
#include <vector>
#include "Random.h"
#include "Util.h"

namespace vs
{
    // ENCORE.'s one way to turn float audio into 16, 24 or 32-bit integers.
    // Full scale is 2^(bits - 1), so a 16-bit -32768 becomes -1.0 and back
    // exactly; each sample is scaled, given triangular dither (+-1 LSB, the
    // difference of two uniform numbers from xorshift32) when it needs it,
    // rounded (halves upward) and clipped to the integer range.
    //
    // Dither is only added when the audio is not already on the integer grid
    // (a 16-bit source written back at 16 bits stays bit for bit). It streams:
    // one Quantizer for a whole file carries the random state from block to
    // block, and in automatic mode dither starts at the first block that is
    // off the grid and stays on. Within a block channels are taken one after
    // another, so quantizing a whole file as one block draws the random
    // numbers channel by channel.
    class Quantizer
    {
    public:
        enum class Dither { automatic, always, never };

        static constexpr std::uint32_t defaultSeed = 22222;

        explicit Quantizer (int bitDepth = 16, Dither mode = Dither::automatic, std::uint32_t seed = defaultSeed)
            : bits (bitDepth), dither (mode), rng (seed) {}

        // True when every sample times 2^(bits - 1) is already a whole number in range.
        static bool onGrid (const float* const* ch, int numCh, int n, int bits)
        {
            const double scale = std::ldexp (1.0, bits - 1), max = scale - 1;
            for (int c = 0; c < numCh; ++c)
                for (int i = 0; i < n; ++i)
                {
                    const double v = ch[c][i] * scale;
                    if (v != roundHalfUp (v) || v > max || v < -scale) return false;
                }
            return true;
        }

        // Quantizes n samples of each channel into out (one int32 array per channel).
        void process (const float* const* in, int numCh, int n, std::int32_t* const* out)
        {
            if (dither == Dither::automatic && ! dithering && ! onGrid (in, numCh, n, bits)) dithering = true;
            const bool add = dither == Dither::always || (dither == Dither::automatic && dithering);
            const double scale = std::ldexp (1.0, bits - 1), max = scale - 1, min = -scale;
            for (int c = 0; c < numCh; ++c)
                for (int i = 0; i < n; ++i)
                {
                    double v = in[c][i] * scale;
                    if (add)
                    {
                        const double a = rng.uniform();
                        v += a - rng.uniform();
                    }
                    v = roundHalfUp (v);
                    out[c][i] = (std::int32_t) (v > max ? max : v < min ? min : v);
                }
        }

        // The same, written as interleaved little-endian PCM (bits / 8 bytes a
        // sample) for a WAV file's data chunk; out holds n * numCh * bits / 8 bytes.
        void processToPcm (const float* const* in, int numCh, int n, std::uint8_t* out)
        {
            ints.resize ((size_t) numCh);
            std::vector<std::int32_t*> ptr ((size_t) numCh);
            for (int c = 0; c < numCh; ++c) { ints[(size_t) c].resize ((size_t) n); ptr[(size_t) c] = ints[(size_t) c].data(); }
            process (in, numCh, n, ptr.data());
            const int bps = bits / 8;
            for (int i = 0; i < n; ++i)
                for (int c = 0; c < numCh; ++c)
                {
                    const auto v = (std::uint32_t) ints[(size_t) c][(size_t) i];
                    for (int b = 0; b < bps; ++b) *out++ = (std::uint8_t) ((v >> (8 * b)) & 255u);
                }
        }

        bool isDithering() const { return dither == Dither::always || (dither == Dither::automatic && dithering); }
        int bitDepth() const { return bits; }

    private:
        int bits;
        Dither dither;
        bool dithering = false;
        Xorshift32 rng;
        std::vector<std::vector<std::int32_t>> ints;
    };
}
