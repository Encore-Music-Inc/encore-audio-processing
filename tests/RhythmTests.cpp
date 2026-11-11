// The rhythm tools, built without JUCE as the browser build is: tap tempo and
// the tempo finder (dsp/Tempo.h), the onset detector (dsp/Onsets.h), and
// latency, the timing check and its report (dsp/TimingCheck.h), on synthetic
// playing with known times. Each check prints PASS or FAIL; the suite exits
// non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "dsp/Onsets.h"
#include "dsp/Random.h"
#include "dsp/Tempo.h"
#include "dsp/TimingCheck.h"

namespace
{
    int failures = 0;
    constexpr double pi = 3.141592653589793;
    constexpr double SR = 48000;

    void expect (const std::string& name, bool ok, const std::string& detail = "")
    {
        std::printf ("%s %-66s %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
        if (! ok) ++failures;
    }

    std::string num (double v, int digits = 3)
    {
        char b[64];
        std::snprintf (b, sizeof b, "%.*f", digits, v);
        return b;
    }

    // ---- tap tempo

    void taps()
    {
        // tapping along: 24 taps at 123.4 BPM, up to 15 ms off each, one missed
        vs::ParkMiller r (5);
        std::vector<double> t;
        for (int i = 0; i < 24; ++i)
            if (i != 10) t.push_back (500 + (i * 60000.0) / 123.4 + (r.uniform() - 0.5) * 30);
        const auto f = vs::tempo::tapTempo (t.data(), (int) t.size());
        expect ("taps: a line through every tap (one missed) within 0.3 BPM", f.valid && std::abs (f.bpm - 123.4) < 0.3, num (f.bpm));
        expect ("taps: spread under 15 ms, every tap used", f.spreadMs < 15 && f.used == 23, num (f.spreadMs));
        const double two[] { 0, 500 };
        expect ("taps: needs 3", ! vs::tempo::tapTempo (two, 2).valid);
        // a way-off tap is left out of the fit
        std::vector<double> off;
        for (int i = 0; i < 12; ++i) off.push_back (i * 500.0 + (i == 6 ? 200 : 0));
        const auto g = vs::tempo::tapTempo (off.data(), (int) off.size());
        expect ("taps: a tap 200 ms off is left out, 120 BPM exactly", g.used == 11 && std::abs (g.bpm - 120) < 1e-9, num (g.bpm, 6));

        // the live button: 120, a flubbed tap, then a pause and 150
        vs::tempo::TapTempo live;
        vs::tempo::TapFit b, odd, c;
        expect ("live: nothing after one tap", ! live.tap (1000).valid);
        for (int i = 1; i < 6; ++i) b = live.tap (1000 + i * 500);
        odd = live.tap (1000 + 6 * 500 + 120);
        for (int i = 0; i < 4; ++i) c = live.tap (20000 + i * 400);
        expect ("live: 120 BPM from steady taps", b.valid && std::abs (b.bpm - 120) < 1e-9, num (b.bpm, 6));
        expect ("live: a flubbed tap moves it less than to 125", odd.valid && odd.bpm > 100 && odd.bpm < 125, num (odd.bpm));
        expect ("live: a pause starts over (150 BPM)", c.valid && std::abs (c.bpm - 150) < 1e-9 && c.taps == 4, num (c.bpm, 6));
        // only the latest taps count
        vs::tempo::TapTempo latest (2000, 5);
        vs::tempo::TapFit e;
        for (int i = 0; i < 10; ++i) e = latest.tap (i * 600.0);
        for (int i = 0; i < 5; ++i) e = latest.tap (9 * 600.0 + (i + 1) * 500.0);
        expect ("live: the latest 5 taps give the new tempo", e.taps == 5 && std::abs (e.bpm - 120) < 1e-9, num (e.bpm, 6));
    }

    // ---- the tempo finder, on synthetic grooves

    std::vector<float> groove (vs::ParkMiller& r, double sr, double bpm, int style)
    {
        std::vector<float> x ((size_t) (sr * 20), 0.0f);
        const double beat = 60 / bpm;
        auto hit = [&] (double t, double f, double dec, double amp, bool noise)
        {
            const long s0 = (long) std::floor (t * sr);
            for (long i = 0; i < sr * 0.4 && s0 + i < (long) x.size(); ++i)
            {
                const double tt = i / sr;
                const double e = std::exp (-tt * dec);
                const double v = amp * e * (noise ? r.uniform() * 2 - 1 : std::sin (2 * pi * f * tt * (1 + 2 * std::exp (-tt * 30))));
                if (s0 + i >= 0) x[(size_t) (s0 + i)] += (float) v;   // a hit just before the start is cut
            }
        };
        for (int b = 0; b * beat < 19.5; ++b)
        {
            const double t = b * beat + (r.uniform() - 0.5) * 0.008;
            if (style == 0)
            {
                if (b % 2 == 0) hit (t, 55, 12, 0.8, false);
                else hit (t, 0, 18, 0.5, true);
                hit (t, 0, 60, 0.15, true);
                hit (t + beat / 2, 0, 60, 0.12, true);
            }
            else if (style == 1)
            {
                if (b % 2 == 0) hit (t, 49, 8, 0.7, false);
                else { hit (t, 0, 25, 0.35, true); hit (t, 392, 10, 0.25, false); hit (t, 494, 10, 0.2, false); }
            }
            else
            {
                if (b % 3 == 0) hit (t, 65, 8, 0.7, false);
                else { hit (t, 330, 12, 0.25, false); hit (t, 392, 12, 0.2, false); }
            }
        }
        for (size_t i = 0; i < x.size(); ++i)
            x[i] += (float) (0.05 * std::sin ((2 * pi * 220 * (double) i) / sr) + 0.003 * (r.uniform() * 2 - 1));
        return x;
    }

    vs::tempo::TempoEstimate listen (const std::vector<float>& x, double sr, int block)
    {
        vs::tempo::TempoFinder f (sr);
        for (size_t i = 0; i < x.size(); i += (size_t) block)
            f.push (x.data() + i, (int) std::min ((size_t) block, x.size() - i));
        return f.estimate();
    }

    void tempoFinder()
    {
        const double sr = 44100;
        vs::ParkMiller r (3);
        const struct { double bpm; int style; const char* name; } cases[] {
            { 128, 0, "rock 128" }, { 97.5, 0, "rock 97.5" }, { 140, 0, "rock 140" }, { 120, 1, "polka 120" }, { 90, 2, "waltz 90" } };
        for (const auto& c : cases)
        {
            const auto x = groove (r, sr, c.bpm, c.style);
            const auto e = listen (x, sr, 4096);
            expect (std::string ("tempo: ") + c.name + " within 0.1 BPM, counted the usual way, sure of it",
                    e.valid && std::abs (e.bpm - c.bpm) < 0.1 && e.confidence > 0.9,
                    num (e.bpm) + " BPM, confidence " + num (e.confidence, 2));
        }
        // no steady beat: random hits, slow chords
        std::vector<float> randomHits ((size_t) sr * 20, 0.0f), chords ((size_t) sr * 20, 0.0f);
        for (double t = 0; t < 19.5; t += 0.15 + r.uniform() * 0.9)
        {
            const long s0 = (long) std::floor (t * sr);
            for (long i = 0; i < 8000; ++i) randomHits[(size_t) (s0 + i)] += (float) (0.5 * std::exp (-i / 2000.0) * (r.uniform() * 2 - 1));
        }
        for (int c = 0; c < 6; ++c)
        {
            const long s0 = (long) std::floor ((c * 3.3 + r.uniform()) * sr);
            const double f0 = 200 + r.uniform() * 200;
            for (long i = 0; i < sr * 3 && s0 + i < (long) chords.size(); ++i)
                chords[(size_t) (s0 + i)] += (float) (0.2 * std::sin ((2 * pi * f0 * i) / sr) * std::min (1.0, i / 8000.0));
        }
        const auto a = listen (randomHits, sr, (int) randomHits.size());
        const auto b = listen (chords, sr, (int) chords.size());
        expect ("tempo: not sure of random hits", a.valid && a.confidence < 0.2, num (a.confidence, 2));
        expect ("tempo: not sure of slow chords", b.valid && b.confidence < 0.2, num (b.confidence, 2));
        // the same answer whatever the block size; nothing before 4 s; half and double offered
        vs::ParkMiller r2 (11);
        const auto g = groove (r2, sr, 110, 0);
        const auto e1 = listen (g, sr, 4096), e2 = listen (g, sr, 37);
        expect ("tempo: the same answer for any block size", e1.bpm == e2.bpm && e1.strength == e2.strength);
        expect ("tempo: half and double offered (55, 220)", e1.others.size() == 2 && std::abs (e1.others[0] - e1.bpm / 2) < 1e-12
                                                               && std::abs (e1.others[1] - e1.bpm * 2) < 1e-12);
        vs::tempo::TempoFinder early (sr);
        early.push (g.data(), (int) (sr * 3.9));
        expect ("tempo: nothing before 4 s", ! early.estimate().valid);
        // no rhythm: digital silence or a constant (nothing starts) is no tempo, not the slowest one searched
        const auto silent = listen (std::vector<float> ((size_t) (sr * 6), 0.0f), sr, 4096);
        const auto flat = listen (std::vector<float> ((size_t) (sr * 6), 0.25f), sr, 4096);
        expect ("tempo: silence and a constant have no tempo", ! silent.valid && silent.bpm == 0 && silent.others.empty() && ! flat.valid);
    }

    // ---- onsets, on synthetic playing

    vs::Mulberry32 R (99);
    double rr() { return R.uniform(); }

    std::vector<float> noiseFloor (size_t n, double level, std::uint32_t seed)
    {
        vs::Mulberry32 r (seed);
        std::vector<float> x (n);
        for (auto& v : x) v = (float) (level * (r.uniform() * 2 - 1));
        return x;
    }

    using Play = std::function<void (std::vector<float>&, long, double, int)>;

    // Adds v to x[i] (nothing outside the signal)
    void add (std::vector<float>& x, long i, double v)
    {
        if (i >= 0 && i < (long) x.size()) x[(size_t) i] = (float) (x[(size_t) i] + v);
    }

    const std::vector<std::pair<const char*, Play>>& instruments()
    {
        static const std::vector<std::pair<const char*, Play>> list {
            // a drum: a stick crack then a decaying tone with an off overtone
            { "drum", [] (std::vector<float>& x, long at, double g, int)
              {
                  for (long i = 0; i < SR * 0.4 && at + i < (long) x.size(); ++i)
                  {
                      const double t = i / SR;
                      add (x, at + i, g * (std::exp (-t * 14) * (0.6 * std::sin (2 * pi * 180 * t) + 0.3 * std::sin (2 * pi * 286 * t))
                                           + (i < SR * 0.003 ? 0.5 * (rr() - 0.5) * (1 - i / (SR * 0.003)) : 0)));
                  }
              } },
            // a clap: three quick noise bursts and a tail
            { "clap", [] (std::vector<float>& x, long at, double g, int)
              {
                  const double bursts[3][3] { { 0, 0.6, 0.002 }, { 0.0045, 0.8, 0.002 }, { 0.009, 1, 0.025 } };
                  for (const auto& b : bursts)
                  {
                      const long s = at + (long) std::floor (b[0] * SR + 0.5);
                      for (long i = 0; i < SR * 0.12 && s + i < (long) x.size(); ++i) add (x, s + i, g * b[1] * std::exp (-i / (b[2] * SR)) * (rr() - 0.5));
                  }
              } },
            // a piano note: a hammer knock, then a harmonic tone; a new pitch each time
            { "piano", [] (std::vector<float>& x, long at, double g, int k)
              {
                  const int steps[8] { 0, 2, 4, 5, 7, 9, 11, 12 };
                  const double f = 262 * std::pow (2.0, steps[(k * 3) % 8] / 12.0);
                  for (long i = 0; i < SR * 0.6 && at + i < (long) x.size(); ++i)
                  {
                      const double t = i / SR;
                      double v = 0;
                      for (int h = 1; h <= 8; ++h) v += (std::sin (2 * pi * f * h * t) * std::exp (-t * (2 + h))) / h;
                      add (x, at + i, g * (0.5 * v * std::min ({ 1.0, t / 0.002, (0.6 - t) / 0.1 }) + (i < SR * 0.002 ? 0.15 * (rr() - 0.5) : 0)));
                  }
              } },
            // a strum: six strings, 5 ms apart, low to high, each with a little pick noise
            { "strum", [] (std::vector<float>& x, long at, double g, int)
              {
                  const double fs[6] { 82, 110, 147, 196, 247, 330 };
                  for (int s = 0; s < 6; ++s)
                  {
                      const long st = at + (long) std::floor (s * 0.005 * SR + 0.5);
                      for (long i = 0; i < SR * 0.5 && st + i < (long) x.size(); ++i)
                      {
                          const double t = i / SR;
                          double v = 0;
                          for (int h = 1; h <= 6; ++h) v += (std::sin (2 * pi * fs[s] * h * t) * std::exp (-t * (3 + 2 * h))) / h;
                          add (x, st + i, g * (0.15 * v * std::min ({ 1.0, t / 0.0005, (0.5 - t) / 0.1 }) + (i < SR * 0.0015 ? 0.06 * (rr() - 0.5) : 0)));
                      }
                  }
              } },
            // a kick drum: a low thump dropping in pitch
            { "kick", [] (std::vector<float>& x, long at, double g, int)
              {
                  double ph = 0;
                  for (long i = 0; i < SR * 0.35 && at + i < (long) x.size(); ++i)
                  {
                      const double t = i / SR;
                      ph += (2 * pi * (50 + 90 * std::exp (-t / 0.03))) / SR;
                      add (x, at + i, g * 0.8 * std::exp (-t / 0.12) * std::sin (ph) * std::min (1.0, t / 0.001));
                  }
              } },
        };
        return list;
    }

    const Play& instrument (const char* name)
    {
        for (const auto& [n, p] : instruments())
            if (std::string (n) == name) return p;
        return instruments()[0].second;
    }

    std::vector<double> onsetsOf (const std::vector<float>& x)
    {
        vs::OnsetDetector d (SR);
        std::vector<double> out;
        for (size_t i = 0; i < x.size(); i += 1024)
            d.push (x.data() + i, (int) std::min<size_t> (1024, x.size() - i), (std::int64_t) i + 7777, out);
        for (auto& o : out) o -= 7777;
        return out;
    }

    void onsets()
    {
        for (const auto& [name, play] : instruments())
        {
            // 8th notes at 100 BPM (300 ms apart), each pushed a few ms off, with dynamics (0 to -6 dB)
            std::vector<double> truth;
            auto x = noiseFloor ((size_t) SR * 9, 0.0008, 5);
            const int dyn[7] { 0, 3, 6, 2, 5, 1, 4 };
            for (int k = 0; k < 24; ++k)
            {
                const long at = (long) std::floor ((0.5 + k * 0.3 + (((k * 37) % 11) - 5) * 0.003) * SR + 0.5);
                truth.push_back ((double) at);
                play (x, at, std::pow (10.0, -dyn[k % 7] / 20.0), k);
            }
            const auto got = onsetsOf (x);
            double worst = 0;
            for (double t : truth)
            {
                double best = 1e30;
                for (double o : got) best = std::min (best, std::abs (o - t));
                worst = std::max (worst, best / SR * 1000);
            }
            expect (std::string ("onsets: ") + name + ": every note within 2 ms, nothing extra",
                    got.size() == truth.size() && worst < 2, std::to_string (got.size()) + " found, worst " + num (worst) + " ms");
        }
        // a softer note right after a loud one that is still ringing
        for (const char* name : { "drum", "piano", "strum" })
        {
            int softest = 0;
            for (int db : { 6, 9, 12, 15, 18 })
            {
                auto x = noiseFloor ((size_t) SR * 2, 0.0008, 5);
                instrument (name) (x, (long) (0.3 * SR), 1, 0);
                instrument (name) (x, (long) (0.6 * SR), std::pow (10.0, -db / 20.0), 3);
                const auto got = onsetsOf (x);
                if (got.size() == 2 && std::abs (got[1] - 0.6 * SR) < 0.003 * SR) softest = db;
                else break;
            }
            expect (std::string ("onsets: a ") + name + " note 6 dB softer under a ringing one is heard", softest >= 6,
                    std::to_string (softest) + " dB");
        }
        // a held note with vibrato (5 Hz, 30 cents) and tremolo (4 Hz, 3 dB) is one note at most
        auto v = noiseFloor ((size_t) SR * 4, 0.0008, 3);
        double ph = 0;
        for (size_t i = 0; i < v.size(); ++i)
        {
            const double t = (double) i / SR;
            ph += (2 * pi * 330 * std::pow (2.0, (0.3 / 12) * std::sin (2 * pi * 5 * t))) / SR;
            v[i] += (float) (0.3 * std::min ({ 1.0, t / 0.2, (4 - t) / 0.2 }) * std::pow (10.0, 0.15 * std::sin (2 * pi * 4 * t))
                             * (std::sin (ph) + 0.4 * std::sin (2 * ph) + 0.2 * std::sin (3 * ph)));
        }
        expect ("onsets: a held note with vibrato and tremolo is one note at most", onsetsOf (v).size() <= 1);
        expect ("onsets: steady noise alone is no note", onsetsOf (noiseFloor ((size_t) SR * 3, 0.003, 8)).empty());
        // the same onsets whatever the block size, and a dropout carried over
        auto x = noiseFloor ((size_t) SR * 3, 0.0008, 9);
        for (int k = 0; k < 6; ++k) instrument ("drum") (x, (long) ((0.4 + k * 0.4) * SR), 0.8, k);
        std::vector<double> a, b;
        vs::OnsetDetector da (SR), db (SR);
        for (size_t i = 0; i < x.size(); i += 1024) da.push (x.data() + i, (int) std::min<size_t> (1024, x.size() - i), (std::int64_t) i, a);
        for (size_t i = 0; i < x.size(); i += 97) db.push (x.data() + i, (int) std::min<size_t> (97, x.size() - i), (std::int64_t) i, b);
        expect ("onsets: the same for any block size", a == b && a.size() == 6);
    }

    // ---- latency

    // a click as a small speaker plays it: a short bright burst (no lows)
    void clickInto (std::vector<float>& x, long at, double g)
    {
        double lp = 0;
        for (long i = 0; i < SR * 0.03 && at + i < (long) x.size(); ++i)
        {
            const double t = i / SR;
            const double c = std::exp (-t / 0.004) * (std::sin (2 * pi * 1500 * t) + 0.5 * std::sin (2 * pi * 3700 * t));
            lp += 0.5 * (c - lp);
            add (x, at + i, g * lp);
            if (at + i + 288 < (long) x.size()) add (x, at + i + 288, g * 0.27 * lp);   // a reflection 6 ms later
        }
    }

    void latency()
    {
        std::vector<double> clicks;
        for (int i = 0; i < 12; ++i) clicks.push_back (0.5 * SR + i * 0.6 * SR);
        auto room = [&] (double delayMs, std::vector<long> claps)
        {
            auto x = noiseFloor ((size_t) SR * 8, 0.002, 4);
            for (double c : clicks) clickInto (x, (long) std::floor (c + delayMs * SR / 1000 + 0.5), 0.3);
            for (long c : claps) instrument ("clap") (x, c, 0.5, 0);
            return onsetsOf (x);
        };
        using vs::timing::measureLatency;
        auto o1 = room (42.3, {});
        auto r1 = measureLatency (clicks.data(), (int) clicks.size(), o1.data(), (int) o1.size(), SR);
        expect ("latency: a 42.3 ms round trip", r1.ok && std::abs (r1.ms - 42.3) <= 0.5, num (r1.ms) + " ms");
        auto o2 = room (187.6, { (long) (2.1 * SR), (long) (4.4 * SR) });
        auto r2 = measureLatency (clicks.data(), (int) clicks.size(), o2.data(), (int) o2.size(), SR);
        expect ("latency: 187.6 ms, with two stray claps", r2.ok && std::abs (r2.ms - 187.6) <= 0.5, num (r2.ms) + " ms");
        auto o3 = onsetsOf (noiseFloor ((size_t) SR * 8, 0.002, 4));
        auto r3 = measureLatency (clicks.data(), (int) clicks.size(), o3.data(), (int) o3.size(), SR);
        expect ("latency: says so when it hears nothing", ! r3.ok && r3.matched == 0 && r3.total == 12);
        std::vector<double> claps;
        for (int i = 0; i < 12; ++i) claps.push_back (std::floor (clicks[(size_t) i] + ((60 + (((i * 5) % 9) - 4)) * SR) / 1000 + 0.5));
        auto rc = measureLatency (clicks.data(), (int) clicks.size(), claps.data(), (int) claps.size(), SR, -0.1, 0.45, 25);
        expect ("latency: clapping along, the middle of slightly-off claps", rc.ok && std::abs (rc.ms - 60) <= 2, num (rc.ms) + " ms");
    }

    // ---- the timing check, end to end: a player against a grid, through the detector, to the report

    struct Take
    {
        std::vector<vs::timing::Hit> hits;
        int extra = 0;
        int played = 0;
        vs::timing::TimeReport report;
    };

    // 4/4 at bpm after a bar of count-in; `subs` positions per count (straight); gapPlay/gapMute: bars
    // with the click then silent bars (0: no gap). err (count, sub, bar, gap) -> ms off, or NAN for no note.
    Take take (double bpm, int subs, double latencyMs, int gapPlay, int gapMute, const char* inst,
               const std::function<double (int, int, int, bool, vs::Mulberry32&)>& err)
    {
        const double seconds = 40, len = SR * 60 / bpm;
        std::vector<vs::timing::Count> counts;
        for (int i = 0;; ++i)
        {
            vs::timing::Count c;
            c.frame = 0.3 * SR + i * len;
            if (c.frame > (seconds - 1) * SR) break;
            c.len = len;
            c.bar = i / 4 - 1;
            c.count = i % 4;
            c.level = c.count == 0 ? 3 : 2;
            c.countIn = c.bar < 0;
            c.gap = gapMute > 0 && c.bar >= 0 && c.bar % (gapPlay + gapMute) >= gapPlay;
            c.subdivided = subs > 1;
            c.bpm = bpm;
            counts.push_back (c);
        }
        std::vector<double> fr;
        for (int k = 0; k < subs; ++k) fr.push_back ((double) k / subs);
        auto x = noiseFloor ((size_t) (SR * seconds), 0.0008, 11);
        vs::Mulberry32 r (5);
        Take t;
        for (const auto& c : counts)
        {
            if (c.countIn) continue;
            for (int k = 0; k < subs; ++k)
            {
                const double e = err (c.count, k, c.bar, c.gap, r);
                if (std::isnan (e)) continue;
                instrument (inst) (x, (long) std::floor (c.frame + fr[(size_t) k] * c.len + ((latencyMs + e) * SR) / 1000 + 0.5), 0.6 + 0.4 * r.uniform(), ++t.played);
            }
        }
        vs::timing::TimingCheck tc (SR, latencyMs);
        vs::OnsetDetector det (SR);
        size_t ci = 0;
        std::vector<double> found;
        for (size_t f = 0; f < x.size(); f += 1024)
        {
            while (ci < counts.size() && counts[ci].frame < (double) f + 1024) tc.addCount (counts[ci++], fr.data(), (int) fr.size());
            found.clear();
            det.push (x.data() + f, (int) std::min<size_t> (1024, x.size() - f), (std::int64_t) f, found);
            tc.addOnsets (found.data(), (int) found.size());
        }
        tc.resolve (true);
        t.hits = tc.matched();
        t.extra = tc.extras();
        t.report = vs::timing::summarize (t.hits);
        return t;
    }

    const vs::timing::PositionReport* position (const vs::timing::TimeReport& r, int count, int sub)
    {
        for (const auto& p : r.positions)
            if (p.count == count && p.sub == sub) return &p;
        return nullptr;
    }

    void timingCheck()
    {
        // 4/4 at 100 with 8ths: beat 4 rushed 14 ms, the & of 2 dragged 9 ms, a few ms of human wobble
        auto a = take (100, 2, 37.5, 0, 0, "drum", [] (int c, int k, int, bool, vs::Mulberry32& r)
                       { return (c == 3 && k == 0 ? -14.0 : c == 1 && k == 1 ? 9.0 : 0.0) + (r.uniform() - 0.5) * 6; });
        expect ("timing: every note matched to its place, nothing extra", (int) a.hits.size() == a.played && a.extra == 0,
                std::to_string (a.hits.size()) + " of " + std::to_string (a.played));
        const auto* p4 = position (a.report, 3, 0);
        const auto* p2 = position (a.report, 1, 1);
        expect ("timing: beat 4 rushed by 14 ms, the & of 2 dragged by 9", p4 && p2 && std::abs (p4->mean + 14) <= 2 && std::abs (p2->mean - 9) <= 2,
                p4 && p2 ? num (p4->mean) + ", " + num (p2->mean) : "");
        expect ("timing: those are the two odd positions, beat 4 first", a.report.oddPositions.size() == 2
                    && &a.report.positions[(size_t) a.report.oddPositions[0]] == p4 && &a.report.positions[(size_t) a.report.oddPositions[1]] == p2);
        expect ("timing: a score between 60 and 90, no click bleed", a.report.score > 60 && a.report.score < 90 && ! a.report.bleed,
                std::to_string (a.report.score));
        expect ("timing: 8 positions, drift measured", a.report.positions.size() == 8 && a.report.hasTrend && std::abs (a.report.trendChange) < 8);

        // gap click: 2 bars on, 2 silent; in the silent bars the player speeds up (each count 2 ms earlier)
        auto g = take (120, 1, 20, 2, 2, "clap", [] (int c, int, int bar, bool gap, vs::Mulberry32& r)
                       {
                           if (! gap) return (r.uniform() - 0.5) * 4;
                           const int i = ((bar % 4) - 2) * 4 + c;
                           return -2.0 * (i + 1) + (r.uniform() - 0.5) * 2;
                       });
        expect ("timing: speeding up in the silent bars is caught", g.report.hasGap && g.report.gap.endErr < -12 && g.report.gap.endErr > -20,
                num (g.report.gap.endErr) + " ms");
        expect ("timing: with the tempo it drifted to", g.report.gap.playedBpm > 120.3 && g.report.gap.bpm == 120, num (g.report.gap.playedBpm) + " BPM");

        // the mic hearing the click: too perfect
        auto b = take (100, 1, 30, 0, 0, "drum", [] (int, int, int, bool, vs::Mulberry32&) { return 0.1; });
        expect ("timing: a click bleeding into the mic is flagged", b.report.bleed);

        // a few notes are not a report; the count-in is left out
        std::vector<vs::timing::Hit> few (3);
        for (auto& h : few) h.err = 5;
        vs::timing::Hit ci;
        ci.countIn = true;
        few.push_back (ci);
        const auto f = vs::timing::summarize (few);
        expect ("timing: under 4 notes after the count-in, no report yet", ! f.enough && f.n == 3 && f.positions.empty());

        // a note too far from any position is extra; two on one position keep the closer
        vs::timing::TimingCheck tc (SR, 0);
        vs::timing::Count c;
        c.len = SR / 2;
        c.bpm = 120;
        const double zero = 0;
        for (int i = 0; i < 4; ++i) { c.frame = i * c.len; c.count = i; tc.addCount (c, &zero, 1); }
        const double ons[] { 10, 9000, 24000 + 300, 24000 - 100 };
        tc.addOnsets (ons, 4);
        tc.resolve (true);
        expect ("timing: a stray note is extra, two on one beat keep the closer", tc.matched().size() == 2 && tc.extras() == 2
                    && std::abs (tc.matched()[1].err - (-100 * 1000 / SR)) < 1e-12);
    }
}

int main()
{
    taps();
    tempoFinder();
    onsets();
    latency();
    timingCheck();
    std::printf ("\n%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
