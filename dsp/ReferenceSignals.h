#pragma once

// The reference set's input signals (data/reference.json, tools/reference.py):
// tones with harmonics, vibrato and envelopes, seeded noise and short runs of
// given sample values, mixed into one or two channels. Everything is made with
// integer arithmetic (a 32-bit phase, a fixed-point sine, shifts for the
// envelopes and filters), so every system and the browser build make the
// same samples, bit for bit, whatever its compiler, its maths library or its
// floating-point settings. hash() names a signal's samples, so a product can
// check it made the same input before it compares results.
//
// A signal is described by parts, each a record of `fields` numbers (and, for
// samples, its values after it):
//
//   0 type      0 tone, 1 noise, 2 samples, 3 clip (after every other part: the
//               mix held to within l of full scale either way, a flat-topped peak)
//   1 from      start, seconds
//   2 to        end, seconds (negative: the end of the signal)
//   3 l, 4 r    level in each channel (1 = full scale)
//   5 hz        tone: frequency (to the millihertz)
//   6 mult      tone: a whole-number multiple of hz (a harmonic, its phase locked to the first)
//   7 phase     tone: starting phase, turns
//   8 vibHz     tone: vibrato rate
//   9 vibDepth  tone: vibrato depth, a fraction of the frequency
//   10 attack   seconds of linear fade-in at each start
//   11 decay    k: the level falls by 1/2^k of itself each sample (0: held); a time constant of 2^k samples
//   12 every    seconds between restarts (0: once): each restart begins the envelope and the tone's phase again
//   13 seed     noise: the Xorshift32 seed
//   14 lowpass  noise: k of a one-pole low-pass (y += (x - y) / 2^k; 0: none)
//   15 highpass noise: k of a one-pole low-pass taken away (0: none)
//   16 count    samples: how many values follow (each a sample value, times l or r)
//
// Plain C++20 with no JUCE.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "Random.h"

namespace vs::reference
{
    inline constexpr int fields = 17;
    enum PartType { tone = 0, noise = 1, samples = 2, clip = 3 };

    // sin (2 pi phase / 2^32) in Q30, by a degree-13 odd polynomial on a quarter turn (error under 1e-7)
    inline std::int64_t sineQ30 (std::uint32_t phase)
    {
        std::int64_t t = (std::int32_t) phase;                      // turns in Q32, -0.5 .. 0.5
        constexpr std::int64_t quarter = 1LL << 30, half = 1LL << 31;
        if (t > quarter) t = half - t;
        else if (t < -quarter) t = -half - t;
        const std::int64_t u = (t * t) >> 32;                       // t^2, Q32
        // (2 pi)^n / n!, Q26
        constexpr std::int64_t c[] { 421657428LL, 2774394673LL, 5476435575LL, 5147643110LL, 2822511172LL, 1012984316LL, 256352679LL };
        std::int64_t p = c[6];
        for (int k = 5; k >= 0; --k) p = c[k] - ((p * u) >> 32);
        return ((p * t) >> 32) * 16;
    }

    inline std::int64_t fixed (double x, int bits) { return std::llround (x * (double) (1LL << bits)); }

    // A 32-bit phase step for hz at rate fs (whole hertz)
    inline std::uint32_t phaseStep (double hz, std::int64_t fs)
    {
        const std::int64_t mhz = std::llround (hz * 1000.0);
        const std::uint64_t a = (std::uint64_t) (mhz < 0 ? -mhz : mhz);
        return (std::uint32_t) ((a << 32) / ((std::uint64_t) fs * 1000u));
    }

    struct Signal
    {
        int channels = 1;
        std::vector<float> l, r; // r is empty for one channel
    };

