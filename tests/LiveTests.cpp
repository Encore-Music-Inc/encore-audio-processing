// Live recordings, built without JUCE as the browser build is: finding the
// songs in a live set (dsp/LiveSet.h) on synthetic sets with known song
// boundaries (rock with drums, a quiet folk song, banter, applause, tuning,
// a song with a breakdown), the slow ride's gain track (LevelRider), a gain
// track on audio (dsp/Mix.h), the inputs routed and surveyed and each batch's
// peaks and full-scale samples (dsp/Channels.h), and how much later a room pair
// hears the band than the board (dsp/TimingCheck.h). Each check prints PASS
// or FAIL; the suite exits non-zero on any failure.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "dsp/Channels.h"
#include "dsp/LiveSet.h"
#include "dsp/Mix.h"
#include "dsp/Rider.h"
#include "dsp/TimingCheck.h"

namespace
{
    int failures = 0;
    constexpr double twoPi = 6.283185307179586;

    void expect (const std::string& name, bool ok, const char* fmt = "", double a = 0, double b = 0, double c = 0)
    {
        std::printf ("%s %-72s ", ok ? "PASS" : "FAIL", name.c_str());
        std::printf (fmt, a, b, c);
        std::printf ("\n");
        if (! ok) ++failures;
    }

    // ---- synthetic live-set sounds (mono, 44.1 kHz), from one seeded generator

    constexpr double fs = 44100.0;
    std::uint32_t seed = 7;
    double rnd() { seed = seed * 1664525u + 1013904223u; return (double) seed / 4294967296.0 * 2.0 - 1.0; }
    using Sound = std::vector<float>;
    Sound make (double sec) { return Sound ((size_t) (sec * fs), 0.0f); }

