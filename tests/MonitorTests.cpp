// Mastering as you listen and comparing songs, built without JUCE as the
// browser build is: vs::MasterMonitor (dsp/MasterMonitor.h) plays what the
// chain alone plays, the original lined up and at the master's loudness, a
// service's level and a device; vs::SongProfile (dsp/SongProfile.h) finds the
// loudest stretch and measures tone, width and squash there; and
// vs::LoudestStretch is the stretch vs::Mastering learns on. Each check prints
// PASS or FAIL; the suite exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <vector>
#include "dsp/MasterMonitor.h"
#include "dsp/SongProfile.h"

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

    // pinkish noise and a kick, louder where gain (t) says
    template <typename Gain>
    Song makeSong (double seconds, double fs, Gain gain, double width = 1.0)
    {
        const int n = (int) (seconds * fs);
        Song s { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n), fs };
        std::uint32_t seed = 7;
        auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return seed / 4294967296.0 * 2 - 1; };
        double b[2][3] {};
        for (int i = 0; i < n; ++i)
        {
            const double t = i / fs, beat = std::fmod (t, 0.5);
            const double kick = std::exp (-beat * 30) * std::sin (twoPi * 55 * beat) * 0.6;
            double ch[2];
            for (int c = 0; c < 2; ++c)
            {
                const double w = rnd();
                b[c][0] = 0.99 * b[c][0] + 0.05 * w; b[c][1] = 0.9 * b[c][1] + 0.1 * w; b[c][2] = 0.5 * b[c][2] + 0.2 * w;
                ch[c] = (b[c][0] + b[c][1] + b[c][2]) * 0.2;
            }
            // width 0: the same noise on both sides; 1: independent
            const double l = ch[0], r = width * ch[1] + (1 - width) * ch[0];
            s.l[(size_t) i] = (float) ((l + kick) * gain (t));
            s.r[(size_t) i] = (float) ((r + kick) * gain (t));
        }
        return s;
    }

    float rmsDb (const std::vector<float>& x, size_t from, size_t to)
    {
        double e = 0;
        for (size_t i = from; i < to; ++i) e += (double) x[i] * x[i];
        return (float) (10 * std::log10 (e / (double) std::max<size_t> (1, to - from) + 1e-20));
    }

    float lufsOf (const Song& s, size_t from, size_t to)
    {
        vs::LoudnessMeter m;
        m.prepare (s.fs);
        m.process (s.l.data() + from, s.r.data() + from, (int) (to - from));
        return m.integrated();
    }

    // The song through a monitor (block by block), or through a chain alone with the same settings
    template <typename Setup>
    Song throughMonitor (const Song& s, Setup setup, int block = 128)
    {
        vs::MasterMonitor m;
        m.prepare (s.fs, block);
        vs::MasterChain::Settings st;
        st.style = 0;
        m.chain.setSettings (st);
        setup (m);
        Song out = s;
        for (size_t i = 0; i < out.l.size(); i += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, out.l.size() - i);
            m.process (out.l.data() + i, out.r.data() + i, n);
        }
        return out;
    }

    Song throughChain (const Song& s, int block = 128)
    {
        vs::MasterChain c;
        c.prepare (s.fs, block);
        vs::MasterChain::Settings st;
        st.style = 0;
        c.setSettings (st);
        Song out = s;
        for (size_t i = 0; i < out.l.size(); i += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, out.l.size() - i);
            c.process (out.l.data() + i, out.r.data() + i, n);
        }
        return out;
    }

    vs::SongProfile::Result profileOf (const Song& s, vs::LoudestStretch::Window* where = nullptr)
    {
        vs::SongProfile p;
        const long long len = (long long) s.l.size();
        p.begin (s.fs, len);
        for (long long i = 0; i < len; i += 4096)
            p.feedSong (s.l.data() + i, s.r.data() + i, (int) std::min (4096LL, len - i));
        const auto w = p.stretch();
        if (where != nullptr) *where = w;
        for (long long i = w.from; i < w.from + w.length; i += 4096)
            p.feedStretch (s.l.data() + i, s.r.data() + i, (int) std::min (4096LL, w.from + w.length - i));
        return p.finish();
    }
}

