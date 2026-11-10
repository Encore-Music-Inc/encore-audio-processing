// The building blocks: FFT, windows, random generators, statistics, linear
// prediction, Butterworth sections and cascades, oversampling, the
// Linkwitz-Riley crossover, the state-variable filter and convolution. Built without JUCE, as the browser build is. Each
// check prints PASS or FAIL; the suite exits non-zero on any failure.
#include <cmath>
#include <algorithm>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>
#include "dsp/Fft.h"
#include "dsp/Windows.h"
#include "dsp/Random.h"
#include "dsp/Stats.h"
#include "dsp/Lpc.h"
#include "dsp/Biquad.h"
#include "dsp/Oversampler.h"
#include "dsp/Crossover.h"
#include "dsp/Svf.h"
#include "dsp/Convolution.h"

namespace
{
    int failures = 0;
    constexpr double pi = 3.141592653589793;

    void expect (const char* name, bool ok, double value = 0.0)
    {
        std::printf ("%s %-66s %g\n", ok ? "PASS" : "FAIL", name, value);
        if (! ok) ++failures;
    }

    std::vector<double> noise (int n, unsigned seed)
    {
        std::mt19937 r (seed);
        std::uniform_real_distribution<double> u (-1.0, 1.0);
        std::vector<double> x ((size_t) n);
        for (auto& v : x) v = u (r);
        return x;
    }