    // Renders parts (as above, `size` numbers in all) into frames samples at sampleRate (whole hertz).
    inline Signal render (double sampleRate, std::int64_t frames, int channels, const double* parts, int size)
    {
        const std::int64_t fs = std::llround (sampleRate);
        channels = channels > 1 ? 2 : 1;
        std::vector<std::int64_t> acc[2];
        for (int c = 0; c < channels; ++c) acc[c].assign ((size_t) std::max<std::int64_t> (0, frames), 0);
        auto seconds = [&] (double s) { return (std::int64_t) std::llround (s * (double) fs); };
        std::int64_t clipAt = -1;

        for (int at = 0; at + fields <= size;)
        {
            const double* p = parts + at;
            const int type = (int) p[0];
            const int count = type == samples ? (int) p[16] : 0;
            at += fields + count;
            if (at > size) break;
            if (type == clip) { clipAt = fixed (p[3], 30); continue; }
            const std::int64_t from = std::max<std::int64_t> (0, seconds (p[1]));
            const std::int64_t to = p[2] < 0 ? frames : std::min (frames, seconds (p[2]));
            const std::int64_t gain[2] { fixed (p[3], 24), fixed (p[4], 24) };
            const std::uint32_t step = phaseStep (p[5], fs) * (std::uint32_t) std::max (1.0, p[6]);
            const std::uint32_t start = (std::uint32_t) (std::int64_t) fixed (p[7] - std::floor (p[7]), 32);
            const std::uint32_t vibStep = phaseStep (p[8], fs);
            const std::int64_t vibDepth = fixed (p[9], 16);
            const std::int64_t attack = seconds (p[10]);
            const int decay = (int) p[11];
            const std::int64_t every = fixed (p[12] * (double) fs, 32); // frames, Q32
            Xorshift32 random ((std::uint32_t) p[13]);
            const int lowpass = (int) p[14], highpass = (int) p[15];
            std::vector<std::int64_t> values;
            for (int i = 0; i < count; ++i) values.push_back (fixed (p[fields + i], 30));

            std::uint32_t phase = start, vib = 0;
            std::int64_t env = 0, lp = 0, hp = 0, hit = 0, sinceHit = 0;
            std::int64_t nextHit = from;
            for (std::int64_t f = from; f < to; ++f)
            {
                if (f == nextHit)
                {
                    phase = start;
                    sinceHit = 0;
                    env = attack > 0 ? 0 : (1LL << 30);
                    ++hit;
                    nextHit = every > 0 ? from + ((hit * every) >> 32) : -1;
                }
                // the envelope, Q30
                if (sinceHit < attack) env = ((1LL << 30) * sinceHit) / attack;
                else if (sinceHit == attack && attack > 0) env = 1LL << 30;
                std::int64_t x = 0;
                if (type == tone)
                {
                    x = (sineQ30 (phase) * env) >> 30;
                    std::uint32_t s = step;
                    if (vibDepth != 0)
                    {
                        const std::int64_t d = ((std::int64_t) step * vibDepth) >> 16;
                        s = (std::uint32_t) ((std::int64_t) step + ((d * sineQ30 (vib)) >> 30));
                    }
                    phase += s;
                    vib += vibStep;
                }
                else if (type == noise)
                {
                    const std::int64_t w = (std::int64_t) (std::int32_t) random.next() >> 1; // Q30, -1 .. 1
                    std::int64_t y = w;
                    if (lowpass > 0) { lp += (w - lp) >> lowpass; y = lp; }
                    if (highpass > 0) { hp += (y - hp) >> highpass; y -= hp; }
                    x = (y * env) >> 30;
                }
                else if (type == samples)
                {
                    x = sinceHit < (std::int64_t) values.size() ? (values[(size_t) sinceHit] * env) >> 30 : 0;
                }
                if (sinceHit >= attack && decay > 0) env -= env >> decay;
                ++sinceHit;
                for (int c = 0; c < channels; ++c) acc[c][(size_t) f] += (x * gain[c]) >> 24;
            }
        }

        if (clipAt >= 0)
            for (int c = 0; c < channels; ++c)
                for (auto& v : acc[c]) v = std::clamp (v, -clipAt, clipAt);

        Signal out;
        out.channels = channels;
        auto toFloat = [] (const std::vector<std::int64_t>& a)
        {
            std::vector<float> x (a.size());
            for (size_t i = 0; i < a.size(); ++i) x[i] = (float) ((double) a[i] / 1073741824.0);
            return x;
        };
        out.l = toFloat (acc[0]);
        if (channels > 1) out.r = toFloat (acc[1]);
        return out;
    }

    // FNV-1a (32-bit) over the samples' bit patterns, little-endian, first channel then second
    inline std::uint32_t hash (const float* l, const float* r, std::int64_t n)
    {
        std::uint32_t h = 2166136261u;
        for (const float* x : { l, r })
        {
            if (x == nullptr) continue;
            for (std::int64_t i = 0; i < n; ++i)
            {
                std::uint32_t b;
                static_assert (sizeof (b) == sizeof (float));
                std::memcpy (&b, x + i, 4);
                for (int k = 0; k < 4; ++k) { h ^= (b >> (8 * k)) & 0xffu; h *= 16777619u; }
            }
        }
        return h;
    }
}
