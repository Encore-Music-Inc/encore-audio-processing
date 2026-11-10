// The processing pieces built without JUCE, as the browser build is: the
// resampler, fractional delay and decimator, float to integers with dither,
// channel maps, trims, fades and normalizing, the playback device filters, the
// vocal EQ and the loudness-matched "hear the original". Each check prints PASS
// or FAIL; the suite exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include "dsp/AbCompare.h"
#include "dsp/Channels.h"
#include "dsp/DeviceSim.h"
#include "dsp/Dither.h"
#include "dsp/FracDelay.h"
#include "dsp/Resampler.h"
#include "dsp/VocalEq.h"

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

    std::vector<float> sine (int n, double fs, double hz, double amp = 0.5)
    {
        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i) x[(size_t) i] = (float) (amp * std::sin (twoPi * hz * i / fs));
        return x;
    }

    std::vector<float> noise (int n, unsigned seed, float amp = 0.5f)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<float> u (-amp, amp);
        std::vector<float> x ((size_t) n);
        for (auto& v : x) v = u (rng);
        return x;
    }

    double rms (const float* x, int n) { double s = 0; for (int i = 0; i < n; ++i) s += (double) x[i] * x[i]; return std::sqrt (s / std::max (1, n)); }

    // the amplitude and phase of frequency hz in x (a single-bin DFT)
    double amplitudeAt (const float* x, int n, double fs, double hz)
    {
        double re = 0, im = 0;
        for (int i = 0; i < n; ++i) { re += x[i] * std::cos (twoPi * hz * i / fs); im -= x[i] * std::sin (twoPi * hz * i / fs); }
        return 2 * std::sqrt (re * re + im * im) / n;
    }

    // ---------- resampler, fractional delay, decimation
    void resampling()
    {
        // 1 kHz at 44.1 kHz to 48 kHz: same frequency and level, right length
        {
            const int n = 44100;
            auto x = sine (n, 44100, 1000);
            const int want = vs::resampledLength (n, 44100, 48000);
            std::vector<float> y ((size_t) want);
            const float* in[1] { x.data() }; float* out[1] { y.data() };
            vs::resample (in, 1, n, 44100, 48000, out);
            const double a = amplitudeAt (y.data() + 4800, 38400, 48000, 1000);
            expect ("resampling 44.1 to 48 kHz keeps a 1 kHz sine's level and pitch", want == 48000 && std::abs (a - 0.5) < 0.001, "(%.0f samples, amplitude %.5f)", want, a);
        }
        // downsampling filters what would alias: 20 kHz at 48 kHz to 22.05 kHz
        {
            const int n = 48000;
            auto x = sine (n, 48000, 20000);
            std::vector<float> y ((size_t) vs::resampledLength (n, 48000, 22050));
            const float* in[1] { x.data() }; float* out[1] { y.data() };
            vs::resample (in, 1, n, 48000, 22050, out);
            const double r = rms (y.data() + 2000, (int) y.size() - 4000);
            expect ("downsampling removes what would fold back (20 kHz into 22.05 kHz)", r < 1e-3, "(rms %.2g)", r);
        }
        // the output doesn't depend on how the input is cut into blocks
        {
            const int n = 30000;
            auto x = noise (n, 1), x2 = noise (n, 2);
            auto run = [&] (std::vector<int> blocks)
            {
                vs::Resampler r (96000.0 / 44100.0, 2);
                std::vector<float> out;
                size_t b = 0;
                for (int s = 0; s < n;)
                {
                    const int m = std::min (blocks[b++ % blocks.size()], n - s);
                    const float* in[2] { x.data() + s, x2.data() + s };
                    const int k = r.push (in, m);
                    out.insert (out.end(), r.output (1), r.output (1) + k);
                    s += m;
                }
                const int k = r.flush();
                out.insert (out.end(), r.output (1), r.output (1) + k);
                return out;
            };
            const auto a = run ({ 30000 }), b = run ({ 1, 777, 4096, 13 });
            expect ("the resampler's output is the same however the input is cut", a == b, "(%.0f samples)", (double) a.size());
        }
        // a varying step (a wow fix) reads the input where it says: step 1 is a copy
        {
            const int n = 5000;
            auto x = sine (n, 48000, 440);
            vs::Resampler r ([] (double) { return 1.0; }, 1.0, 1);
            const float* in[1] { x.data() };
            std::vector<float> y;
            int k = r.push (in, n); y.insert (y.end(), r.output (0), r.output (0) + k);
            k = r.flush(); y.insert (y.end(), r.output (0), r.output (0) + k);
            double worst = 0;
            for (int i = 200; i < n - 200; ++i) worst = std::max (worst, (double) std::abs (y[(size_t) i] - x[(size_t) i]));
            expect ("a step of 1 passes a 440 Hz sine through, in line", (int) y.size() == n && worst < 2e-3, "(largest difference %.2g)", worst);
        }
    }

    void delays()
    {
        const int n = 8000;
        auto x = sine (n, 48000, 1000);
        // whole-sample delays are plain copies, with latency for the negative one
        {
            vs::FracDelay d ({ 3.0, -2.0 });
            auto a = x, b = x;
            d.process (0, a.data(), n); d.process (1, b.data(), n);
            bool ok = d.latency() == 2;
            for (int i = 5; i < n; ++i) ok = ok && a[(size_t) i] == x[(size_t) (i - 5)];
            for (int i = 0; i < n; ++i) ok = ok && b[(size_t) i] == x[(size_t) i];
            expect ("whole-sample delays are copies; a negative one sets the latency", ok, "(latency %.0f)", d.latency());
        }
        // a fractional delay shifts a sine by exactly that much (latency included)
        {
            vs::FracDelay d ({ 0.25 });
            auto a = x;
            for (int s = 0; s < n; s += 1000) d.process (0, a.data() + s, 1000);
            const double total = d.latency() + 0.25;
            double worst = 0;
            for (int i = 200; i < n; ++i) worst = std::max (worst, std::abs (a[(size_t) i] - 0.5 * std::sin (twoPi * 1000 * (i - total) / 48000)));
            expect ("a 0.25-sample delay shifts a 1 kHz sine by latency + 0.25", d.latency() == 63 && worst < 1e-4, "(latency %.0f, largest error %.2g)", d.latency(), worst);
        }
    }

    void decimation()
    {
        // the decimator keeps every factor-th sample, whatever the blocks
        {
            const int n = 20000;
            auto x = noise (n, 3);
            auto run = [&] (int block)
            {
                vs::Decimator d (44100, 4);
                std::vector<float> out, tmp ((size_t) d.maxOutput (block));
                for (int s = 0; s < n; s += block)
                {
                    const int k = d.push (x.data() + s, std::min (block, n - s), tmp.data());
                    out.insert (out.end(), tmp.begin(), tmp.begin() + k);
                }
                return out;
            };
            const auto a = run (n), b = run (37);
            expect ("the decimator's output is the same however the input is cut", a == b && (int) a.size() == n / 4, "(%.0f samples)", (double) a.size());
        }
        // its filter stops what would alias: 9 kHz at 44.1 kHz decimated by 4
        {
            const int n = 44100;
            auto x = sine (n, 44100, 9000);
            vs::Decimator d (44100, 4);
            std::vector<float> y ((size_t) d.maxOutput (n));
            const int k = d.push (x.data(), n, y.data());
            const double r = rms (y.data() + 1000, k - 1000);
            expect ("decimating by 4 removes 9 kHz (above the new Nyquist)", r < 1e-3, "(rms %.2g)", r);
        }
        // the low-band copy: floor (n / d) samples, a low tone kept
        {
            const int n = 44101;
            auto x = sine (n, 44100, 100);
            std::vector<float> y ((size_t) (n / 7 + 1));
            const int k = vs::decimateLowBand (x.data(), n, 44100, 7, y.data());
            const double a = amplitudeAt (y.data() + 1000, k - 1000, 44100.0 / 7, 100);
            expect ("the low-band copy has floor (n / d) samples and keeps 100 Hz", k == n / 7 && std::abs (a - 0.5) < 0.01, "(%.0f samples, amplitude %.4f)", k, a);
        }
    }

    // ---------- float to integers
    void quantizing()
    {
        // 16-bit audio written at 16 bits comes back exactly (no dither added)
        {
            std::vector<float> x (1000);
            std::mt19937 rng (5);
            for (auto& v : x) v = (float) ((int) (rng() % 65536) - 32768) / 32768.0f;
            const float* in[1] { x.data() };
            std::vector<std::int32_t> q (1000); std::int32_t* out[1] { q.data() };
            vs::Quantizer Q (16);
            Q.process (in, 1, 1000, out);
            bool ok = ! Q.isDithering() && vs::Quantizer::onGrid (in, 1, 1000, 16);
            for (int i = 0; i < 1000; ++i) ok = ok && q[(size_t) i] == (std::int32_t) std::lround (x[(size_t) i] * 32768.0);
            expect ("audio already on the 16-bit grid is written bit for bit", ok);
        }
        // full scale: -1 is -32768, +1 clips to 32767, halves round upward
        {
            const float x[] { -1.0f, 1.0f, 1.5f / 32768, -1.5f / 32768, -0.5f / 32768 };
            const float* in[1] { x };
            std::int32_t q[5]; std::int32_t* out[1] { q };
            vs::Quantizer Q (16, vs::Quantizer::Dither::never);
            Q.process (in, 1, 5, out);
            expect ("full scale is 2^(bits - 1), clipped, halves rounded upward", q[0] == -32768 && q[1] == 32767 && q[2] == 2 && q[3] == -1 && q[4] == 0,
                    "(%.0f %.0f %.0f ...)", q[0], q[1], q[2]);
        }
        // off-grid audio gets triangular dither: never more than 1 LSB from the plain rounding, mean error 0
        {
            auto x = noise (48000, 6, 0.9f);
            const float* in[1] { x.data() };
            std::vector<std::int32_t> q (48000); std::int32_t* out[1] { q.data() };
            vs::Quantizer Q (16);
            Q.process (in, 1, 48000, out);
            double worst = 0, mean = 0;
            for (int i = 0; i < 48000; ++i) { const double e = q[(size_t) i] - x[(size_t) i] * 32768.0; worst = std::max (worst, std::abs (e)); mean += e; }
            mean /= 48000;
            expect ("off-grid audio is dithered: within 1.5 LSB, unbiased", Q.isDithering() && worst <= 1.5 && std::abs (mean) < 0.02, "(largest %.3f, mean %.4f LSB)", worst, mean);
        }
        // streaming: one quantizer over blocks carries the dither, and 24-bit PCM bytes are little-endian
        {
            auto x = noise (4800, 7);
            vs::Quantizer whole (24), parts (24);
            const float* in[2] { x.data(), x.data() };
            std::vector<std::uint8_t> a (4800 * 2 * 3), b (a.size());
            whole.processToPcm (in, 2, 4800, a.data());
            for (int s = 0; s < 4800; s += 480) { const float* p[2] { x.data() + s, x.data() + s }; parts.processToPcm (p, 2, 480, b.data() + s * 6); }
            const int first = (int) (a[0] | (a[1] << 8) | (a[2] << 16) | (a[2] & 0x80 ? 0xff000000u : 0u));
            const bool near = std::abs (first - x[0] * 8388608.0) <= 1.5;
            int differ = 0;
            for (size_t i = 0; i < a.size(); i += 3) differ += a[i] != b[i];
            expect ("24-bit PCM is little-endian; block by block dithers the same way", near && differ > 0 && b.size() == a.size(), "(first sample %.0f)", first);
        }
    }

    // ---------- channels
    void channelWork()
    {
        const int n = 1000;
        std::vector<std::vector<float>> six;
        for (int c = 0; c < 6; ++c) six.push_back (std::vector<float> ((size_t) n, 0.1f * (c + 1)));
        std::vector<const float*> in;
        for (auto& c : six) in.push_back (c.data());
        std::vector<float> o1 ((size_t) n), o2 ((size_t) n);
        float* out[2] { o1.data(), o2.data() };
        vs::channels::map (vs::channels::Map::stereo, in.data(), 6, n, out);
        const double l = 0.1 + 0.707107 * (0.3 + 0.5), r = 0.2 + 0.707107 * (0.3 + 0.6);
        expect ("5.1 folds down to L + 0.707 (C + Ls), LFE left out", std::abs (o1[0] - l) < 1e-5 && std::abs (o2[0] - r) < 1e-5, "(%.5f %.5f)", o1[0], o2[0]);
        vs::channels::map (vs::channels::Map::mono, in.data(), 2, n, out);
        expect ("mono is the average of the channels", std::abs (o1[0] - 0.15f) < 1e-7, "(%.6f)", o1[0]);
        expect ("stereo, left, right and mono make 2, 1, 1 and 1 channels",
                vs::channels::mappedChannels (vs::channels::Map::stereo, 6) == 2 && vs::channels::mappedChannels (vs::channels::Map::mono, 6) == 1
                && vs::channels::mappedChannels (vs::channels::Map::keep, 6) == 6);

        const auto t = vs::channels::trimRange (48000, 48000, 0.25, 0.5), all = vs::channels::trimRange (48000, 48000, 0, 0);
        expect ("trimming 0.25 to 0.5 s keeps samples 12000 to 24000", t.first == 12000 && t.last == 24000 && all.first == 0 && all.last == 48000);

        std::vector<float> f ((size_t) n, 1.0f);
        float* fp[1] { f.data() };
        vs::channels::fades (fp, 1, n, 1000, 0.1, 0.1);
        expect ("fades start and end at silence and are sin (pi/2 x) shaped", f[0] == 0.0f && f[(size_t) n - 1] == 0.0f && std::abs (f[50] - std::sin (3.14159265 / 4)) < 1e-6 && f[500] == 1.0f,
                "(%.6f at the middle of the fade)", f[50]);

        // the other curves: sin^2 is 0.5 halfway, linear is t, and "end" leaves no sample silent
        {
            std::vector<float> g ((size_t) n, 1.0f), h ((size_t) n, 1.0f);
            float* gp[1] { g.data() }; float* hp[1] { h.data() };
            vs::channels::fades (gp, 1, n, 1000, 0.1, 0, vs::channels::FadeCurve::sineSquared);
            vs::channels::fadeSamples (hp, 1, n, 0, 100, vs::channels::FadeCurve::linear, vs::channels::FadePoint::end);
            expect ("sin^2 fades are half gain halfway; linear end-point fades never reach 0", std::abs (g[50] - 0.5f) < 1e-6 && g[0] == 0.0f && h[(size_t) n - 1] == 0.01f && h[(size_t) n - 50] == 0.5f,
                    "(%.6f, last sample %.3f)", g[50], h[(size_t) n - 1]);
        }
        // an equal-power crossfade keeps the power of two unrelated signals
        {
            auto a = noise (48000, 21), b = noise (48000, 22);
            const double before = rms (a.data(), 48000);
            float* ap[1] { a.data() }; const float* bp[1] { b.data() };
            vs::channels::crossfade (ap, bp, 1, 48000);
            const double mid = rms (a.data() + 20000, 8000);
            expect ("an equal-power crossfade holds the level through the join", std::abs (20 * std::log10 (mid / before)) < 0.2, "(%.3f dB at the middle)", 20 * std::log10 (mid / before));
        }

        // normalize: peak to -1 dBFS; LUFS to -14 unless the peak would pass -1 dBFS
        {
            auto x = sine (48000 * 4, 48000, 997, 0.25);
            float* p[1] { x.data() };
            const auto res = vs::channels::normalize (p, 1, (int) x.size(), 48000, vs::channels::Normalize::peak);
            const float pk = vs::channels::peakOf (p, 1, (int) x.size());
            expect ("peak normalizing lands the sample peak at -1 dBFS", std::abs (20 * std::log10 (pk) + 1) < 1e-4, "(%.4f dBFS, gain %.3f dB)", 20 * std::log10 (pk), res.gainDb);
        }
        {
            auto l = noise (48000 * 4, 8, 0.1f), r = noise (48000 * 4, 9, 0.1f);
            float* p[2] { l.data(), r.data() };
            const auto res = vs::channels::normalize (p, 2, (int) l.size(), 48000, vs::channels::Normalize::lufs);
            const float after = vs::channels::loudnessOf (p, 2, (int) l.size(), 48000);
            expect ("LUFS normalizing lands quiet noise at -14 LUFS", res.outcome == vs::channels::NormalizeResult::applied && std::abs (after + 14) < 0.05, "(%.2f LUFS)", after);
        }
        {
            std::vector<float> x ((size_t) 48000 * 4, 0.0f);
            for (int i = 0; i < 48000 * 4; i += 4800) x[(size_t) i] = 0.5f; // clicks: loud peaks, very little loudness
            float* p[1] { x.data() };
            const auto res = vs::channels::normalize (p, 1, (int) x.size(), 48000, vs::channels::Normalize::lufs);
            const float pk = vs::channels::peakOf (p, 1, (int) x.size());
            expect ("LUFS normalizing stops at -1 dBFS peak when that comes first", res.outcome == vs::channels::NormalizeResult::peakLimited && std::abs (20 * std::log10 (pk) + 1) < 1e-4,
                    "(stopped at %.1f LUFS)", res.reachedLufs);
        }
    }

    // ---------- playback devices
    void devices()
    {
        vs::DeviceSim sim;
        sim.prepare (48000);
        auto l = noise (4800, 10), r = noise (4800, 11);
        auto l0 = l, r0 = r;
        sim.process (l.data(), r.data(), 4800);
        expect ("studio monitors change nothing", l == l0 && r == r0);

        sim.setDevice (vs::device::find ("phone"));
        sim.process (l.data(), r.data(), 4800);
        expect ("the phone plays mono: both sides the same", l == r);
        // the specification's highpass takes Q in dB: at its frequency the gain is Q dB
        vs::device::Filter hp { vs::device::FilterType::highpass, 450, 0.9 };
        const double at = vs::device::design (hp, 48000).magnitudeDb (450, 48000);
        expect ("a Web Audio highpass with Q 0.9 is +0.9 dB at its frequency", std::abs (at - 0.9) < 1e-6, "(%.6f dB)", at);
        vs::device::Filter shelf { vs::device::FilterType::lowshelf, 120, 1, 2 };
        const double lowEnd = vs::device::design (shelf, 48000).magnitudeDb (10, 48000), mid = vs::device::design (shelf, 48000).magnitudeDb (120, 48000);
        expect ("a Web Audio low shelf of +2 dB: +2 at the bottom, +1 at its frequency", std::abs (lowEnd - 2) < 0.02 && std::abs (mid - 1) < 1e-6, "(%.3f, %.6f dB)", lowEnd, mid);
        expect ("five devices: studio, phone, laptop, earbuds, car", vs::device::devices().size() == 5 && vs::device::find ("car") == 4);
    }

    // ---------- vocal EQ
    void vocalEq()
    {
        vs::VocalEq eq;
        vs::VocalEq::Settings s;
        s.lowCutHz = 100; s.lowDb = 6; s.presenceDb = -4;
        eq.setSettings (s);
        eq.prepare (48000);
        const double cut = eq.magnitudeDb (100), boost = eq.magnitudeDb (40), pres = eq.magnitudeDb (3500);
        expect ("the low cut is -3 dB at its frequency (plus the low shelf there)", std::abs (cut - (-3.01 + vs::EqBand::magnitudeDb ({ true, vs::EqBand::lowShelf, 110, 6, 0.7f }, 100, 48000))) < 0.02, "(%.3f dB)", cut);
        expect ("the presence bell is -4 dB at 3.5 kHz", std::abs (pres + 4) < 0.05, "(%.3f dB, %.1f dB at 40 Hz)", pres, boost);

        // a sudden change glides: +12 dB on the body bell reaches the output over a few blocks, not at once
        auto x = sine (48000, 48000, 350, 0.25);
        float* p[1] { x.data() };
        vs::VocalEq a;
        a.prepare (48000);
        a.process (p, 1, 24000);
        vs::VocalEq::Settings loud; loud.bodyDb = 12;
        a.setSettings (loud);
        float* q[1] { x.data() + 24000 };
        a.process (q, 1, 24000);
        double jump = 0;
        for (int i = 24001; i < 24400; ++i) jump = std::max (jump, (double) std::abs (x[(size_t) i] - x[(size_t) i - 1]));
        const double natural = 0.25 * 3.981 * twoPi * 350 / 48000; // the largest step of the boosted sine itself
        const double gainAfter = 20 * std::log10 (amplitudeAt (x.data() + 36000, 9600, 48000, 350) / 0.25);
        expect ("+12 dB on the body bell glides in (no jump) and arrives", jump <= natural * 1.01 && std::abs (gainAfter - 12) < 0.05,
                "(largest step %.4f, the sine's own %.4f; %.2f dB after)", jump, natural, gainAfter);
    }

    // ---------- loudness-matched compare
    void compare()
    {
        const double fs = 48000;
        const int n = (int) fs * 6, block = 512, latency = 100;
        auto in = noise (n, 12, 0.3f);
        // the "processing": 6 dB quieter and `latency` samples late
        std::vector<float> delayLine ((size_t) latency, 0.0f);
        vs::AbCompare ab;
        ab.prepare (fs, block, latency);
        ab.setHearOriginal (true);
        std::vector<float> out ((size_t) n);
        int d = 0;
        for (int s = 0; s < n; s += block)
        {
            std::vector<float> buf (in.begin() + s, in.begin() + s + block);
            ab.takeOriginal (buf.data(), nullptr, block);
            for (auto& v : buf) { const float y = delayLine[(size_t) d] * 0.5f; delayLine[(size_t) d] = v; d = (d + 1) % latency; v = y; }
            ab.mix (buf.data(), nullptr, block);
            std::copy (buf.begin(), buf.end(), out.begin() + s);
        }
        // after the meters fill (3 s) and the gain glides (0.5 s), the original plays 6 dB down, in line
        double worst = 0;
        for (int i = (int) fs * 5; i < n; ++i) worst = std::max (worst, (double) std::abs (out[(size_t) i] - 0.5f * in[(size_t) (i - latency)]));
        expect ("hearing the original plays it in line, at the processed loudness", worst < 0.003 && std::abs (ab.matchGainDb() + 6.02) < 0.05, "(match %.2f dB, largest difference %.4f)", ab.matchGainDb(), worst);

        // switching back crossfades over 30 ms
        ab.setHearOriginal (false);
        std::vector<float> ones (2000, 1.0f), zeros (2000, 0.0f);
        ab.takeOriginal (zeros.data(), nullptr, 2000);
        ab.mix (ones.data(), nullptr, 2000);
        const int ramp = (int) std::floor (0.03 * fs);
        expect ("switching back fades over 30 ms", ones[0] < 0.01f && ones[(size_t) ramp / 2] > 0.4f && ones[(size_t) ramp / 2] < 0.6f && ones[(size_t) ramp] == 1.0f,
                "(%.3f halfway)", ones[(size_t) ramp / 2]);
    }
}

int main()
{
    resampling();
    delays();
    decimation();
    quantizing();
    channelWork();
    devices();
    vocalEq();
    compare();
    std::printf ("%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
