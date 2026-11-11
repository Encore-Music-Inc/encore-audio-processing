// The processing pieces built without JUCE, as the browser build is: the
// resampler, fractional delay and decimator, float to integers with dither,
// channel maps, trims, fades and normalizing, the playback device filters, the
// vocal EQ, the loudness-matched "hear the original", DSD to PCM and the
// mastering chain's true-peak limiter on its own. Each check prints PASS or
// FAIL; the suite exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include "dsp/AbCompare.h"
#include "dsp/Channels.h"
#include "dsp/DeviceSim.h"
#include "dsp/Dither.h"
#include "dsp/Dsd.h"
#include "dsp/FracDelay.h"
#include "dsp/Loudness.h"
#include "dsp/MasterChain.h"
#include "dsp/Resampler.h"
#include "dsp/TruePeak.h"
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

        // the devices' lowpass and highpass Qs are plain Qs: at its corner each reads 20 log10 (Q), and none rises above 0 dB
        // more than a plain Q's own bump (Q 0.7 none, 0.8 0.21 dB, 0.9 0.69 dB)
        for (const auto& d : vs::device::devices())
            for (const auto& f : d.filters)
            {
                if (f.type != vs::device::FilterType::lowpass && f.type != vs::device::FilterType::highpass) continue;
                const double q = std::pow (10.0, f.q / 20), corner = vs::device::design (f, 48000).magnitudeDb (f.frequency, 48000);
                double top = -1e9;
                for (double hz = 10; hz < 23000; hz *= 1.001) top = std::max (top, vs::device::design (f, 48000).magnitudeDb (hz, 48000));
                const double bump = q * q > 0.5 ? 20 * std::log10 (q * q / std::sqrt (q * q - 0.25)) : 0.0;
                char name[120];
                std::snprintf (name, sizeof (name), "%s %s at %.0f Hz: Q %.1f, no resonance beyond that Q's", d.id,
                               f.type == vs::device::FilterType::lowpass ? "lowpass" : "highpass", f.frequency, q);
                expect (name, std::abs (corner - 20 * std::log10 (q)) < 1e-6 && top < bump + 0.01, "(corner %.3f dB, top %.3f dB)", corner, top);
            }
        // the phone: the highpass at 450 Hz no longer lifts the low mids
        sim.setDevice (vs::device::find ("phone"));
        expect ("the phone is flat (within 0.1 dB) at 500 Hz", std::abs (sim.magnitudeDb (500)) < 0.1, "(%.3f dB)", sim.magnitudeDb (500));
    }

    // ---------- DSD to PCM

    // a second-order one-bit delta-sigma modulator, most significant bit first
    std::vector<std::uint8_t> modulate (const std::vector<double>& x)
    {
        std::vector<std::uint8_t> out (x.size() / 8, 0);
        double i1 = 0, i2 = 0, y = 0;
        for (size_t k = 0; k < out.size() * 8; ++k)
        {
            i1 += x[k] - y;
            i2 += i1 - y;
            const bool bit = i2 >= 0;
            y = bit ? 1 : -1;
            if (bit) out[k >> 3] = (std::uint8_t) (out[k >> 3] | (0x80 >> (k & 7)));
        }
        return out;
    }

    void dsd()
    {
        using namespace vs::dsd;
        expect ("DSD64 and DSD128 to 88.2 kHz, DSD256 and DSD512 to 176.4 kHz", outputRate (2822400) == 88200 && outputRate (5644800) == 88200
                && outputRate (11289600) == 176400 && outputRate (22579200) == 176400);
        expect ("bytes per sample: 4 (DSD64), 8 (DSD128, DSD256), 16 (DSD512); 0 for 3.072 MHz", bytesPerSample (2822400) == 4 && bytesPerSample (5644800) == 8
                && bytesPerSample (11289600) == 8 && bytesPerSample (22579200) == 16 && bytesPerSample (3072000) == 0 && ! Decoder (3072000, 2).ok());
        expect ("reverseBits: 0x01 to 0x80, 0xC5 to 0xA3", reverseBits (0x01) == 0x80 && reverseBits (0xC5) == 0xA3);
        const auto f = makeFilter (2822400);
        double dc = 0;
        for (int k = 0; k < f.bytes; ++k) dc += f.table[(size_t) (k * 256 + 255)];
        expect ("the DSD64 filter: 128 bytes of taps that sum to 1 (all ones read 1)", f.bytes == 128 && std::abs (dc - 1) < 1e-5, "(%.7f)", dc);

        for (double rate : { 2822400.0, 5644800.0, 11289600.0 })
        {
            // a 1 kHz sine at half scale, and 20 kHz (kept) beside it
            const int n = (int) (rate * 0.5);
            std::vector<double> x ((size_t) n);
            for (int i = 0; i < n; ++i) x[(size_t) i] = 0.5 * std::sin (twoPi * 1000 * i / rate) + 0.1 * std::sin (twoPi * 20000 * i / rate);
            const auto bits = modulate (x);
            const auto pcm = toPcm (bits.data(), (int) bits.size(), rate);
            const double fs = outputRate (rate);
            const int expectLen = ((int) bits.size() - makeFilter (rate).bytes) / bytesPerSample (rate) + 1;
            const int from = (int) (fs * 0.1), len = (int) (fs * 0.3);
            const double a1 = amplitudeAt (pcm.data() + from, len, fs, 1000), a20 = amplitudeAt (pcm.data() + from, len, fs, 20000);
            // what is left once both tones are taken out (20 kHz at the level it came out: DSD256's filter, half as long in
            // time as DSD64's, starts its slope lower): the modulator's noise below 30 kHz and the filter's leftovers
            std::vector<float> rest ((size_t) len);
            const double delay = (makeFilter (rate).bytes * 8 - 1) / 2.0 / rate;   // the linear-phase filter's delay: output i is the input around i / fs + delay
            for (int i = 0; i < len; ++i)
            {
                const double t = (from + i) / fs + delay;
                rest[(size_t) i] = (float) (pcm[(size_t) (from + i)] - 0.5 * std::sin (twoPi * 1000 * t) - a20 * std::sin (twoPi * 20000 * t));
            }
            const double restDb = 20 * std::log10 (rms (rest.data(), len) + 1e-12);
            char name[120];
            std::snprintf (name, sizeof (name), "DSD at %.4f MHz: 1 kHz and 20 kHz come through at their levels, in time", rate / 1e6);
            expect (name, (int) pcm.size() == expectLen && std::abs (a1 - 0.5) < 0.005 && std::abs (20 * std::log10 (a20 / 0.1)) < 0.5 && restDb < -60,
                    "(%.4f, %.4f, rest %.1f dB)", a1, a20, restDb);

            // streaming in uneven pushes gives the same samples as all at once
            Decoder d (rate, 2);
            std::vector<float> got;
            std::mt19937 rng (5);
            for (size_t s = 0; s < bits.size();)
            {
                const int k = (int) std::min<size_t> (bits.size() - s, 1 + rng() % 3000);
                const std::uint8_t* in[2] { bits.data() + s, bits.data() + s };
                const int made = d.push (in, k);
                got.insert (got.end(), d.output (1), d.output (1) + made);
                s += (size_t) k;
            }
            std::snprintf (name, sizeof (name), "DSD at %.4f MHz: pushed in pieces, the same samples", rate / 1e6);
            expect (name, got == pcm);
        }
        // the filter stops DSD's noise: 60 kHz (above the cut-off) is held more than 60 dB down
        {
            const double rate = 2822400;
            const int n = (int) (rate * 0.3);
            std::vector<double> x ((size_t) n);
            for (int i = 0; i < n; ++i) x[(size_t) i] = 0.4 * std::sin (twoPi * 60000 * i / rate);
            const auto pcm = toPcm (modulate (x).data(), n / 8, rate);
            const double a = amplitudeAt (pcm.data() + 8820, 17640, 88200, 88200 - 60000);   // 60 kHz folds to 28.2 kHz at 88.2 kHz
            expect ("DSD64: a 60 kHz tone is gone (more than 60 dB down)", 20 * std::log10 (a / 0.4 + 1e-12) < -60, "(%.1f dB)", 20 * std::log10 (a / 0.4 + 1e-12));
        }
        expect ("nothing comes out until the filter has its bytes", toPcm (std::vector<std::uint8_t> (127, 0x69).data(), 127, 2822400).empty()
                && toPcm (std::vector<std::uint8_t> (128, 0x69).data(), 128, 2822400).size() == 1);
    }

    // ---------- the mastering chain's true-peak limiter, on its own

    void limiter()
    {
        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            // a hot mix: a buzzy bass, noise and a high tone, peaking well over full scale
            const int n = (int) fs * 3;
            auto l = noise (n, 31, 0.6f), r = noise (n, 32, 0.6f);
            for (int i = 0; i < n; ++i)
            {
                const double t = i / fs, buzz = std::sin (twoPi * 110 * t) + 0.5 * std::sin (twoPi * 330 * t) + 0.3 * std::sin (twoPi * 5000 * t + 0.4);
                l[(size_t) i] += (float) (0.9 * buzz);
                r[(size_t) i] += (float) (0.8 * buzz);
            }
            auto meter = [] (const std::vector<float>& a, const std::vector<float>& b)
            {
                vs::TruePeakMeter m;
                m.prepare (2, 4096);
                for (size_t s = 0; s < a.size(); s += 4096)
                {
                    const float* ch[2] { a.data() + s, b.data() + s };
                    m.process (ch, (int) std::min<size_t> (4096, a.size() - s));
                }
                m.finish();
                return 20 * std::log10 ((double) m.truePeak (-1));
            };
            const double before = meter (l, r);

            // on its own, as the browser runs it (blocks of up to 4096)
            vs::TruePeakLimiter lim;
            lim.prepare (fs, 4096);
            lim.setCeilingDb (-1.0f);
            auto ol = l, orr = r;
            for (int s = 0; s < n; s += 4096)
            {
                float* ch[2] { ol.data() + s, orr.data() + s };
                juce::dsp::AudioBlock<float> b (ch, 2, (size_t) std::min (4096, n - s));
                lim.process (b);
            }
            const double after = meter (ol, orr);
            char name[120];
            std::snprintf (name, sizeof (name), "limiter at %.1f kHz: a hot mix held to -1 dBTP on the meter", fs / 1000);
            expect (name, before > 3 && after <= -1.0, "(%.2f dBTP in, %.3f dBTP out)", before, after);

            // the mastering chain's own limiter stage, fed the same input as the chain feeds it (its release
            // and ceiling set each block, in the chain's blocks), gives the same samples bit for bit
            vs::MasterChain chain;
            chain.prepare (fs, 512);
            const auto& style = vs::MasterChain::style (0);
            auto cl = l, cr = r;
            for (int s = 0; s < n; s += 512)
            {
                float* ch[2] { cl.data() + s, cr.data() + s };
                juce::dsp::AudioBlock<float> b (ch, 2, (size_t) std::min (512, n - s));
                chain.limiter.setCeilingDb (-1.0f);
                chain.limiter.setReleaseMs (style.releaseMs, fs);
                chain.limiter.process (b);
            }
            std::snprintf (name, sizeof (name), "limiter at %.1f kHz: the same samples as the chain's limiter stage", fs / 1000);
            expect (name, cl == ol && cr == orr && chain.limiter.getLatency() == lim.getLatency());
        }

        // one channel: the same as two identical channels
        const double fs = 48000;
        auto x = noise (48000, 41, 1.5f);
        vs::TruePeakLimiter mono, stereo;
        mono.prepare (fs, 4096); stereo.prepare (fs, 4096);
        mono.setCeilingDb (-0.5f); stereo.setCeilingDb (-0.5f);
        auto m = x, a = x, b = x;
        for (int s = 0; s < 48000; s += 4096)
        {
            const size_t k = (size_t) std::min (4096, 48000 - s);
            float* c1[1] { m.data() + s };
            float* c2[2] { a.data() + s, b.data() + s };
            juce::dsp::AudioBlock<float> b1 (c1, 1, k), b2 (c2, 2, k);
            mono.process (b1);
            stereo.process (b2);
        }
        expect ("limiter: one channel limits as two identical ones", m == a && a == b);
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
    dsd();
    limiter();
    std::printf ("%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
