#pragma once

// The drum synthesizer: every built-in drum sound, made from oscillators,
// noise and filters (no sample files), and the kits that use them. Every
// sound is at full level within its first millisecond (the attack is the
// drum), then peaks at its kit level. Plain C++20, no JUCE.
//
// The kits: ENCORE. (a punchy all-round kit), Boom (the classic analog drum
// machine), Club (the classic house and techno machine), and Studio, 808,
// Rock and Lo-fi, one recipe with different settings (Style), where Lo-fi
// adds a bit crusher, sample-and-hold and a low-pass after the drums.
//
// Every value is computed the same way, operation for operation, in double
// precision with float buffers, so every platform renders the same samples.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>
#include "Biquad.h"
#include "Music.h"
#include "Random.h"
#include "Channels.h"

namespace vs::drumsynth
{
    using Buffer = std::vector<float>;
    using Type = BiquadCoefs::Type;

    inline constexpr double pi = 3.141592653589793;

    // The running peak of |v|; a NaN sample makes it NaN (a NaN peak then
    // counts as no peak), as a browser's Math.max does
    using channels::runningPeak;

    // ---- the building blocks

    // A biquad (RBJ cookbook) over the whole buffer, in place, its frequency
    // kept at or under 0.45 of the sample rate. gainDb is for peak.
    inline Buffer& biquad (Buffer& x, double sr, Type type, double hz, double q, double gainDb = 0)
    {
        filterRbjCapped (x.data(), (int) x.size(), sr, type, hz, q, gainDb);
        return x;
    }

    // A square wave that doesn't alias (polyBLEP), cheap enough to stack six of for cymbal metal
    inline Buffer squarePolyBlep (double sr, int n, double hz, double phase0 = 0)
    {
        Buffer x ((size_t) n);
        const double dt = hz / sr;
        auto blep = [dt] (double t)
        {
            if (t < dt) { t /= dt; return t + t - t * t - 1; }
            if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
            return 0.0;
        };
        double t = phase0;
        for (int i = 0; i < n; ++i)
        {
            double v = t < 0.5 ? 1 : -1;
            v += blep (t);
            v -= blep (std::fmod (t + 0.5, 1.0));
            x[(size_t) i] = (float) v;
            t += dt;
            if (t >= 1) t -= 1;
        }
        return x;
    }

    // The six clashing square waves of a drum machine's hats and cymbals, `scale` times the classic pitches
    inline Buffer metal (double sr, int n, double scale = 1)
    {
        Buffer x ((size_t) n, 0.0f);
        const double hz[] { 205.3, 304.4, 369.6, 522.7, 540, 800 };
        for (int k = 0; k < 6; ++k)
        {
            const auto s = squarePolyBlep (sr, n, hz[k] * scale, k * 0.17);
            for (size_t i = 0; i < (size_t) n; ++i) x[i] = (float) (x[i] + (double) s[i] / 6);
        }
        return x;
    }

    // White noise in [-1, 1), the same every time for a seed
    inline Buffer noise (int n, std::uint32_t seed)
    {
        Xorshift32 r (seed);
        Buffer x ((size_t) n);
        for (auto& v : x) v = (float) (r.uniform() * 2 - 1);
        return x;
    }

    // Soft saturation: makes a drum dense and loud without hard clipping
    inline Buffer& drive (Buffer& x, double k)
    {
        const double d = std::tanh (k);
        for (auto& v : x) v = (float) (std::tanh (k * v) / d);
        return x;
    }

    // Starts a sound where it first gets loud (filters ease into a hit)
    inline Buffer onTime (const Buffer& x) { return channels::fromOnset (x); }

    // Peak to `level`, with a short fade at the very end so nothing clicks off
    inline Buffer finish (const Buffer& in, double level)
    {
        Buffer x = onTime (in);
        channels::toPeakWithEndFade (x, level, 512);
        return x;
    }

    struct BodyOptions { double hold = 0, click = 0.25, clickHz = 3500, second = 0; std::uint32_t seed = 5; };

