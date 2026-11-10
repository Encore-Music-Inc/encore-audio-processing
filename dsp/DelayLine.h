#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Util.h"

namespace vs
{
    // Reading a ring buffer of floats (its size a power of two, mask = size -
    // 1) between its samples. pos is a position in the buffer's own index
    // space: any whole number (negative too) is the sample at pos & mask.

    // Straight-line interpolation between the two samples around pos. The
    // position may be float or double; the fraction is worked out in that
    // type and the interpolation in float.
    template <typename P>
    inline float readLinear (const float* buf, std::int64_t mask, P pos)
    {
        const auto i = (std::int64_t) std::floor (pos);
        const float f = (float) (pos - (P) i);
        const float a = buf[(size_t) (i & mask)], b = buf[(size_t) ((i + 1) & mask)];
        return lerp (a, b, f);
    }

    // 4-point cubic (Hermite) interpolation: smoother than a straight line
    // for a delay that keeps moving.
    inline float readHermite (const float* buf, std::int64_t mask, double pos)
    {
        const int i0 = (int) std::floor (pos);
        const float t = (float) (pos - i0);
        auto at = [buf, mask] (std::int64_t i) { return buf[(size_t) (i & mask)]; };
        const float xm1 = at (i0 - 1), x0 = at (i0), x1 = at (i0 + 1), x2 = at (i0 + 2);
        const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }

    // A delay line: a power-of-two ring of floats and its write position.
    // write() stores a sample and moves on; read (d) is then the sound d
    // samples back (1 = the newest), between samples by straight lines, and
    // readHermite (d) by cubic curves.
    // A sample can also be put() first, read around (it is at delay 0), and
    // advance() moves on after.
    class DelayLine
    {
    public:
        // Room for at least maxDelaySamples (plus the interpolation's reach).
        void prepare (int maxDelaySamples)
        {
            int size = 1;
            while (size < maxDelaySamples + 4) size <<= 1;
            buf.assign ((size_t) size, 0.0f);
            mask = size - 1;
            w = 0;
        }

        // Every sample set to v (silence by default), the write position back to the start.
        void clear (float v = 0.0f) { std::fill (buf.begin(), buf.end(), v); w = 0; }

        void write (float x) { buf[(size_t) w] = x; w = (w + 1) & mask; }
        void put (float x) { buf[(size_t) w] = x; }
        void advance() { w = (w + 1) & mask; }

        // The position is worked out in the delay's own type: a float delay in
        // float, a double one (for long delays, where float would round the
        // fraction) in double.
        template <typename P>
        float read (P delay) const { return readLinear (buf.data(), mask, (P) w - delay); }
        float readHermite (double delay) const { return vs::readHermite (buf.data(), mask, (double) w - delay); }

        // The sample exactly delay samples back (a fixed, whole-sample delay)
        float at (int delay) const { return buf[(size_t) ((w - delay) & mask)]; }

        int size() const { return mask + 1; }

    private:
        std::vector<float> buf;
        int mask = 0, w = 0;
    };
}