    void fftTests()
    {
        // against a plain DFT
        double worst = 0.0;
        for (int n : { 1, 2, 4, 8, 64, 512 })
        {
            auto re = noise (n, 1), im = noise (n, 2);
            std::vector<std::complex<double>> want ((size_t) n);
            for (int k = 0; k < n; ++k)
                for (int i = 0; i < n; ++i)
                    want[(size_t) k] += std::complex<double> (re[(size_t) i], im[(size_t) i]) * std::polar (1.0, -2.0 * pi * k * i / n);
            vs::fftFor (n).forward (re.data(), im.data());
            for (int k = 0; k < n; ++k) worst = std::max (worst, std::abs (want[(size_t) k] - std::complex<double> (re[(size_t) k], im[(size_t) k])));
        }
        expect ("Fft: forward matches the DFT (largest difference)", worst < 1e-11, worst);

        // inverse undoes forward
        {
            const int n = 4096;
            auto re = noise (n, 3), im = noise (n, 4);
            const auto re0 = re, im0 = im;
            const vs::Fft f (n);
            f.forward (re.data(), im.data());
            f.inverse (re.data(), im.data());
            double e = 0.0;
            for (int i = 0; i < n; ++i) e = std::max ({ e, std::abs (re[(size_t) i] - re0[(size_t) i]), std::abs (im[(size_t) i] - im0[(size_t) i]) });
            expect ("Fft: inverse (forward (x)) == x", e < 1e-13, e);
        }

        // real transform: the same bins as the complex one, and back
        for (int n : { 2, 4, 16, 1024, 16384 })
        {
            auto x = noise (n, 5);
            std::vector<double> re = x, im ((size_t) n, 0.0);
            vs::fftFor (n).forward (re.data(), im.data());
            vs::RealFft rf (n);
            std::vector<double> r2 ((size_t) n / 2 + 1), i2 ((size_t) n / 2 + 1), back ((size_t) n);
            rf.forward (x.data(), r2.data(), i2.data());
            double e = 0.0;
            for (int k = 0; k <= n / 2; ++k) e = std::max ({ e, std::abs (r2[(size_t) k] - re[(size_t) k]), std::abs (i2[(size_t) k] - im[(size_t) k]) });
            rf.inverse (r2.data(), i2.data(), back.data());
            double eb = 0.0;
            for (int i = 0; i < n; ++i) eb = std::max (eb, std::abs (back[(size_t) i] - x[(size_t) i]));
            char name[96];
            std::snprintf (name, sizeof (name), "RealFft %d: bins match the complex FFT, and back", n);
            expect (name, e < 1e-10 && eb < 1e-13, std::max (e, eb));
        }

        // vDSP runs the transforms on Apple systems (unless VS_FFT_PORTABLE),
        // and nowhere else
        expect ("Fft: vDSP on Apple systems only", vs::fftFor (1024).usesAccelerate() == (VS_FFT_ACCELERATE == 1) && vs::RealFft (1024).usesAccelerate() == (VS_FFT_ACCELERATE == 1)
                                                && ! vs::Fft (1024, true).usesAccelerate() && ! vs::RealFft (1024, true).usesAccelerate());
       #if VS_FFT_ACCELERATE
        // vDSP against the code here, every size: the largest difference
        // against the largest bin (forward) or sample (inverse)
        {
            double worst = 0.0;
            for (int l = 1; l <= 20; ++l)
            {
                const int n = 1 << l;
                const vs::Fft fa (n), fp (n, true);
                auto re = noise (n, 20 + (unsigned) l), im = noise (n, 40 + (unsigned) l);
                auto ra = re, ia = im, rp = re, ip = im;
                fa.forward (ra.data(), ia.data());
                fp.forward (rp.data(), ip.data());
                double big = 0.0, d = 0.0;
                for (int k = 0; k < n; ++k)
                {
                    big = std::max (big, std::hypot (rp[(size_t) k], ip[(size_t) k]));
                    d = std::max (d, std::hypot (ra[(size_t) k] - rp[(size_t) k], ia[(size_t) k] - ip[(size_t) k]));
                }
                worst = std::max (worst, d / big);
                fa.inverse (ra.data(), ia.data());
                fp.inverse (rp.data(), ip.data());
                d = 0.0;
                for (int k = 0; k < n; ++k) d = std::max ({ d, std::abs (ra[(size_t) k] - rp[(size_t) k]), std::abs (ia[(size_t) k] - ip[(size_t) k]) });
                worst = std::max (worst, d); // the samples are at most 1

                vs::RealFft qa (n), qp (n, true);
                const auto x = noise (n, 60 + (unsigned) l);
                const int b = n / 2 + 1;
                std::vector<double> ar ((size_t) b), ai ((size_t) b), pr ((size_t) b), pi_ ((size_t) b), ya ((size_t) n), yp ((size_t) n);
                qa.forward (x.data(), ar.data(), ai.data());
                qp.forward (x.data(), pr.data(), pi_.data());
                big = 0.0; d = 0.0;
                for (int k = 0; k < b; ++k)
                {
                    big = std::max (big, std::hypot (pr[(size_t) k], pi_[(size_t) k]));
                    d = std::max (d, std::hypot (ar[(size_t) k] - pr[(size_t) k], ai[(size_t) k] - pi_[(size_t) k]));
                }
                worst = std::max (worst, d / big);
                qa.inverse (pr.data(), pi_.data(), ya.data());
                qp.inverse (pr.data(), pi_.data(), yp.data());
                d = 0.0;
                for (int k = 0; k < n; ++k) d = std::max (d, std::abs (ya[(size_t) k] - yp[(size_t) k]));
                worst = std::max (worst, d);
            }
            expect ("Fft and RealFft: vDSP agrees with the code here (2 to 2^20)", worst < 1e-12, worst);
        }
       #endif
    }