    // A pitched drum: a sine (starting at its peak, so the hit is instant)
    // that drops from hiHz to loHz, plus a click. tau: how long the body
    // rings; drop: how fast the pitch falls; second: a partial at 1.58 times
    // the pitch, ringing half as long
    inline Buffer body (double sr, double seconds, double hiHz, double loHz, double tau, double drop, BodyOptions o = {})
    {
        const int n = (int) std::floor (sr * seconds);
        Buffer x ((size_t) n);
        double ph = 0, ph2 = 0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr, f = loHz + (hiHz - loHz) * std::exp (-t / drop);
            const double env = t < o.hold ? 1 : std::exp (-(t - o.hold) / tau);
            x[(size_t) i] = (float) (env * (std::cos (ph) + o.second * std::cos (ph2) * std::exp (-t / (tau * 0.5))));
            ph += 2 * pi * f / sr;
            ph2 += 2 * pi * f * 1.58 / sr;
        }
        if (o.click != 0)
        {
            auto c = noise ((int) std::floor (sr * 0.006), o.seed);
            biquad (c, sr, Type::bandPass, o.clickHz, 0.8);
            for (int i = 0; i < (int) std::min (c.size(), x.size()); ++i)
                x[(size_t) i] = (float) (x[(size_t) i] + o.click * 4 * c[(size_t) i] * std::exp (-i / sr / 0.0012));
        }
        return x;
    }

    struct Filter { Type type; double hz, q, gainDb = 0; };

    // Filtered noise with an envelope (env: seconds to level)
    inline Buffer hiss (double sr, double seconds, const std::function<double (double)>& env, std::uint32_t seed, std::initializer_list<Filter> filters)
    {
        const int n = (int) std::floor (sr * seconds);
        auto x = noise (n, seed);
        for (const auto& f : filters) biquad (x, sr, f.type, f.hz, f.q, f.gainDb);
        for (int i = 0; i < n; ++i) x[(size_t) i] = (float) (x[(size_t) i] * env (i / sr));
        return x;
    }

    struct Part { const Buffer* x; double gain; };

    // Sounds added together, each at a gain
    inline Buffer mix (std::initializer_list<Part> parts)
    {
        size_t n = 0;
        for (const auto& p : parts) n = std::max (n, p.x->size());
        Buffer x (n, 0.0f);
        for (const auto& p : parts)
            for (size_t i = 0; i < p.x->size(); ++i) x[i] = (float) (x[i] + (*p.x)[i] * p.gain);
        return x;
    }

    // A clap: several hands a few milliseconds apart (gap), then the room (room seconds)
    inline Buffer clap (double sr, double gap, double room, double hz, std::uint32_t seed)
    {
        const double hits[] { 0, gap, gap * 2, gap * 2.9 }, last = hits[3];
        auto env = [=] (double t)
        {
            double v = 0;
            for (double h : hits) if (t >= h) v = std::max (v, std::exp (-(t - h) / 0.0045));
            return t >= last ? std::max (v, 0.6 * std::exp (-(t - last) / room)) : v;
        };
        auto x = hiss (sr, room * 3.5, env, seed, { { Type::bandPass, hz, 1.1 }, { Type::highPass, 500, 0.7 }, { Type::peak, hz * 2.2, 1, 3 } });
        return drive (x, 1.6);
    }

    // A partial of a struck sound: Hz, level, decay seconds
    struct Partial { double hz, level, tau; };

    // Struck wood or metal: decaying sines that start at their peak, plus a noise tick
    inline Buffer modes (double sr, double seconds, std::initializer_list<Partial> partials, double tick, std::uint32_t seed)
    {
        const int n = (int) std::floor (sr * seconds);
        Buffer x ((size_t) n, 0.0f);
        for (const auto& p : partials)
        {
            const double w = 2 * pi * p.hz / sr;
            for (int i = 0; i < n; ++i) x[(size_t) i] = (float) (x[(size_t) i] + p.level * std::cos (w * i) * std::exp (-i / sr / p.tau));
        }
        const auto c = hiss (sr, 0.01, [] (double t) { return std::exp (-t / 0.0012); }, seed, { { Type::highPass, 2500, 0.7 } });
        for (size_t i = 0; i < std::min (c.size(), x.size()); ++i) x[i] = (float) (x[i] + tick * c[i]);
        return x;
    }

    struct SnareOptions { double hz, tau, wires, wireHp, bright; std::uint32_t seed; double drv = 1.8, head = 0.8, wireMix = 0.9; };

    // A snare: the head (two modes) and the wires (bright noise), both straight in at full level
    inline Buffer snare (double sr, const SnareOptions& o)
    {
        const auto h = body (sr, o.wires * 3, o.hz * 1.4, o.hz, o.tau, 0.012, { 0, 0.2, 5000, 0.55, o.seed });
        const double wires = o.wires;
        const auto w = hiss (sr, o.wires * 3, [wires] (double t) { return std::exp (-t / wires) * (0.75 + 0.25 * std::exp (-t / 0.01)); }, o.seed + 8,
                             { { Type::highPass, o.wireHp, 0.7 }, { Type::peak, o.bright, 1, 4 }, { Type::lowPass, 13000, 0.7 } });
        auto x = mix ({ { &h, o.head }, { &w, o.wireMix } });
        return drive (x, o.drv);
    }

    struct CymbalOptions
    {
        double scale, hp, peakHz, splash, wash, metalMix = 0.55, noiseMix = 0.6;
        std::vector<Partial> bell {};
        std::uint32_t seed = 1;
        double lowPass = 14500;             // where the top is eased off (0: not at all)
    };

    // A cymbal: metal and noise high-passed, a splash up front and a wash
    // after; bell: sines for a ride's ping
    inline Buffer cymbal (double sr, double seconds, const CymbalOptions& o)
    {
        const int n = (int) std::floor (sr * seconds);
        const auto m = metal (sr, n, o.scale), z = noise (n, o.seed);
        Buffer x ((size_t) n);
        for (size_t i = 0; i < (size_t) n; ++i) x[i] = (float) (o.metalMix * m[i] + o.noiseMix * z[i]);
        biquad (x, sr, Type::highPass, o.hp, 0.7);
        biquad (x, sr, Type::peak, o.peakHz, 0.8, 4);
        if (o.lowPass > 0) biquad (x, sr, Type::lowPass, o.lowPass, 0.6);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            x[(size_t) i] = (float) (x[(size_t) i] * (0.5 * std::exp (-t / o.splash) + 0.5 * std::exp (-t / o.wash)));
        }
        for (const auto& b : o.bell)
        {
            const double w = 2 * pi * b.hz / sr;
            for (int i = 0; i < n; ++i) x[(size_t) i] = (float) (x[(size_t) i] + b.level * std::cos (w * i) * std::exp (-i / sr / b.tau));
        }
        return x;
    }

    // A hi-hat: metal and noise, band-passed and high-passed hard
    inline Buffer hat (double sr, double seconds, const std::function<double (double)>& env, std::uint32_t seed, double scale = 1, double metalMix = 0.7)
    {
        const int n = (int) std::floor (sr * seconds);
        const auto m = metal (sr, n, scale), z = noise (n, seed);
        Buffer x ((size_t) n);
        for (size_t i = 0; i < (size_t) n; ++i) x[i] = (float) (metalMix * m[i] + 0.45 * z[i]);
        biquad (x, sr, Type::bandPass, 10000, 0.8);
        biquad (x, sr, Type::highPass, 7000, 0.7);
        biquad (x, sr, Type::highPass, 7000, 0.7);
        biquad (x, sr, Type::lowPass, 15000, 0.6);
        for (int i = 0; i < n; ++i) x[(size_t) i] = (float) (x[(size_t) i] * env (i / sr));
        return x;
    }

    // A tom: a body falling from 1.5 times its pitch, with a second partial, driven
    inline Buffer tom (double sr, double hz, double tau, std::uint32_t seed)
    {
        auto x = body (sr, tau * 2.6, hz * 1.5, hz, tau, 0.03, { 0, 0.22, 3000, 0.3, seed });
        return drive (x, 1.9);
    }

    // ---- the Style kits: one recipe, different settings

    struct Style
    {
        const char* id;
        const char* name;
        double kickLo, kickHi, kickBend, kickDecay, click;                // the kick's pitch drop, length and beater click
        double snareTone1, snareTone2, snareToneDecay, snareNoiseHp, snareNoiseDecay, snareNoise;
        double hatDecay, cymbalBright;                                    // multipliers on the Studio kit's
        double tomPitch, tomDecay;
        double drive;                                                     // 0 clean .. 1 saturated
        int crushBits, crushHold; double lowpass;                         // the lo-fi stages (0 = off)
    };

    inline constexpr std::array<Style, 4> styles { {
        { "studio", "Studio", 48, 150, 0.035, 0.32, 0.5,  185, 330, 0.06, 1200, 0.12, 0.8,  1.0, 1.0,  1.0,  0.28, 0.0,  0, 1, 0 },
        { "808",    "808",    45, 120, 0.060, 0.90, 0.15, 230, 476, 0.09, 2500, 0.08, 0.55, 0.7, 1.2,  0.85, 0.50, 0.3,  0, 1, 0 },
        { "rock",   "Rock",   55, 180, 0.020, 0.22, 0.9,  170, 290, 0.10,  800, 0.20, 1.0,  1.3, 0.75, 0.9,  0.40, 0.5,  0, 1, 0 },
        { "lofi",   "Lo-fi",  50, 140, 0.035, 0.30, 0.4,  190, 320, 0.07, 1500, 0.10, 0.7,  0.9, 0.8,  1.0,  0.25, 0.25, 8, 3, 6500 },
    } };

    // The kit's colour after each drum: saturation, then the lo-fi crunch
    // (fewer bits held over several samples, then darker)
    inline Buffer& styleStages (Buffer& x, double sr, const Style& st)
    {
        if (st.drive > 0) drive (x, 1 + st.drive * 4);
        if (st.crushBits > 0)
        {
            const float levels = (float) (1 << (st.crushBits - 1));
            float held = 0;
            for (size_t i = 0; i < x.size(); ++i)
            {
                if (i % (size_t) std::max (1, st.crushHold) == 0) held = std::round (x[i] * levels) / levels;
                x[i] = held;
            }
        }
        if (st.lowpass > 0) biquad (x, sr, Type::lowPass, st.lowpass, 0.7071067811865476);
        return x;
    }

    // One drum of a Style kit, by its General MIDI note (36 kick, 38 snare,
    // 39 clap, 42 and 46 hats, 41, 45 and 48 toms, 49 crash, 51 ride); the
    // seeds differ by kit and drum
    inline Buffer styleDrum (const Style& st, int note, double sr, std::uint32_t seedBase)
    {
        const std::uint32_t seed = seedBase + (std::uint32_t) note;
        Buffer x;
        switch (note)
        {
            case 36:
                x = body (sr, std::max (0.6, st.kickDecay * 3), st.kickHi, st.kickLo, st.kickDecay, st.kickBend, { 0, st.click * 0.8, 6000, 0, seed });
                break;
            case 38:
            {
                const auto tones = modes (sr, 0.4, { { st.snareTone1, 0.6, st.snareToneDecay }, { st.snareTone2, 0.3, st.snareToneDecay } }, 0, seed);
                const double decay = st.snareNoiseDecay;
                const auto wires = hiss (sr, 0.4, [decay] (double t) { return std::exp (-t / decay); }, seed + 100, { { Type::highPass, st.snareNoiseHp, 0.7071067811865476 } });
                x = mix ({ { &tones, 1 }, { &wires, st.snareNoise } });
                break;
            }
            case 39:
                x = clap (sr, 0.009, 0.13, 1000, seed);
                break;
            case 42: case 46: case 49: case 51:
            {
                const bool isHat = note == 42 || note == 46;
                const double decay = (note == 42 ? 0.035 : note == 46 ? 0.25 : note == 49 ? 0.75 : 0.5) * (isHat ? st.hatDecay : 1.0);
                const double seconds = note == 42 ? 0.18 * st.hatDecay : note == 46 ? 0.7 * st.hatDecay : note == 49 ? 2.2 : 1.6;
                const double hp = (note == 51 ? 5000.0 : 7000.0) * st.cymbalBright;
                CymbalOptions o { 1.7, hp, hp * 2.2, decay, decay, 1.0, note == 49 ? 0.7 : note == 51 ? 0.25 : 0.5, {}, seed, 0 };
                if (note == 51) o.bell = { { 2600, 0.15, 0.4 } };
                x = cymbal (sr, seconds, o);
                break;
            }
            default:
            {
                const double f0 = (note == 41 ? 92.0 : note == 45 ? 124.0 : 165.0) * st.tomPitch;
                x = body (sr, std::max (0.7, st.tomDecay * 2.5), f0 * 1.6, f0, st.tomDecay, 0.05, { 0, 0.1, 3000, 0, seed });
            }
        }
        return styleStages (x, sr, st);
    }

    // ---- every sound, by id

    struct SoundDef
    {
        const char* id;
        double level;                       // its peak, so a kit is balanced before anyone touches a slider
        Buffer (*make) (double sr);
    };

    template <int styleIndex, int note>
    Buffer makeStyled (double sr) { return styleDrum (styles[(size_t) styleIndex], note, sr, 1000u * (std::uint32_t) (styleIndex + 1)); }

    // The level every Style kit drum peaks at (-1 dB)
    inline constexpr double styleLevel = 0.89;

    inline const std::vector<SoundDef>& sounds()
    {
        static const std::vector<SoundDef> s {
            // ENCORE.: a tight, round kick (165 Hz falling fast to 48, a short hold so it thumps, a beater click on top)
            { "kick", 0.98, [] (double sr) { auto x = body (sr, 0.6, 165, 48, 0.2, 0.032, { 0.018, 0.35, 4000, 0, 5 }); biquad (x, sr, Type::peak, 60, 0.8, 3); return drive (x, 2.4); } },
            { "snare", 0.92, [] (double sr) { return snare (sr, { 185, 0.055, 0.13, 1400, 5000, 9 }); } },
            { "closedHat", 0.55, [] (double sr) { return hat (sr, 0.16, [] (double t) { return std::exp (-t / 0.028); }, 31); } },
            { "openHat", 0.55, [] (double sr) { return hat (sr, 0.95, [] (double t) { return 0.25 * std::exp (-t / 0.012) + 0.75 * std::exp (-t / 0.3); }, 32); } },
            { "clap", 0.85, [] (double sr) { return clap (sr, 0.0085, 0.11, 1150, 41); } },
            // a rimshot: the stick on the hoop (a sharp wooden knock) and the head under it
            { "rim", 0.75, [] (double sr) { auto x = modes (sr, 0.14, { { 1720, 1, 0.014 }, { 510, 0.7, 0.02 }, { 3950, 0.35, 0.006 }, { 880, 0.3, 0.012 } }, 1.2, 51); return drive (x, 1.5); } },
            { "highTom", 0.9, [] (double sr) { return tom (sr, 205, 0.2, 61); } },
            { "lowTom", 0.92, [] (double sr) { return tom (sr, 120, 0.3, 62); } },
            { "crash", 0.6, [] (double sr) { return cymbal (sr, 2.4, { 1.42, 3200, 6500, 0.04, 0.75, 0.55, 0.6, {}, 71 }); } },

            // Boom, the classic analog machine: a nearly pure sine kick that falls slowly and rings a long time
            { "boomKick", 0.98, [] (double sr) { auto x = body (sr, 1.5, 125, 47, 0.5, 0.06, { 0.025, 0.18, 2500, 0, 101 }); return drive (x, 1.5); } },
            { "boomSnare", 0.92, [] (double sr) { return snare (sr, { 238, 0.04, 0.075, 1900, 6000, 103, 1.5, 0.9, 1 }); } },
            { "boomClosedHat", 0.55, [] (double sr) { return hat (sr, 0.12, [] (double t) { return std::exp (-t / 0.02); }, 105); } },
            { "boomOpenHat", 0.55, [] (double sr) { return hat (sr, 0.8, [] (double t) { return 0.2 * std::exp (-t / 0.01) + 0.8 * std::exp (-t / 0.24); }, 106); } },
            { "boomClap", 0.85, [] (double sr) { return clap (sr, 0.011, 0.17, 1000, 107); } },
            { "boomRim", 0.75, [] (double sr) { return modes (sr, 0.06, { { 1667, 1, 0.006 }, { 455, 0.8, 0.007 } }, 0.6, 109); } },
            { "boomHighTom", 0.9, [] (double sr) { auto x = body (sr, 0.7, 230, 185, 0.22, 0.05, { 0, 0.06, 2500, 0, 111 }); return drive (x, 1.3); } },
            { "boomLowTom", 0.92, [] (double sr) { auto x = body (sr, 0.9, 140, 108, 0.3, 0.05, { 0, 0.06, 2500, 0, 112 }); return drive (x, 1.3); } },
            // the drum machine cowbell: two square waves a not-quite-fifth apart, band-passed, a sharp drop then a ring
            { "boomCowbell", 0.6, [] (double sr)
              {
                  const int n = (int) std::floor (sr * 0.45);
                  const auto a = squarePolyBlep (sr, n, 540), b = squarePolyBlep (sr, n, 800);
                  Buffer x ((size_t) n);
                  for (size_t i = 0; i < (size_t) n; ++i) x[i] = (float) ((double) a[i] + (double) b[i]);
                  biquad (x, sr, Type::bandPass, 2000, 0.9);
                  biquad (x, sr, Type::highPass, 400, 0.7);
                  for (int i = 0; i < n; ++i)
                  {
                      const double t = i / sr;
                      x[(size_t) i] = (float) (x[(size_t) i] * (0.7 * std::exp (-t / 0.012) + 0.3 * std::exp (-t / 0.13)));
                  }
                  return x;
              } },

            // Club, the house and techno machine: a short kick that starts high and punches, with a hard beater click
            { "clubKick", 0.98, [] (double sr) { auto x = body (sr, 0.5, 260, 52, 0.15, 0.018, { 0.012, 0.55, 5000, 0, 121 }); biquad (x, sr, Type::peak, 70, 0.9, 2); return drive (x, 3); } },
            { "clubSnare", 0.92, [] (double sr) { return snare (sr, { 210, 0.05, 0.17, 2400, 7500, 123, 2.2, 0.7, 1.1 }); } },
            { "clubClosedHat", 0.55, [] (double sr) { return hat (sr, 0.12, [] (double t) { return std::exp (-t / 0.022); }, 125, 1.3, 0.3); } },
            { "clubOpenHat", 0.55, [] (double sr) { return hat (sr, 0.7, [] (double t) { return 0.25 * std::exp (-t / 0.01) + 0.75 * std::exp (-t / 0.2); }, 126, 1.3, 0.3); } },
            { "clubClap", 0.85, [] (double sr) { return clap (sr, 0.0075, 0.14, 1400, 127); } },
            { "clubRim", 0.75, [] (double sr) { return modes (sr, 0.08, { { 1950, 1, 0.008 }, { 620, 0.6, 0.01 }, { 4400, 0.3, 0.004 } }, 0.9, 129); } },
            { "clubTom", 0.9, [] (double sr) { auto x = body (sr, 0.6, 290, 160, 0.17, 0.02, { 0, 0.3, 4000, 0, 131 }); return drive (x, 2); } },
            { "clubRide", 0.5, [] (double sr) { return cymbal (sr, 2.2, { 2.1, 4200, 7000, 0.02, 0.7, 0.8, 0.3, { { 3150, 0.12, 0.5 }, { 4730, 0.08, 0.4 } }, 133 }); } },
            { "clubCrash", 0.6, [] (double sr) { return cymbal (sr, 2.2, { 1.6, 4000, 8000, 0.03, 0.6, 0.55, 0.6, {}, 135 }); } },

            { "shaker", 0.5, [] (double sr) { return hiss (sr, 0.12, [] (double t) { return std::exp (-t / 0.03); }, 91, { { Type::highPass, 5500, 0.7 }, { Type::peak, 9000, 1, 3 } }); } },

#define VS_STYLE_KIT(i, prefix) \
            { prefix "Kick", styleLevel, makeStyled<i, 36> }, { prefix "Snare", styleLevel, makeStyled<i, 38> }, \
            { prefix "Clap", styleLevel, makeStyled<i, 39> }, { prefix "ClosedHat", styleLevel, makeStyled<i, 42> }, \
            { prefix "OpenHat", styleLevel, makeStyled<i, 46> }, { prefix "FloorTom", styleLevel, makeStyled<i, 41> }, \
            { prefix "LowTom", styleLevel, makeStyled<i, 45> }, { prefix "HighTom", styleLevel, makeStyled<i, 48> }, \
            { prefix "Crash", styleLevel, makeStyled<i, 49> }, { prefix "Ride", styleLevel, makeStyled<i, 51> },
            VS_STYLE_KIT (0, "studio")
            VS_STYLE_KIT (1, "808")
            VS_STYLE_KIT (2, "rock")
            VS_STYLE_KIT (3, "lofi")
#undef VS_STYLE_KIT
        };
        return s;
    }

    inline const SoundDef* findSound (const std::string& id)
    {
        for (const auto& d : sounds()) if (id == d.id) return &d;
        return nullptr;
    }

    // One sound at `sr`, at its kit level; empty for an id it doesn't know
    inline Buffer renderSound (const std::string& id, double sr)
    {
        const auto* d = findSound (id);
        return d != nullptr ? finish (d->make (sr), d->level) : Buffer {};
    }

    // ---- the kits

    // A kit's drum: its pad (1..9, 0 for none), its General MIDI note (which
    // names it, drumName), its sound and its choke group (0 for none: a hit
    // in a group stops the others in it)
    struct KitPad { int slot; int note; const char* sound; int choke = 0; };

    struct Kit
    {
        const char* id;
        const char* name;
        const char* about;
        std::vector<KitPad> pads;
    };

    inline std::vector<KitPad> stylePads (const std::string& prefix)
    {
        static const std::vector<std::pair<int, const char*>> lanes { { 36, "Kick" }, { 38, "Snare" }, { 39, "Clap" }, { 42, "ClosedHat" }, { 46, "OpenHat" },
                                                                      { 41, "FloorTom" }, { 45, "LowTom" }, { 48, "HighTom" }, { 49, "Crash" }, { 51, "Ride" } };
        std::vector<KitPad> pads;
        for (const auto& [note, part] : lanes)
        {
            int slot = 0;
            for (const auto& p : music::drumPads) if (p.note == note) slot = p.slot;
            const auto id = prefix + part;
            const auto* d = findSound (id);
            pads.push_back ({ slot, note, d != nullptr ? d->id : "", note == 42 || note == 46 ? 1 : 0 });
        }
        return pads;
    }

    // Every kit, in the order a list shows them
    inline const std::vector<Kit>& kits()
    {
        static const std::vector<Kit> k {
            { "encore", "ENCORE.", "A punchy all-round kit.",
              { { 1, 36, "kick" }, { 2, 38, "snare" }, { 3, 42, "closedHat", 1 }, { 4, 39, "clap" }, { 5, 37, "rim" },
                { 6, 46, "openHat", 1 }, { 7, 48, "highTom" }, { 8, 45, "lowTom" }, { 9, 49, "crash" } } },
            { "boom", "Boom", "In the style of the classic analog drum machine: a deep, long, booming kick, a snappy snare, metallic hats and a cowbell.",
              { { 1, 36, "boomKick" }, { 2, 38, "boomSnare" }, { 3, 42, "boomClosedHat", 1 }, { 4, 39, "boomClap" }, { 5, 37, "boomRim" },
                { 6, 46, "boomOpenHat", 1 }, { 7, 48, "boomHighTom" }, { 8, 45, "boomLowTom" }, { 9, 56, "boomCowbell" } } },
            { "club", "Club", "In the style of the classic house and techno drum machine: a short punchy kick, a bright snare, crisp hats, ride and crash.",
              { { 1, 36, "clubKick" }, { 2, 38, "clubSnare" }, { 3, 42, "clubClosedHat", 1 }, { 4, 39, "clubClap" }, { 5, 37, "clubRim" },
                { 6, 46, "clubOpenHat", 1 }, { 7, 48, "clubTom" }, { 8, 51, "clubRide" }, { 9, 49, "clubCrash" } } },
            { "studio", "Studio", "Short, punchy, mix-ready drums.", stylePads ("studio") },
            { "808", "808", "A long booming kick and tight hats.", stylePads ("808") },
            { "rock", "Rock", "Punchy and roomy, with a driven edge.", stylePads ("rock") },
            { "lofi", "Lo-fi", "Crunchy and dark: fewer bits, a lower sample rate.", stylePads ("lofi") },
        };
        return k;
    }

    inline const Kit* findKit (const std::string& id)
    {
        for (const auto& k : kits()) if (id == k.id) return &k;
        return nullptr;
    }

    // The sound a kit plays for a General MIDI note: the pad with that note,
    // else the pad padForNote() stands in with; nullptr for none
    inline const char* kitSound (const Kit& kit, int note)
    {
        for (const auto& p : kit.pads) if (p.note == note) return p.sound;
        if (const int slot = music::padForNote (note); slot > 0)
            for (const auto& p : kit.pads) if (p.slot == slot) return p.sound;
        return nullptr;
    }

    // A sound's name for a list: the kit's name and the drum's (the ENCORE.
    // kit's sounds go by the drum's name alone)
    inline std::string soundName (const std::string& id)
    {
        if (id == "shaker") return "Shaker";
        for (const auto& k : kits())
            for (const auto& p : k.pads)
                if (id == p.sound) return (std::string (k.id) == "encore" ? std::string() : std::string (k.name) + ": ") + music::drumName (p.note);
        return {};
    }
}
