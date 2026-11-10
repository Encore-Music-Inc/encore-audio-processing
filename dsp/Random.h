#pragma once

#include <cstdint>

namespace vs
{
    // Seeded random number generators. Each is named by its algorithm and
    // gives the same sequence for the same seed on every system, so a sound
    // made from noise (drum hits, dither, clicks, test signals) comes out the
    // same every time. uniform() is in [0, 1).

    // xorshift32 (Marsaglia: shifts 13, 17, 5). A seed of 0 becomes 1 (0 would stay 0).
    struct Xorshift32
    {
        std::uint32_t s;

        explicit Xorshift32 (std::uint32_t seed = 0x9e3779b9u) : s (seed != 0 ? seed : 1u) {}

        std::uint32_t next()
        {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return s;
        }

        double uniform() { return next() / 4294967296.0; }
    };

    // mulberry32 (Tommy Ettinger): a 32-bit counter run through a mixing function.
    struct Mulberry32
    {
        std::uint32_t a;

        explicit Mulberry32 (std::uint32_t seed = 1u) : a (seed) {}

        std::uint32_t next()
        {
            a += 0x6d2b79f5u;
            std::uint32_t t = a;
            t = (t ^ (t >> 15)) * (t | 1u);
            t ^= t + (t ^ (t >> 7)) * (t | 61u);
            return t ^ (t >> 14);
        }

        double uniform() { return next() / 4294967296.0; }
    };

    // The Park-Miller "minimal standard" generator: s = s * 16807 mod (2^31 - 1).
    // The seed must be 1 .. 2^31 - 2. uniform() is s / (2^31 - 1), in (0, 1).
    struct ParkMiller
    {
        std::uint32_t s;

        explicit ParkMiller (std::uint32_t seed) : s (seed) {}

        std::uint32_t next()
        {
            s = (std::uint32_t) (((std::uint64_t) s * 16807u) % 2147483647u);
            return s;
        }

        double uniform() { return next() / 2147483647.0; }
    };

    // The 32-bit linear congruential generator s = s * 1664525 + 1013904223
    // (mod 2^32), from Numerical Recipes. Any seed, 0 included.
    struct Lcg32
    {
        std::uint32_t s;

        explicit Lcg32 (std::uint32_t seed) : s (seed) {}

        std::uint32_t next()
        {
            s = s * 1664525u + 1013904223u;
            return s;
        }

        double uniform() { return next() / 4294967296.0; }

        // uniform() mapped to [-1, 1)
        double bipolar() { return uniform() * 2 - 1; }
    };
}