    void windowTests()
    {
        const auto hp = vs::windows::hannPeriodic (8), hs = vs::windows::hannSymmetric (9);
        expect ("Hann periodic: 0 at the start, 1 in the middle", hp[0] == 0.0 && std::abs (hp[4] - 1.0) < 1e-15);
        expect ("Hann symmetric: 0 at both ends, 1 in the middle", hs[0] == 0.0 && std::abs (hs[8]) < 1e-15 && std::abs (hs[4] - 1.0) < 1e-15);
        {
            // periodic Hann overlapped by half sums to 1
            const int n = 256; const auto w = vs::windows::hannPeriodic (n);
            double e = 0.0;
            for (int i = 0; i < n / 2; ++i) e = std::max (e, std::abs (w[(size_t) i] + w[(size_t) i + n / 2] - 1.0));
            expect ("Hann periodic: overlap-add at half is flat", e < 1e-15, e);
        }
        const auto t = vs::windows::tukey (101);
        expect ("Tukey (0.5): flat over the middle half, tapered to 0", t[0] == 0.0 && t[100] == 0.0 && t[25] == 1.0 && t[75] == 1.0 && t[12] > 0.4 && t[12] < 0.6);
        const auto bh = vs::windows::blackmanHarris (65);
        expect ("Blackman-Harris: symmetric, 1 in the middle, ~0 at the ends",
                std::abs (bh[0] - bh[64]) < 1e-15 && std::abs (bh[32] - 1.0) < 1e-12 && bh[0] < 1e-4);
        {
            // the classic Blackman (alpha 0.16): 0 at the start, 1 in the middle,
            // mirror-symmetric about it, and its mean is a0 = 0.42
            const int n = 2048; const auto w = vs::windows::blackman (n);
            double sum = 0.0, asym = 0.0;
            for (int i = 0; i < n; ++i) sum += w[(size_t) i];
            for (int i = 1; i < n / 2; ++i) asym = std::max (asym, std::abs (w[(size_t) i] - w[(size_t) (n - i)]));
            expect ("Blackman (0.16): 0 at the start, 1 in the middle, mean 0.42, periodic",
                    std::abs (w[0]) < 1e-15 && std::abs (w[(size_t) n / 2] - 1.0) < 1e-15 && std::abs (sum / n - 0.42) < 1e-12 && asym < 1e-14);
            const auto w2 = vs::windows::blackman (8, 0.0);
            expect ("Blackman (alpha 0) is the periodic Hann", std::abs (w2[2] - vs::windows::hannPeriodic (8)[2]) < 1e-15);
        }
        expect ("Bessel I0 (0) = 1, I0 (1) = 1.2660658777520082", vs::windows::besselI0 (0.0) == 1.0 && std::abs (vs::windows::besselI0 (1.0) - 1.2660658777520082) < 1e-15);
        const auto k = vs::windows::kaiser (31, 8.0);
        expect ("Kaiser: 1 in the middle, 1/I0 (beta) at the ends", std::abs (k[15] - 1.0) < 1e-15 && std::abs (k[0] - 1.0 / vs::windows::besselI0 (8.0)) < 1e-15);
        const auto& s = vs::sincTable();
        expect ("SincTable: 1 at 0, 0 at whole crossings and past the end",
                s.at (0.0) == 1.0 && std::abs (s.at (3.0)) < 1e-12 && s.at (64.0) == 0.0 && s.at (-200.0) == 0.0 && s.at (-0.5) == s.at (0.5));
    }

    void randomTests()
    {
        vs::Xorshift32 x; vs::Mulberry32 m (42); vs::Lcg32 l (7); vs::ParkMiller p (22222);
        const bool xs = x.next() == 1359758873u && x.next() == 3761132862u && x.next() == 2075758394u;
        const bool ms = m.next() == 2581720956u && m.next() == 1925393290u && m.next() == 3661312704u;
        const bool ls = l.next() == 1025555898u && l.next() == 3923423697u && l.next() == 2630631676u;
        const bool ps = p.next() == 373485154u && p.next() == 70283097u && p.next() == 132005429u;
        expect ("Xorshift32: the known sequence (default seed)", xs);
        expect ("Mulberry32: the known sequence (seed 42)", ms);
        expect ("Lcg32: the known sequence (seed 7)", ls);
        expect ("ParkMiller: the known sequence (seed 22222)", ps);
        vs::Xorshift32 z (0);
        expect ("Xorshift32: seed 0 runs as seed 1", z.next() == vs::Xorshift32 (1).next());
        double lo = 1.0, hi = 0.0;
        vs::Mulberry32 u (9);
        for (int i = 0; i < 100000; ++i) { const double v = u.uniform(); lo = std::min (lo, v); hi = std::max (hi, v); }
        expect ("uniform() stays in [0, 1)", lo >= 0.0 && hi < 1.0);
    }

