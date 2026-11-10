// Whole-song mastering (dsp/Mastering.h) built without JUCE, as the browser
// build is: the loudest stretch is where it learns, the master is the song's
// length and in line with it, block sizes don't change it, and it lands on the
// loudness target. Each check prints PASS or FAIL; the suite exits non-zero on
// any failure.
#include <cmath>
#include <cstdio>
#include <vector>
#include "dsp/Mastering.h"

namespace
{
    int failures = 0;
    constexpr double twoPi = 6.283185307179586;

    void expect (const char* name, bool ok, const char* fmt = "", double a = 0, double b = 0, double c = 0)
    {
        std::printf ("%s %-66s ", ok ? "PASS" : "FAIL", name);
        std::printf (fmt, a, b, c);
        std::printf ("\n");
        if (! ok) ++failures;
    }

    struct Song { std::vector<float> l, r; double fs; };

    // pinkish noise, a kick and a snare; gain (t) sets the level over time
    template <typename Gain>
    Song makeSong (double seconds, double fs, Gain gain)
    {
        const int n = (int) (seconds * fs);
        Song s { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n), fs };
        std::uint32_t seed = 1;
        auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return seed / 4294967296.0 * 2 - 1; };
        double b[2][3] {};
        for (int i = 0; i < n; ++i)
        {
            const double t = i / fs, beat = std::fmod (t, 0.5), sn = std::fmod (t + 0.25, 1.0);
            const double kick = std::exp (-beat * 30) * std::sin (twoPi * 55 * beat) * 0.9;
            const double snare = sn < 0.12 ? std::exp (-sn * 25) : 0.0;
            for (int c = 0; c < 2; ++c)
            {
                const double w = rnd();
                b[c][0] = 0.99 * b[c][0] + 0.05 * w; b[c][1] = 0.9 * b[c][1] + 0.1 * w; b[c][2] = 0.5 * b[c][2] + 0.2 * w;
                (c == 0 ? s.l : s.r)[(size_t) i] = (float) (((b[c][0] + b[c][1] + b[c][2]) * 0.21 + kick + snare * w * 0.6) * gain (t));
            }
        }
        return s;
    }

    struct Result { std::vector<float> l, r; float lufs; int renders; vs::Mastering::Window window; vs::MasterChain::Learned learned; };

    Result master (const Song& s, const vs::MasterChain::Settings& settings, int feedBlock, bool mono = false)
    {
        const long long len = (long long) s.l.size();
        vs::Mastering m;
        m.begin (s.fs, settings, len);
        const float* r = mono ? nullptr : s.r.data();
        for (long long i = 0; i < len; i += feedBlock)
            m.feedEnergy (s.l.data() + i, r != nullptr ? r + i : nullptr, (int) std::min ((long long) feedBlock, len - i));
        Result res;
        res.window = m.learningWindow();
        for (long long i = 0; i < res.window.length; i += feedBlock)
        {
            const long long at = res.window.from + i;
            m.feedWindow (s.l.data() + at, r != nullptr ? r + at : nullptr, (int) std::min ((long long) feedBlock, res.window.length - i));
        }
        m.learn();
        res.learned = m.learned();
        res.l.assign ((size_t) len, 0.0f);
        res.r.assign ((size_t) len, 0.0f);
        do
        {
            m.beginRender();
            long long o = 0;
            std::vector<float> ol ((size_t) vs::Mastering::maxRenderOutput (feedBlock) + (size_t) m.maxFinishOutput()), orr (ol.size());
            auto take = [&] (int k) { std::copy (ol.begin(), ol.begin() + k, res.l.begin() + o); std::copy (orr.begin(), orr.begin() + k, res.r.begin() + o); o += k; };
            for (long long i = 0; i < len; i += feedBlock)
                take (m.render (s.l.data() + i, r != nullptr ? r + i : nullptr, (int) std::min ((long long) feedBlock, len - i), ol.data(), orr.data()));
            take (m.finishRender (ol.data(), orr.data()));
            if (o != len) std::printf ("(rendered %lld of %lld samples)\n", o, len);
        }
        while (m.nextPass());
        res.lufs = m.renderedLufs();
        res.renders = m.rendersDone();
        return res;
    }
}

int main()
{
    const double fs = 44100;
    // quiet for 20 s, then loud for 25 s
    const auto song = makeSong (45, fs, [] (double t) { return t < 20 ? 0.1 : 0.5; });
    vs::MasterChain::Settings s;

    const auto a = master (song, s, 65536);
    expect ("it learns on the loudest 30 s (the loud part, at the end)", a.window.from == (long long) (15 * fs) && a.window.length == (long long) (30 * fs),
            "(from %.1f s, %.1f s long)", a.window.from / fs, a.window.length / fs);
    expect ("the master is exactly the song's length", a.l.size() == song.l.size());
    expect ("it lands within 0.25 LU of the target", std::abs (a.lufs - s.targetLufs) < 0.25, "(%.2f LUFS in %.0f render(s))", a.lufs, a.renders);

    // in line with the song: the master's correlation with the song peaks at no delay
    {
        // on the first difference (the highs), where the mastering's low-end filters don't shift the peak
        auto corr = [&] (int lag)
        {
            double c = 0;
            for (size_t i = (size_t) (25 * fs); i < (size_t) (40 * fs); ++i)
                c += (double) (a.l[i] - a.l[i - 1]) * (song.l[i - (size_t) lag] - song.l[i - 1 - (size_t) lag]);
            return c;
        };
        int best = 0; double bestC = -1e30;
        for (int lag = -8; lag <= 8; ++lag) if (corr (lag) > bestC) { bestC = corr (lag); best = lag; }
        expect ("the chain's latency is taken out (the master lines up with the song)", best == 0, "(best lag %.0f)", best);
    }

    const auto b = master (song, s, 777);
    expect ("feeding it in other block sizes gives the same master", a.l == b.l && a.r == b.r && a.lufs == b.lufs);

    // a mono song (r null) is mastered as both channels of a stereo pair
    {
        Song mono { song.l, song.l, fs };
        const auto m1 = master (song, s, 50000, true), m2 = master (mono, s, 50000);
        expect ("a mono song is mastered as the same audio on both sides", m1.l == m2.l && m1.r == m2.r && m1.l == m1.r);
    }

    // the drive held at its limit: no render repeats for nothing
    {
        const auto quiet = makeSong (12, fs, [] (double) { return 0.02; });
        vs::MasterChain::Settings loud; loud.style = 1; loud.targetLufs = -6.0f;
        const auto q = master (quiet, loud, 65536);
        expect ("a song shorter than 30 s learns on all of it", q.window.from == 0 && q.window.length == (long long) quiet.l.size());
        vs::Mastering m;
        m.begin (fs, loud, (long long) quiet.l.size());
        vs::MasterChain::Learned l = q.learned;
        l.driveDb = 24.0f;
        m.setLearned (l);
        std::vector<float> ol (quiet.l.size() + 8192), orr (ol.size());
        m.beginRender();
        const int k = m.render (quiet.l.data(), quiet.r.data(), (int) quiet.l.size(), ol.data(), orr.data());
        m.finishRender (ol.data() + k, orr.data() + k);
        expect ("with the drive at +24 dB and the target out of reach, it stops", m.renderedLufs() < -6.25f && ! m.nextPass(), "(%.2f LUFS)", m.renderedLufs());
    }

    std::printf ("%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
