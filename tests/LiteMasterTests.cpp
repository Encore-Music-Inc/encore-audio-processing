// The mastering chain built WITHOUT JUCE (VS_NO_JUCE, the browser build's
// configuration), checked against the same targets as the desktop build.
// True peak here is measured independently with 8x windowed-sinc
// interpolation, not with the chain's own oversampler.
#include <chrono>
#include <cstdio>
#include <random>
#include "dsp/MasterChain.h"

namespace
{
    constexpr double fs = 48000.0;
    int failures = 0;

    void expect (const char* name, bool ok, const char* fmt = "", double a = 0, double b = 0, double c = 0)
    {
        std::printf ("%s %-48s ", ok ? "PASS" : "FAIL", name);
        std::printf (fmt, a, b, c);
        std::printf ("\n");
        if (! ok) ++failures;
    }

    struct Stereo { std::vector<float> l, r; };

    Stereo music (double sec, float gain, unsigned seed)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<float> u (-1.0f, 1.0f);
        Stereo s { std::vector<float> ((size_t) (sec * fs)), std::vector<float> ((size_t) (sec * fs)) };
        float b[2][3] {};
        for (size_t i = 0; i < s.l.size(); ++i)
        {
            const double t = (double) i / fs, beat = std::fmod (t, 0.5);
            const float kick = (float) (std::exp (-beat * 30.0) * std::sin (6.283185307 * 55.0 * beat) * 0.9);
            const double sn = std::fmod (t + 0.25, 1.0);
            const float snareEnv = (float) (sn < 0.12 ? std::exp (-sn * 25.0) : 0.0);
            for (int c = 0; c < 2; ++c)
            {
                const float w = u (rng);
                b[c][0] = 0.99f * b[c][0] + 0.05f * w; b[c][1] = 0.9f * b[c][1] + 0.1f * w; b[c][2] = 0.5f * b[c][2] + 0.2f * w;
                (c == 0 ? s.l : s.r)[i] = ((b[c][0] + b[c][1] + b[c][2]) * 0.6f * 0.35f + kick + snareEnv * w * 0.6f) * gain;
            }
        }
        return s;
    }

    float lufsOf (const Stereo& s, size_t from)
    {
        vs::LoudnessMeter m;
        m.prepare (fs);
        m.process (s.l.data() + from, s.r.data() + from, (int) (s.l.size() - from));
        return m.integrated();
    }

    // Independent true-peak estimate: 8x windowed-sinc interpolation
    float truePeakDb (const Stereo& s)
    {
        constexpr int half = 16, os = 8;
        float peak = 0.0f;
        for (const auto* ch : { &s.l, &s.r })
        {
            const auto& x = *ch;
            for (size_t i = half; i + half < x.size(); ++i)
            {
                if (std::abs (x[i]) < 0.5f && std::abs (x[i + 1]) < 0.5f) continue; // only check loud spots
                for (int k = 0; k < os; ++k)
                {
                    const double frac = (double) k / os;
                    double acc = 0.0;
                    for (int j = -half + 1; j <= half; ++j)
                    {
                        const double tt = j - frac;
                        const double sinc = std::abs (tt) < 1e-9 ? 1.0 : std::sin (3.14159265358979 * tt) / (3.14159265358979 * tt);
                        const double win = 0.5 + 0.5 * std::cos (3.14159265358979 * tt / half);
                        acc += x[(size_t) ((long) i + j)] * sinc * win;
                    }
                    peak = std::max (peak, (float) std::abs (acc));
                }
            }
        }
        return 20.0f * std::log10 (peak + 1e-9f);
    }

    Stereo run (vs::MasterChain& chain, Stereo in)
    {
        for (size_t start = 0; start < in.l.size(); start += 128) // browser audio blocks are 128
        {
            const int len = (int) std::min ((size_t) 128, in.l.size() - start);
            chain.process (in.l.data() + start, in.r.data() + start, len);
        }
        return in;
    }
}

int main()
{
    // EBU 3341 #1 through the lite build
    {
        Stereo s { std::vector<float> ((size_t) (20 * fs)), std::vector<float> ((size_t) (20 * fs)) };
        const float a = std::pow (10.0f, -23.0f / 20.0f);
        for (size_t i = 0; i < s.l.size(); ++i) s.l[i] = s.r[i] = a * (float) std::sin (6.283185307 * 1000.0 * (double) i / fs);
        const float got = lufsOf (s, 0);
        expect ("meter: EBU 3341 #1 (-23 LUFS)", std::abs (got + 23.0f) < 0.1f, "%.2f LUFS", got);
    }

    const char* names[] { "Balanced", "Heavy", "Bright", "Warm", "Gentle", "Wide", "Arena", "Drive" };
    for (int st = 0; st < vs::MasterChain::numStyles; ++st)
        for (float g : { 0.08f, 0.9f })
        {
            vs::MasterChain chain;
            chain.prepare (fs, 128);
            vs::MasterChain::Settings s; s.style = st;
            chain.setSettings (s);
            auto out = run (chain, music (40.0, g, 5u + (unsigned) st));
            const float got = lufsOf (out, (size_t) (30 * fs)), tp = truePeakDb (out);
            char name[96];
            std::snprintf (name, sizeof name, "%s (%s) -> -14 LUFS, <= -1 dBTP", names[st], g < 0.5f ? "quiet" : "hot");
            expect (name, std::abs (got + 14.0f) < 1.0f && tp <= -0.9f, "out %.2f LUFS, TP %.2f dBTP", got, tp);
        }

    {
        vs::MasterChain chain;
        chain.prepare (fs, 128);
        chain.setSettings ({});
        Stereo z { std::vector<float> ((size_t) (3 * fs), 0.0f), std::vector<float> ((size_t) (3 * fs), 0.0f) };
        auto out = run (chain, z);
        float p = 0.0f;
        for (float v : out.l) p = std::max (p, std::abs (v));
        expect ("silence stays silent", p < 1e-6f && std::isfinite (p), "peak %.2g", p);
    }

    {
        vs::MasterChain chain;
        chain.prepare (fs, 128);
        vs::MasterChain::Settings s; s.style = vs::MasterChain::cinematic; s.intensity = 1.0f;
        chain.setSettings (s);
        auto in = music (20.0, 0.2f, 99);
        const auto t0 = std::chrono::steady_clock::now();
        run (chain, in);
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("INFO lite build CPU: %.2f%% of one core\n", ms / 20000.0 * 100.0);
    }

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