    void statsTests()
    {
        const double a[] { 5, 1, 4, 2, 3 }, b[] { 4, 1, 3, 2 };
        expect ("median of an odd and an even count", vs::stats::median (a, 5) == 3.0 && vs::stats::median (b, 4) == 2.5);
        expect ("median of nothing is 0", vs::stats::median (a, 0) == 0.0);
        expect ("percentile: element floor (p n), kept in the array",
                vs::stats::percentile (a, 5, 0.0) == 1.0 && vs::stats::percentile (a, 5, 0.5) == 3.0 && vs::stats::percentile (a, 5, 1.0) == 5.0 && vs::stats::percentile (a, 5, 0.39) == 2.0);
        auto x = noise (1001, 6);
        auto c = x;
        expect ("medianInPlace (quickselect) agrees with sorting", vs::stats::medianInPlace (c.data(), 1001) == vs::stats::median (x.data(), 1001));
        double c4[] { 4, 1, 3, 2 };
        expect ("medianInPlace of an even count: the upper middle value", vs::stats::medianInPlace (c4, 4) == 3.0);
        const double v[] { 2, 4, 4, 4, 5, 5, 7, 9 };
        expect ("mean and standard deviation", vs::stats::mean (v, 8) == 5.0 && vs::stats::standardDeviation (v, 8) == 2.0);
        const double lx[] { 0, 1, 2, 3 }, ly[] { 1, 3, 5, 7 };
        const auto line = vs::stats::fitLine (lx, ly, 4);
        expect ("fitLine through y = 2x + 1", line.slope == 2.0 && line.at (10.0) == 21.0);
        const double same[] { 3, 3, 3 };
        expect ("fitLine with every x the same: slope 0", vs::stats::fitLine (same, ly, 3).slope == 0.0);
    }

    void lpcTests()
    {
        // an AR(2) resonance: x[n] = 1.6 x[n-1] - 0.8 x[n-2] + noise
        const int n = 20000;
        auto e = noise (n, 7);
        std::vector<double> x ((size_t) n, 0.0);
        for (int i = 2; i < n; ++i) x[(size_t) i] = 1.6 * x[(size_t) i - 1] - 0.8 * x[(size_t) i - 2] + 0.1 * e[(size_t) i];
        const auto w = vs::windows::hannPeriodic (n);
        double r[3], a[3];
        vs::lpc::windowedAutocorrelation (x.data(), n, 0, n, w.data(), 2, r);
        vs::lpc::predictor (r, 2, a);
        expect ("predictor finds an AR(2) process's coefficients", std::abs (a[1] - 1.6) < 0.02 && std::abs (a[2] + 0.8) < 0.02, a[1]);
        double f[3];
        vs::lpc::errorFilter (r, 2, f);
        const double off = std::max (std::abs (f[1] + a[1]), std::abs (f[2] + a[2]));
        expect ("errorFilter: 1 then the predictor negated (to the 1e-4 bias)", f[0] == 1.0 && off < 0.01, off);
        int orders = 0; double lastErr = 1e300; bool falling = true;
        vs::lpc::predictorsUpTo (r, 2, [&] (int, const double*, double err) { ++orders; falling = falling && err <= lastErr; lastErr = err; });
        expect ("predictorsUpTo: every order, error never rising", orders == 2 && falling);
        const double zero[] { 0, 0, 0 };
        int none = 0;
        vs::lpc::predictorsUpTo (zero, 2, [&] (int, const double*, double) { ++none; });
        double az[3];
        const double errZ = vs::lpc::predictor (zero, 2, az);
        expect ("silence: no predictors to choose from; the predictor is 0", none == 0 && az[1] == 0.0 && az[2] == 0.0 && errZ > 0.0);
        std::vector<double> pad ((size_t) 8, 1.0);
        double rp[2];
        const double ones[] { 1, 1, 1, 1 };
        vs::lpc::windowedAutocorrelation (pad.data(), 8, 6, 4, ones, 1, rp);
        expect ("windowedAutocorrelation: samples past the end count as 0", rp[0] == 2.0 && rp[1] == 1.0);
    }