int main()
{
    const double fs = 44100;
    const auto song = makeSong (12, fs, [] (double) { return 0.5; });

    // ---- MasterMonitor
    {
        const auto chain = throughChain (song);
        const auto mon = throughMonitor (song, [] (vs::MasterMonitor&) {});
        double worst = 0;
        for (size_t i = 0; i < song.l.size(); ++i) worst = std::max (worst, (double) std::abs (chain.l[i] - mon.l[i]) + std::abs (chain.r[i] - mon.r[i]));
        expect ("the monitor plays what the chain alone plays", worst < 1e-6, "(largest difference %.2g)", worst);
    }
    {
        vs::MasterMonitor probe;
        probe.prepare (fs, 128);
        const int latency = probe.chain.getLatency();
        const auto chain = throughChain (song);
        const auto orig = throughMonitor (song, [] (vs::MasterMonitor& m) { m.setHearOriginal (true); });
        // after the 30 ms crossfade and the 3 s short-term window, the original
        // plays lined up, at a steady gain, at the master's loudness
        const size_t from = (size_t) (8 * fs), to = (size_t) (11 * fs);
        double ratioMin = 1e9, ratioMax = 0;
        for (size_t i = from; i < to; i += 997)
        {
            const double src = song.l[i - (size_t) latency];
            if (std::abs (src) < 0.05) continue;
            const double g = orig.l[i] / src;
            ratioMin = std::min (ratioMin, g); ratioMax = std::max (ratioMax, g);
        }
        // the match follows the 3 s short-term loudness, gliding, so it moves a little
        expect ("hear the original: the original, lined up with the chain's latency", ratioMin > 0 && ratioMax / ratioMin < 1.1, "(gain %.3f .. %.3f)", ratioMin, ratioMax);
        const float levelDiff = lufsOf (orig, from, to) - lufsOf (chain, from, to);
        expect ("hear the original: at the master's loudness", std::abs (levelDiff) < 0.5f, "(%.2f LU from the master)", levelDiff);
    }
    {
        const auto chain = throughChain (song);
        const auto lev = throughMonitor (song, [] (vs::MasterMonitor& m) { m.setPlaybackLevel (true, -4.0f, 2.0f); });
        const size_t from = (size_t) (2 * fs), to = (size_t) (11 * fs);
        const float d = rmsDb (lev.l, from, to) - rmsDb (chain.l, from, to);
        expect ("a service's level: the master at its gain", std::abs (d + 4.0f) < 0.01f, "(%.3f dB, asked -4)", d);
        const auto origLev = throughMonitor (song, [] (vs::MasterMonitor& m) { m.setPlaybackLevel (true, -4.0f, 2.0f); m.setHearOriginal (true); });
        vs::MasterMonitor probe;
        probe.prepare (fs, 128);
        const int latency = probe.chain.getLatency();
        double e1 = 0, e2 = 0;
        for (size_t i = from; i < to; ++i) { e1 += (double) origLev.l[i] * origLev.l[i]; e2 += (double) song.l[i - (size_t) latency] * song.l[i - (size_t) latency]; }
        const double od = 10 * std::log10 (e1 / e2);
        expect ("a service's level: the original at its own gain", std::abs (od - 2.0) < 0.01, "(%.3f dB, asked +2)", od);
    }
    {
        int phone = -1;
        for (size_t i = 0; i < vs::device::devices().size(); ++i)
            if (vs::device::devices()[i].mono) { phone = (int) i; break; }
        const auto dev = throughMonitor (song, [phone] (vs::MasterMonitor& m) { m.setDevice (phone); });
        bool same = phone >= 0;
        for (size_t i = 0; i < dev.l.size() && same; ++i) same = dev.l[i] == dev.r[i];
        expect ("a one-speaker device plays both sides as one", same);
    }
    {
        Song tone { std::vector<float> ((size_t) (2 * fs)), std::vector<float> ((size_t) (2 * fs)), fs };
        for (size_t i = 0; i < tone.l.size(); ++i) tone.l[i] = tone.r[i] = (float) (0.1 * std::sin (twoPi * 1000 * (double) i / fs));
        vs::MasterMonitor m;
        m.prepare (fs, 128);
        for (size_t i = 0; i < tone.l.size(); i += 128) m.process (tone.l.data() + i, tone.r.data() + i, (int) std::min<size_t> (128, tone.l.size() - i));
        m.spectrum.update();
        const auto& lv = m.spectrum.levelsDb();
        size_t best = 0;
        for (size_t b = 1; b < lv.size(); ++b) if (lv[b] > lv[best]) best = b;
        const double hz = best * fs / 8192.0;
        expect ("the spectrum hears what plays", std::abs (hz - 1000) < 10, "(loudest bin at %.0f Hz)", hz);
    }

    // ---- LoudestStretch and SongProfile
    {
        // quieter (6 dB down, inside the loudness gate), with a loud stretch from 40 s to 70 s
        const auto s = makeSong (100, fs, [] (double t) { return t >= 40 && t < 70 ? 0.6 : 0.3; });
        vs::LoudestStretch::Window w;
        const auto p = profileOf (s, &w);
        expect ("a reference is measured on its loudest 30 s", std::abs (w.from / fs - 40) <= 1 && std::abs (w.length / fs - 30) < 1e-9, "(from %.1f s, %.1f s long)", w.from / fs, w.length / fs);
        expect ("the stretch is louder than the whole song", p.lufs > p.wholeLufs + 1, "(%.1f vs %.1f LUFS)", p.lufs, p.wholeLufs);
    }
    {
        const auto s = makeSong (12, fs, [] (double) { return 0.5; });
        const auto p = profileOf (s);
        std::vector<float> mid (s.l.size());
        for (size_t i = 0; i < mid.size(); ++i) mid[i] = 0.5f * (s.l[i] + s.r[i]);
        const auto t = vs::tonalBalance (mid.data(), (std::int64_t) mid.size(), fs);
        double worst = 0;
        for (int b = 0; b < vs::SongProfile::numTone; ++b) worst = std::max (worst, std::abs ((double) p.tone[(size_t) b] + p.lufs - t.levelDb[b]));
        expect ("tone: tonal balance's octave levels, under the loudness", worst < 1e-3, "(largest difference %.2g dB)", worst);
        float peak = 0;
        for (size_t i = 0; i < s.l.size(); ++i) peak = std::max ({ peak, std::abs (s.l[i]), std::abs (s.r[i]) });
        expect ("squash: the peak over the loudness", std::abs (p.plr - (20 * std::log10 (peak) - p.lufs)) < 1e-3, "(%.2f dB)", p.plr);
        expect ("a song shorter than 30 s is measured whole", std::abs (p.lufs - p.wholeLufs) < 1e-3, "(%.2f vs %.2f LUFS)", p.lufs, p.wholeLufs);
    }
    {
        const auto narrow = profileOf (makeSong (10, fs, [] (double) { return 0.5; }, 0.0));
        const auto wide = profileOf (makeSong (10, fs, [] (double) { return 0.5; }, 1.0));
        expect ("width: a mono song has no side", narrow.width[1] < -60, "(%.1f dB at 1 kHz)", narrow.width[1]);
        expect ("width: a wide song's side is near its mid", wide.width[1] > -6 && wide.width[2] > -6, "(%.1f, %.1f dB)", wide.width[1], wide.width[2]);
    }
    {
        // a chosen stretch, with the whole song's loudness given
        const auto s = makeSong (12, fs, [] (double) { return 0.5; });
        vs::SongProfile p;
        p.begin (fs, (long long) s.l.size());
        p.setStretch ((long long) (2 * fs), (long long) (8 * fs), -9.5f);
        const auto w = p.stretch();
        p.feedStretch (s.l.data() + w.from, s.r.data() + w.from, (int) w.length);
        const auto r = p.finish();
        expect ("a chosen stretch keeps the whole song's loudness it was given", w.from == (long long) (2 * fs) && r.wholeLufs == -9.5f, "(%.1f LUFS)", r.wholeLufs);
    }
    {
        // vs::Mastering learns where vs::LoudestStretch says
        const auto s = makeSong (60, fs, [] (double t) { return t >= 20 && t < 50 ? 0.6 : 0.1; });
        vs::Mastering m;
        m.begin (fs, {}, (long long) s.l.size());
        vs::LoudestStretch ls;
        ls.begin (fs, (long long) s.l.size());
        m.feedEnergy (s.l.data(), s.r.data(), (int) s.l.size());
        ls.feed (s.l.data(), s.r.data(), (int) s.l.size());
        const auto a = m.learningWindow(), b = ls.window (vs::Mastering::windowSeconds);
        expect ("mastering learns on the loudest stretch", a.from == b.from && a.length == b.length, "(from %.1f s)", a.from / fs);
    }

    std::printf ("\n%s\n", failures == 0 ? "ALL PASSED" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}