    Sound rockSong (double sec, double breakdownAt = -1)
    {
        auto x = make (sec);
        double b = 0, lp = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = (double) i / fs, beat = std::fmod (t, 0.5), bar = std::fmod (t, 2.0);
            const bool inBreak = breakdownAt >= 0 && t > breakdownAt && t < breakdownAt + 4;
            const double kick = inBreak ? 0 : std::exp (-beat * 25) * std::sin (twoPi * 55 * beat) * 0.8;
            const double bass = inBreak ? 0 : 0.25 * std::sin (twoPi * (bar < 1 ? 82 : 98) * t);
            const double chord = (inBreak ? 0.06 : 0.12) * (std::sin (twoPi * 196 * t) + std::sin (twoPi * 247 * t) + std::sin (twoPi * 294 * t));
            const double w = rnd();
            b = 0.6 * b + 0.4 * w;
            lp += 0.3 * (w - lp);
            const double hat = inBreak ? 0 : (std::fmod (t, 0.25) < 0.03 ? (w - lp) * 0.25 : 0);
            const double voice = 0.1 * std::sin (twoPi * (220 + 30 * std::sin (t * 2)) * t) * (std::sin (t * 1.3) > -0.3 ? 1 : 0);
            x[i] = (float) (kick + bass + chord + hat + voice + b * 0.04);
        }
        return x;
    }

    Sound folkSong (double sec)
    {
        auto x = make (sec);
        const double notes[] { 196, 247, 294, 392, 330, 262 };
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = (double) i / fs, strum = std::fmod (t, 0.75);
            const int k = (int) std::floor (t / 0.75) % 6;
            double g = 0;
            for (int h = 1; h <= 4; ++h) g += std::sin (twoPi * notes[k] * h * t) / h;
            const double body = 0.05 * std::sin (twoPi * 98 * t);
            const double voice = 0.07 * std::sin (twoPi * (262 + 20 * std::sin (t * 3)) * t) * (std::sin (t * 0.9) > -0.2 ? 1 : 0);
            x[i] = (float) (0.12 * g * std::exp (-strum * 3) + body + voice + rnd() * 0.004);
        }
        return x;
    }

    // syllables of band-limited noise through the PA, with pauses
    Sound banter (double sec)
    {
        auto x = make (sec);
        double b1 = 0, b2 = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = (double) i / fs, syl = std::pow (std::max (0.0, std::sin (twoPi * 4.3 * t)), 2.0), phrase = std::fmod (t, 3.2) < 2.4 ? 1 : 0;
            const double w = rnd();
            b1 += 0.25 * (w - b1);
            b2 += 0.04 * (w - b2);
            x[i] = (float) ((b1 - b2) * syl * phrase * 0.5 + 0.06 * std::sin (twoPi * (140 + 20 * std::sin (t * 5)) * t) * syl * phrase + rnd() * 0.003);
        }
        return x;
    }

    Sound applause (double sec)
    {
        auto x = make (sec);
        double lp = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = (double) i / fs, env = std::min (1.0, t * 3) * std::exp (-t / (sec * 0.6));
            const double w = rnd();
            lp += 0.5 * (w - lp);
            x[i] = (float) ((w - lp) * env * (0.35 + 0.25 * std::max (0.0, rnd())));
        }
        return x;
    }

    // tuning: a few lone notes
    Sound noodle (double sec)
    {
        auto x = make (sec);
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = (double) i / fs, k = std::fmod (t, 1.5);
            x[i] = (float) (0.08 * std::sin (twoPi * 110 * t) * std::exp (-k * 2) + rnd() * 0.003);
        }
        return x;
    }

    Sound quiet (double sec)
    {
        auto x = make (sec);
        for (auto& v : x) v = (float) (rnd() * 0.002);
        return x;
    }

    struct Part { Sound audio; const char* song = nullptr; };
    struct Truth { std::string name; double start, end; };
    struct LiveSet { Sound x; std::vector<Truth> truth; };

    LiveSet makeSet (std::vector<Part> parts)
    {
        LiveSet s;
        for (auto& p : parts)
        {
            const double at = (double) s.x.size() / fs;
            s.x.insert (s.x.end(), p.audio.begin(), p.audio.end());
            if (p.song != nullptr) s.truth.push_back ({ p.song, at, (double) s.x.size() / fs });
        }
        return s;
    }

    vs::liveset::Set detect (const Sound& x, bool stereo = true)
    {
        vs::LiveSetAnalyzer a (fs);
        const int chunk = (int) fs * 10;
        for (size_t i = 0; i < x.size(); i += (size_t) chunk)
        {
            const int n = (int) std::min<size_t> ((size_t) chunk, x.size() - i);
            a.push (x.data() + i, stereo ? x.data() + i : nullptr, n);
        }
        return a.finish();
    }

    void check (const std::string& label, const LiveSet& set)
    {
        const auto res = detect (set.x);
        expect (label + ": the songs found", res.songs.size() == set.truth.size(), "found %.0f of %.0f", (double) res.songs.size(), (double) set.truth.size());
        for (size_t i = 0; i < set.truth.size() && i < res.songs.size(); ++i)
        {
            const auto& t = set.truth[i];
            const auto& s = res.songs[i];
            expect ("  " + t.name + ": starts within 3 s and ends within 5 s of the truth",
                    std::abs (s.start - t.start) <= 3 && std::abs (s.end - t.end) <= 5, "start %.1f (%.1f), end %.1f", s.start, t.start, s.end);
        }
    }

    void songFinder()
    {
        check ("A typical set", makeSet ({ { quiet (6) }, { rockSong (150), "Rock opener" }, { applause (6) }, { banter (25) }, { noodle (10) },
                                           { folkSong (130), "Quiet folk song" }, { applause (5) }, { banter (40) },
                                           { rockSong (170, 80), "Song with a breakdown" }, { applause (8) }, { quiet (5) } }));
        check ("A punk set, songs back to back", makeSet ({ { quiet (3) }, { rockSong (110), "Song 1" }, { applause (4) }, { rockSong (95), "Song 2" },
                                                             { applause (3) }, { rockSong (120), "Song 3" }, { applause (6) } }));
        const auto folk = makeSet ({ { banter (20) }, { folkSong (160), "Folk 1" }, { applause (5) }, { banter (30) }, { folkSong (140), "Folk 2" },
                                     { applause (6) }, { quiet (8) } });
        check ("A folk set, no drums anywhere", folk);

        const auto set = detect (folk.x);
        // the banter here is shaped noise, so it reads as applause (as it did before the
        // finder moved into this repository); the quiet tail outlasts the last applause
        std::string kinds;
        for (const auto& g : set.gaps) kinds += std::string (vs::liveset::kindName (g.kind)) + " ";
        expect ("between and after the songs: applause, applause, quiet", kinds == "applause applause quiet ", "");
        if (kinds != "applause applause quiet ") std::printf ("     gaps: %s\n", kinds.c_str());
        expect ("every second has a kind", (int) set.timeline.size() == set.seconds && set.judged, "%.0f s", (double) set.seconds);
        const auto mono = detect (folk.x, false);
        bool same = mono.songs.size() == set.songs.size() && mono.timeline == set.timeline;
        for (size_t i = 0; same && i < set.songs.size(); ++i) same = mono.songs[i].start == set.songs[i].start && mono.songs[i].end == set.songs[i].end;
        expect ("one channel is heard as the same channel twice", same);
        const auto shortOne = detect (quiet (4));
        expect ("under 5 s nothing is judged", ! shortOne.judged && shortOne.songs.empty() && shortOne.timeline.empty(), "%.0f s", (double) shortOne.seconds);
        // the split sits at the centre of the lower group's top bin (of 64)
        const double split = vs::liveset::otsu ({ 1, 1.1, 0.9, 5, 5.2, 4.8 });
        expect ("Otsu splits two groups at the top of the lower one", std::abs (split - (0.9 + 2.5 / 64 * 4.3)) < 1e-12, "%.6f", split);
        expect ("Otsu of one value is that value", vs::liveset::otsu ({ 3, 3, 3 }) == 3);
    }

    // ---- the slow ride

    void ride()
    {
        // 30 s of a steady tone at about -30 dB, then 20 s of silence, then 30 s at about -10 dB
        const auto tone = [] (double sec, double amp)
        {
            auto x = make (sec);
            for (size_t i = 0; i < x.size(); ++i) x[i] = (float) (amp * std::sin (twoPi * 220 * (double) i / fs));
            return x;
        };
        auto x = tone (30, 0.045);
        const auto silent = make (20);
        const auto loud = tone (30, 0.45);
        x.insert (x.end(), silent.begin(), silent.end());
        x.insert (x.end(), loud.begin(), loud.end());
        vs::LevelRider r (fs);
        for (size_t i = 0; i < x.size(); i += 4096) r.push (x.data() + i, nullptr, (int) std::min<size_t> (4096, x.size() - i));
        const auto& t = r.track();
        double worstStep = 0;
        for (size_t k = 1; k < t.size(); ++k) worstStep = std::max (worstStep, (double) std::abs (t[k] - t[k - 1]));
        expect ("a gain value every 0.1 s", t.size() == (size_t) (80 * 10), "%.0f values", (double) t.size());
        expect ("it moves at most 1 dB a second", worstStep <= 0.1 + 1e-6, "%.4f dB a step", worstStep);
        expect ("a quiet stretch is brought up toward -18 dB, at most 12 dB", t[299] > 5 && t[299] <= 12, "%.2f dB", t[299]);
        expect ("it holds still through the silence", t[499] == t[300], "%.2f / %.2f dB", t[300], t[499]);
        expect ("a loud stretch is turned down", t.back() < 0, "%.2f dB", t.back());
    }

    // ---- a gain track on audio

    void gainTrack()
    {
        std::vector<float> l (100, 1.0f), r (100, 1.0f);
        vs::mix::applyGainTrack (nullptr, 0, 0.1, -6.0, 100, 0, l.data(), r.data(), 100);
        expect ("no track: the offset alone", std::abs (l[50] - std::pow (10.0, -6.0 / 20)) < 1e-6 && r[0] == l[0]);
        // two values 0.1 s apart at 100 Hz: 0 dB then -20 dB, moving in a straight line in dB
        const float db[] { 0.0f, -20.0f };
        std::vector<float> x (40, 1.0f);
        vs::mix::applyGainTrack (db, 2, 0.1, 0.0, 100, 0, x.data(), nullptr, 40);
        expect ("halfway between two values: halfway in dB", std::abs (20 * std::log10 (x[5]) + 10) < 1e-5, "%.4f dB", 20 * std::log10 (x[5]));
        expect ("after the last value it holds", std::abs (20 * std::log10 (x[30]) + 20) < 1e-5, "%.4f dB", 20 * std::log10 (x[30]));
        std::vector<float> y (5, 1.0f);
        vs::mix::applyGainTrack (db, 2, 0.1, 0.0, 100, 5, y.data(), nullptr, 5);
        expect ("start places the audio in the track's time", std::abs (y[0] - x[5]) < 1e-7);
        std::vector<float> z (10, 1.0f);
        vs::mix::applyGainTrack (nullptr, 0, 0.1, 0.0, 100, 0, z.data(), nullptr, 10);
        expect ("no track and no offset: untouched", z[3] == 1.0f);
    }

    // ---- the inputs

    void inputs()
    {
        const int n = 8, d = 2;
        std::vector<std::vector<float>> raw (4, std::vector<float> ((size_t) (n + d)));
        for (int c = 0; c < 4; ++c) for (int i = 0; i < n + d; ++i) raw[(size_t) c][(size_t) i] = (float) (c * 100 + i);
        const float* p[4] { raw[0].data(), raw[1].data(), raw[2].data(), raw[3].data() };
        std::vector<float> l ((size_t) n), r ((size_t) n);
        vs::channels::routeLive (p, 4, n, 0, 0, 1, false, -1, -1, 0, l.data(), r.data());
        expect ("the main pair, left and right", l[3] == 3 && r[3] == 103);
        vs::channels::routeLive (p, 4, n, 0, 0, 1, true, -1, -1, 0, l.data(), r.data());
        expect ("two separate inputs blended to the middle", l[3] == 53 && r[3] == 53);
        vs::channels::routeLive (p, 4, n, 0, 2, 2, true, -1, -1, 0, l.data(), r.data());
        expect ("one input heard in both ears (not blended with itself)", l[3] == 203 && r[3] == 203);
        vs::channels::routeLive (p, 4, n, d, 0, 1, false, 2, 3, 0, l.data(), r.data());
        expect ("the room blended in, the main held back to line up", l[3] == 3 + 205 && r[3] == 103 + 305, "%.0f / %.0f", l[3], r[3]);
        vs::channels::routeLive (p, 4, n, 0, 9, -1, false, -1, -1, 0, l.data(), r.data());
        expect ("an input past the last is the last", l[0] == 300 && r[0] == 300);
        vs::channels::routeLive (p, 2, n, 0, 0, 1, false, 2, 3, 0, l.data(), r.data());
        expect ("a room with two inputs in all is left out", l[3] == 3 && r[3] == 103);

        // the survey: a stereo pair that moves together, then a quiet different input
        const int len = 1000;
        std::vector<std::vector<float>> w (3, std::vector<float> ((size_t) len));
        for (int i = 0; i < len; ++i)
        {
            const float v = (float) std::sin (0.05 * i);
            w[0][(size_t) i] = v;
            w[1][(size_t) i] = 0.5f * v;
            w[2][(size_t) i] = (float) (0.001 * std::cos (0.37 * i));
        }
        const float* q[3] { w[0].data(), w[1].data(), w[2].data() };
        vs::channels::InputSurvey s (3);
        s.add (q, len);
        expect ("survey: a pair moving together correlates fully", std::abs (s.nextCorr (0) - 1.0) < 1e-9, "%.6f", s.nextCorr (0));
        expect ("survey: levels, 6 dB apart for half the signal", std::abs ((s.levelDb (0) - s.levelDb (1)) - 6.0206) < 1e-3, "%.4f dB", s.levelDb (0) - s.levelDb (1));
        expect ("survey: the last input has no next", s.nextCorr (2) == 0.0);
        expect ("survey: a dozen 2 s windows spread across the file",
                vs::channels::InputSurvey::windowLength (480000, 48000) == 96000 && vs::channels::InputSurvey::windowStart (0, 480000, 48000) == 0
                    && vs::channels::InputSurvey::windowStart (11, 480000, 48000) == 412000,
                "%.0f", (double) vs::channels::InputSurvey::windowStart (11, 480000, 48000));

        // a batch: each channel's peak and its samples at full scale
        const float batch[] { 0.5f, -1.0f, 0.999f, 0.2f, -0.3f, 0.9995f };
        float peaks[2];
        int overloads[2];
        vs::channels::peaksAndOverloads (batch, 3, 2, peaks, overloads);
        expect ("a batch's peaks and its samples at full scale", peaks[0] == 0.999f && peaks[1] == 1.0f && overloads[0] == 1 && overloads[1] == 2);
    }

    // ---- lining up the room

    void roomDelay()
    {
        const double rate = 48000;
        const int n = (int) rate * 8, lag = 1234;
        std::vector<float> aL ((size_t) n), aR ((size_t) n), bL ((size_t) n), bR ((size_t) n);
        std::uint32_t s = 3;
        const auto noise = [&] { s = s * 1664525u + 1013904223u; return (double) s / 4294967296.0 * 2.0 - 1.0; };
        std::vector<double> src ((size_t) (n + lag));
        double lp = 0;
        for (auto& v : src) { lp += 0.2 * (noise() - lp); v = lp; }
        for (int i = 0; i < n; ++i)
        {
            aL[(size_t) i] = aR[(size_t) i] = (float) src[(size_t) (i + lag)];
            const double later = src[(size_t) i];
            bL[(size_t) i] = (float) (0.5 * later + 0.02 * noise());
            bR[(size_t) i] = (float) (0.5 * later + 0.02 * noise());
        }
        const int got = vs::timing::delayBetween (aL.data(), aR.data(), bL.data(), bR.data(), n, rate);
        expect ("the room hears the board 1234 samples (25.7 ms) later", got == lag, "%.0f", (double) got);
        const int none = vs::timing::delayBetween (aL.data(), aR.data(), aL.data(), aR.data(), n, rate);
        expect ("the same feed is 0 later", none == 0, "%.0f", (double) none);
    }
}

int main()
{
    songFinder();
    ride();
    gainTrack();
    inputs();
    roomDelay();
    std::printf ("%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