    void biquadTests()
    {
        const double fs = 48000.0;
        const auto lp = vs::butterworth (vs::BiquadCoefs::Type::lowPass, fs, 1000.0, 8);
        const double at = vs::sectionsResponse (lp, 1000.0, fs), pass = vs::sectionsResponse (lp, 100.0, fs), stop = vs::sectionsResponse (lp, 4000.0, fs);
        expect ("butterworth 8: -3 dB at the cut-off", std::abs (20 * std::log10 (at) + 3.0103) < 0.01, 20 * std::log10 (at));
        expect ("butterworth 8: flat below, 48 dB/octave above", std::abs (20 * std::log10 (pass)) < 0.001 && 20 * std::log10 (stop) < -94.0, 20 * std::log10 (stop));
        // sectionsResponse agrees with magnitudeDb
        const auto pk = vs::BiquadCoefs::peak (fs, 2000.0, 1.0, 6.0);
        expect ("sectionsResponse agrees with magnitudeDb", std::abs (20 * std::log10 (vs::sectionsResponse ({ pk }, 2000.0, fs)) - pk.magnitudeDb (2000.0, fs)) < 1e-12);
        // the clamped designs are the unclamped ones at the kept frequency
        const auto a = vs::BiquadCoefs::lowShelf (fs, 2.0, 0.7, 3.0), b = vs::BiquadCoefs::rbj (vs::BiquadCoefs::Type::lowShelf, fs, 5.0, 0.7, 3.0);
        expect ("designs keep the frequency to 5 Hz .. 0.49 fs; rbj () doesn't", a.b0 == b.b0 && a.a2 == b.a2 && vs::BiquadCoefs::rbj (vs::BiquadCoefs::Type::lowShelf, fs, 2.0, 0.7, 3.0).b0 != a.b0);
        // a chain run in chunks gives the same as all at once, per channel
        auto x = noise (5000, 8);
        auto y1 = x, y2 = x;
        vs::BiquadChain one (lp, 2), chunks (lp, 2);
        one.process (1, y1.data(), 5000);
        for (int s = 0; s < 5000; s += 317) chunks.process (1, y2.data() + s, std::min (317, 5000 - s));
        expect ("BiquadChain: chunks carry the state across", y1 == y2);
        {
            // both channels at once, every section per sample: the same numbers
            bool same = true;
            for (int ns = 1; ns <= 5; ++ns)
            {
                std::vector<vs::BiquadCoefs> secs;
                for (int k = 0; k < ns; ++k) secs.push_back (vs::BiquadCoefs::rbj (k % 2 ? vs::BiquadCoefs::Type::highPass : vs::BiquadCoefs::Type::peak, 44100.0, 300.0 * (k + 1), 0.8, 4.0));
                vs::BiquadChain one (secs, 2), pair (secs, 2), mono (secs, 2);
                const auto na = noise (3000, 11), nb = noise (3000, 12);
                std::vector<float> a1 (na.begin(), na.end()), b1 (nb.begin(), nb.end());
                auto a2 = a1, b2 = b1, a3 = a1;
                for (int s0 = 0; s0 < 3000; s0 += 701)
                {
                    const int len = std::min (701, 3000 - s0);
                    one.process (0, a1.data() + s0, len);
                    one.process (1, b1.data() + s0, len);
                    pair.processPair (a2.data() + s0, b2.data() + s0, len);
                    mono.processPair (a3.data() + s0, (float*) nullptr, len);
                }
                same = same && a1 == a2 && b1 == b2 && a1 == a3;
            }
            expect ("BiquadChain: two channels at once give the same numbers", same);
        }
    }

    void oversamplerTests()
    {
        // the same output as a plain FIR over the zero-stuffed signal (which is
        // what the ring buffers and skipped zeros must reproduce), for the
        // default filter and the limiter's longer one. Compilers add the terms
        // in different orders, so it's the same to float rounding: the error
        // against the sum of the terms' sizes.
        for (const auto [numTaps, beta] : { std::pair { 63, 8.0 }, std::pair { 127, 10.0 } })
        {
            vs::Oversampler<float> os (1, 1, numTaps, beta);
            os.initProcessing (256);
            const int n = 256, taps = os.getNumTaps();
            std::vector<float> h ((size_t) taps);
            {
                // the taps, by sending a click through
                vs::Oversampler<float> probe (1, 1, numTaps, beta);
                probe.initProcessing (256);
                std::vector<float> c ((size_t) 256, 0.0f); c[0] = 0.5f; // x2 inside: a unit impulse into the filter
                const float* in[1] { c.data() };
                auto* const* up = probe.processSamplesUp (in, 1, 256);
                for (int k = 0; k < taps; ++k) h[(size_t) k] = up[0][k];
            }
            {
                // half-band: every tap an even distance from the centre (but the centre) is exactly 0
                bool zeros = true, others = true;
                for (int k = 0; k < taps; ++k)
                {
                    const int m = k - (taps - 1) / 2;
                    if (m != 0 && m % 2 == 0) zeros = zeros && h[(size_t) k] == 0.0f;
                    else others = others && h[(size_t) k] != 0.0f;
                }
                char zname[96];
                std::snprintf (zname, sizeof (zname), "Oversampler (%d taps): the half-band's zero taps are exactly 0", taps);
                expect (zname, zeros && others);
            }
            auto src = noise (n, 9);
            std::vector<float> x (src.begin(), src.end());
            const float* in[1] { x.data() };
            auto* const* up = os.processSamplesUp (in, 1, n);
            double worst = 0.0;
            for (int i = 0; i < 2 * n; ++i)
            {
                double acc = 0.0, size = 0.0;
                for (int k = 0; k < taps; ++k)
                {
                    const int j = i - k;
                    const double term = (double) h[(size_t) k] * ((j >= 0 && (j & 1) == 0) ? x[(size_t) (j / 2)] * 2.0 : 0.0);
                    acc += term;
                    size += std::abs (term);
                }
                if (size > 0.0) worst = std::max (worst, std::abs (acc - (double) up[0][i]) / size);
            }
            char name[96];
            std::snprintf (name, sizeof (name), "Oversampler up (%d taps): a plain FIR over the zero-stuffed signal", taps);
            expect (name, worst < 1.0e-6, worst);
        }

        // passband and level: a 1 kHz sine at 48 kHz through 2x up and down comes out the same, delayed
        vs::Oversampler<float> os4 (2, 1);
        os4.initProcessing (512);
        const double fs = 48000.0;
        const int len = 4800;
        std::vector<float> l ((size_t) len), r ((size_t) len);
        for (int i = 0; i < len; ++i) l[(size_t) i] = r[(size_t) i] = (float) (0.5 * std::sin (2 * pi * 1000.0 * i / fs));
        const auto ref = l;
        float* ch[2] { l.data(), r.data() };
        for (int s = 0; s < len; s += 480)
        {
            float* b[2] { ch[0] + s, ch[1] + s };
            os4.processSamplesUp (b, 2, 480);
            os4.processSamplesDown (b, 2, 480);
        }
        const int lat = (int) std::round (os4.getLatencyInSamples());
        double e = 0.0;
        for (int i = 1000; i < len; ++i) e = std::max (e, (double) std::abs (l[(size_t) i] - ref[(size_t) (i - lat)]));
        expect ("Oversampler 2x up and down: 1 kHz unchanged, by the stated latency", lat == 31 && e < 1e-3, e);
        expect ("Oversampler 4x: latency 31 + 15.5 samples", vs::Oversampler<float> (1, 2).getLatencyInSamples() == 46.5f);
        expect ("Oversampler 4x, 127 taps: latency 63 + 31.5 samples", vs::Oversampler<float> (1, 2, 127, 10.0).getLatencyInSamples() == 94.5f);

        // images: 2x up of a 15 kHz sine at 48 kHz, nothing left at 33 kHz
        vs::Oversampler<float> os2 (1, 1);
        os2.initProcessing (8192);
        std::vector<float> s ((size_t) 8192);
        for (int i = 0; i < 8192; ++i) s[(size_t) i] = (float) std::sin (2 * pi * 15000.0 * i / fs);
        const float* sp[1] { s.data() };
        auto* const* u = os2.processSamplesUp (sp, 1, 8192);
        auto mag = [&] (double f) { std::complex<double> acc; for (int i = 4096; i < 16384; ++i) acc += (0.5 - 0.5 * std::cos (2 * pi * (i - 4096) / 12288.0)) * (double) u[0][i] * std::polar (1.0, -2 * pi * f * i / (2 * fs)); return std::abs (acc); };
        const double imageDb = 20 * std::log10 (mag (33000.0) / mag (15000.0));
        expect ("Oversampler 2x: the image of 15 kHz at 33 kHz is gone (dB)", imageDb < -60.0, imageDb);
    }

    void crossoverTests()
    {
        // low + high = all-pass: same level at every frequency
        const double fs = 48000.0;
        double worst = 0.0;
        for (double f : { 50.0, 150.0, 1000.0, 3000.0, 12000.0 })
        {
            vs::LinkwitzRiley<float> lo, hi, ap;
            for (auto* x : { &lo, &hi, &ap }) { x->prepare (fs); x->setCutoffFrequency (1000.0f); }
            lo.setType (vs::LinkwitzRiley<float>::Type::lowPass);
            hi.setType (vs::LinkwitzRiley<float>::Type::highPass);
            ap.setType (vs::LinkwitzRiley<float>::Type::allPass);
            double sumSq = 0.0, inSq = 0.0, apErr = 0.0;
            for (int i = 0; i < 48000; ++i)
            {
                const float x = (float) std::sin (2 * pi * f * i / fs);
                const float y = lo.processSample (0, x) + hi.processSample (0, x), z = ap.processSample (0, x);
                if (i > 24000) { sumSq += (double) y * y; inSq += (double) x * x; apErr = std::max (apErr, (double) std::abs (y - z)); }
            }
            worst = std::max ({ worst, std::abs (10 * std::log10 (sumSq / inSq)), apErr });
        }
        expect ("LinkwitzRiley: low + high is flat and equals the all-pass", worst < 0.01, worst);
        vs::LinkwitzRiley<float> lp;
        lp.prepare (fs); lp.setCutoffFrequency (1000.0f);
        double sq = 0.0;
        for (int i = 0; i < 48000; ++i) { const float y = lp.processSample (0, (float) std::sin (2 * pi * 1000.0 * i / fs)); if (i > 24000) sq += (double) y * y; }
        const double atDb = 10 * std::log10 (sq / 12000.0);
        expect ("LinkwitzRiley: -6 dB at the cut-off", std::abs (atDb + 6.02) < 0.05, atDb);
    }

    void svfTests()
    {
        // the Butterworth high-pass: -3 dB at the cut-off, -24 dB two octaves below, flat above
        const double fs = 48000.0;
        auto levelAt = [&] (double f)
        {
            vs::StateVariableFilter<float> hp;
            hp.prepare (fs, 1);
            hp.setType (vs::StateVariableFilter<float>::Type::highPass);
            hp.setResonance (0.70710678f);
            hp.setCutoffFrequency (4000.0f);
            double sq = 0.0, in = 0.0;
            for (int i = 0; i < 48000; ++i)
            {
                const float x = (float) std::sin (2 * pi * f * i / fs), y = hp.processSample (0, x);
                if (i > 24000) { sq += (double) y * y; in += (double) x * x; }
            }
            return 10 * std::log10 (sq / in);
        };
        const double atCut = levelAt (4000.0), twoBelow = levelAt (1000.0), above = levelAt (16000.0);
        expect ("StateVariableFilter: high-pass -3 dB at the cut-off", std::abs (atCut + 3.01) < 0.05, atCut);
        expect ("StateVariableFilter: high-pass -24 dB two octaves below", twoBelow < -24.0 && twoBelow > -25.0, twoBelow);
        expect ("StateVariableFilter: high-pass flat well above", std::abs (above) < 0.1, above);
    }

    void convolutionTests()
    {
        // against the direct sum, for impulse responses shorter and longer
        // than a partition and blocks of every size up to the largest
        double worst = 0.0;
        unsigned seed = 10;
        for (int len : { 1, 7, 300, 2048, 5000 })
            for (int maxBlock : { 16, 128, 200, 512 })
            {
                const auto h = noise (len, seed++), x = noise (12000, seed++);
                std::vector<float> ir (h.begin(), h.end()), l (x.begin(), x.end()), r ((size_t) x.size(), 0.0f);
                vs::Convolution c;
                c.loadImpulseResponse ({ ir });
                c.prepare (48000.0, maxBlock);
                std::mt19937 rng (seed);
                for (size_t i = 0; i < l.size();)
                {
                    const int n = (int) std::min<size_t> ((size_t) std::uniform_int_distribution<int> (1, maxBlock) (rng), l.size() - i);
                    float* ch[2] { l.data() + i, r.data() + i };
                    c.process (ch, 2, n);
                    i += (size_t) n;
                }
                double peak = 0.0, err = 0.0;
                for (size_t i = 0; i < l.size(); ++i)
                {
                    double want = 0.0;
                    for (size_t k = 0; k < ir.size() && k <= i; ++k) want += (double) ir[k] * (double) (float) x[i - k];
                    err = std::max ({ err, std::abs (l[i] - want), (double) std::abs (r[i]) });
                    peak = std::max (peak, std::abs (want));
                }
                worst = std::max (worst, err / std::max (peak, 1.0));
            }
        expect ("Convolution: the direct sum to float rounding, no latency", worst < 1e-5, worst);

        // a new response crossfades in over 50 ms: no jump bigger than the signal's own steps
        {
            const double fs = 48000.0;
            vs::Convolution c;
            std::vector<float> a (512, 0.0f), b (512, 0.0f);
            a[0] = 1.0f;
            b[0] = -1.0f;
            c.loadImpulseResponse ({ a });
            c.prepare (fs, 256);
            std::vector<float> l (256 * 190), r (256 * 190);
            for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = (float) std::sin (2 * pi * 100.0 * i / fs);
            for (size_t i = 0; i < l.size(); i += 256)
            {
                if (i == 256 * 40) c.loadImpulseResponse ({ b });
                float* ch[2] { l.data() + i, r.data() + i };
                c.process (ch, 2, 256);
            }
            double jump = 0.0;
            for (size_t i = 1; i < l.size(); ++i) jump = std::max (jump, (double) std::abs (l[i] - l[i - 1]));
            const size_t swapAt = 256 * 40, fadeEnd = swapAt + 2400;
            const bool flipped = std::abs (l[fadeEnd + 100] + (float) std::sin (2 * pi * 100.0 * (fadeEnd + 100) / fs)) < 1e-5f;
            const bool before = std::abs (l[swapAt - 1] - (float) std::sin (2 * pi * 100.0 * (swapAt - 1) / fs)) < 1e-5f;
            const bool middle = std::abs (l[swapAt + 1200]) < 0.02f + std::abs ((float) std::sin (2 * pi * 100.0 * (swapAt + 1200) / fs)) * 0.01f;
            expect ("Convolution: a new response crossfades in over 50 ms", flipped && before && middle && jump < 0.02, jump);
        }
    }
}

int main()
{
    fftTests();
    windowTests();
    randomTests();
    statsTests();
    lpcTests();
    biquadTests();
    oversamplerTests();
    crossoverTests();
    svfTests();
    convolutionTests();
    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
