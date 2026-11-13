// What a DAW's mixer and timeline lean on: pan laws, the volume fader's law,
// fade shapes, automation curves, the click voices a host schedules, the live
// resampler for clips at another rate or speed, the waveform picture, a fixed
// A/B match for two renders, levels and silence, the tempo
// range, drawing axes and the tuner's window. Built without JUCE. Each check
// prints PASS or FAIL; the suite exits non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include "dsp/AbCompare.h"
#include "dsp/Channels.h"
#include "dsp/Compressor.h"
#include "dsp/Denormals.h"
#include "dsp/LoudnessReport.h"
#include "dsp/Lpc.h"
#include "dsp/Windows.h"
#include "dsp/Meters.h"
#include "dsp/Metronome.h"
#include "dsp/Mix.h"
#include "dsp/Ramp.h"
#include "dsp/Resampler.h"
#include "dsp/Spectrum.h"
#include "dsp/Tempo.h"
#include "dsp/TunerPitch.h"
#include "dsp/Util.h"
#include "dsp/Waveform.h"

namespace
{
    int failures = 0;
    constexpr double twoPi = 6.283185307179586;

    void expect (const char* name, bool ok, const char* fmt = "", double a = 0, double b = 0, double c = 0)
    {
        std::printf ("%s %-70s ", ok ? "PASS" : "FAIL", name);
        std::printf (fmt, a, b, c);
        std::printf ("\n");
        if (! ok) ++failures;
    }

    bool near (double a, double b, double tol) { return std::abs (a - b) <= tol; }
}

int main()
{
    using namespace vs;

    // pan laws
    {
        const auto lin = mix::panGains (1.0f, 0.0f, mix::PanLaw::linear);
        const auto p3 = mix::panGains (1.0f, 0.0f, mix::PanLaw::centre3dB);
        const auto p6 = mix::panGains (1.0f, 0.0f, mix::PanLaw::centre6dB);
        const auto hardL = mix::panGains (0.5f, -1.0f, mix::PanLaw::centre3dB);
        expect ("linear pan: both sides at full gain in the centre", lin.left == 1.0f && lin.right == 1.0f);
        expect ("3 dB law: each side -3 dB in the centre", near (ampToDb (p3.left, 0), -3.01, 0.01) && p3.left == p3.right, "%.3f dB", ampToDb (p3.left, 0));
        expect ("6 dB law: each side -6 dB in the centre", near (ampToDb (p6.left, 0), -6.02, 0.01), "%.3f dB", ampToDb (p6.left, 0));
        expect ("hard left: all of the gain on the left", near (hardL.left, 0.5, 1e-6) && near (hardL.right, 0.0, 1e-6));
    }

    // the volume fader
    {
        using namespace meters;
        bool inverse = true;
        for (float db : { -60.0f, -12.0f, 0.0f, 6.0f }) inverse = inverse && near (faderPositionToDb (dbToFaderPosition (db)), db, 1e-4);
        expect ("fader law: position and dB convert both ways", inverse);
        expect ("fader law: the top is +6 dB, the bottom silent", near (faderPositionToDb (1.0f), 6.0, 1e-6) && faderPositionToGain (0.0f) == 0.0f
                                                                 && dbToFaderPosition (-100.0f) == 0.0f);
        expect ("fader law: gain and position agree with dB", near (faderPositionToGain (0.5f), dbToGain (faderPositionToDb (0.5f)), 1e-6));
    }

    // fade shapes
    {
        using namespace channels;
        bool ends = true, mirror = true, floats = true;
        for (int c = 0; c < 5; ++c)
        {
            const auto curve = (FadeCurve) c;
            ends = ends && near (fadeGain (curve, 0.0), 0.0, 1e-12) && near (fadeGain (curve, 1.0), 1.0, 1e-12);
            for (double t : { 0.1, 0.4, 0.7 })
            {
                mirror = mirror && near (fadeOutGain (curve, t), fadeGain (curve, 1.0 - t), 1e-12);
                floats = floats && near (fadeGainFloat (curve, t), fadeGain (curve, t), 1e-6);
            }
        }
        expect ("every fade shape runs from silence to full level", ends);
        expect ("every fade out is its fade in read backwards", mirror);
        expect ("the single-precision fades follow the same curves", floats);
        expect ("concave starts slow, convex starts fast", fadeGain (FadeCurve::concave, 0.25) < 0.25 && fadeGain (FadeCurve::quarterSine, 0.25) > 0.25);
        const auto x = crossfadeGains (0.5f);
        expect ("equal-power crossfade: -3 dB each in the middle", near (x.first, x.second, 1e-7) && near (ampToDb (x.first, 0), -3.01, 0.01));
    }

    // automation curves
    {
        expect ("a curve amount of 0 is a straight line", near (curve::valueAt (0.25, 0.0, 0.0f, 0.0f, 1.0, 1.0f), 0.25, 1e-7));
        const float bentUp = curve::valueAt (0.5, 0.0, 0.0f, 0.4f, 1.0, 1.0f), bentDown = curve::valueAt (0.5, 0.0, 0.0f, -0.4f, 1.0, 1.0f);
        expect ("a curve bends one way or the other", bentUp != bentDown && std::min (bentUp, bentDown) < 0.5f && std::max (bentUp, bentDown) > 0.5f,
                "%.3f / %.3f", bentUp, bentDown);
        bool inRange = true;
        for (float c : { -1.0f, -0.75f, 0.75f, 1.0f })
            for (double x = 0.0; x <= 1.0; x += 0.05) { const float v = curve::valueAt (x, 0.0, 0.2f, c, 1.0, 0.9f); inRange = inRange && v >= 0.2f - 1e-6f && v <= 0.9f + 1e-6f; }
        expect ("a strongly bent segment stays between its two values", inRange);
    }

    // click voices: a host's own schedule mixes like the click engine's
    {
        auto sound = std::make_shared<const metronome::Buffer> (metronome::Buffer { 1.0f, 0.5f, 0.25f });
        metronome::ClickVoices v;
        v.add (sound, 2, 0.5);
        v.add (sound, 6, 1.0);
        std::vector<float> a (4, 0.0f), b (4, 0.0f);
        v.mix (a.data(), 4, 0);
        v.mix (b.data(), 4, 4);
        expect ("click voices start on their frames and carry over blocks",
                a[2] == 0.5f && a[3] == 0.25f && b[0] == 0.125f && b[2] == 1.0f && b[3] == 0.5f && ! v.empty());
        std::vector<float> c (8, 0.0f);
        v.mix (c.data(), 8, 8);
        expect ("click voices end with their sounds", c[0] == 0.25f && v.empty());
    }

    // the live resampler
    {
        const double fsIn = 44100.0, fsOut = 48000.0, step = fsIn / fsOut;
        const int nIn = 44100;
        std::vector<float> in ((size_t) nIn);
        for (int i = 0; i < nIn; ++i) in[(size_t) i] = (float) (0.5 * std::sin (twoPi * 1000.0 * i / fsIn));
        LiveResampler r;
        r.prepare (1.0);
        std::vector<float> out;
        int at = 0;
        double owed = 0.0;
        const int block = 480;
        while (true)
        {
            owed += block * step;
            const int want = (int) owed;
            if (at + want > nIn) break;
            owed -= want;
            out.resize (out.size() + block);
            const int used = r.process (step, in.data() + at, want, out.data() + out.size() - block, block);
            at += used;
        }
        // amplitude and frequency in the steady middle
        double peak = 0.0;
        int crossings = 0;
        const size_t from = 4800, to = out.size() - 4800;
        for (size_t i = from; i < to; ++i)
        {
            peak = std::max (peak, (double) std::abs (out[i]));
            if (out[i - 1] < 0.0f && out[i] >= 0.0f) ++crossings;
        }
        const double hz = crossings * fsOut / (double) (to - from);
        expect ("live resampler: a 1 kHz tone at 44.1 kHz stays 1 kHz at 48 kHz", near (hz, 1000.0, 2.0), "%.1f Hz", hz);
        expect ("live resampler: and keeps its level", near (peak, 0.5, 0.005), "peak %.4f", peak);
        expect ("live resampler: its latency is its half width", r.latency() > 0 && r.latency() == (int) std::ceil (liveSincTable().halfWidth() / 0.97));
    }

    // the waveform picture: lowest and highest per block, read for any stretch
    {
        waveform::MinMax mm;
        mm.reset (2, 4);
        const float l[] { 0.1f, -0.5f, 0.2f, 0.0f, 0.9f, 0.3f }, r[] { 0, 0, 0, 0, 0, -0.7f };
        const float* ch[] { l, r };
        mm.add (ch, 2, 6);
        const auto a = mm.range (0, 0, 4), b = mm.range (0, 4, 6), all = mm.range (0, 0, 6), rr = mm.range (1, 0, 6);
        expect ("waveform: the lowest and highest of a block", a.lo == -0.5f && a.hi == 0.2f);
        expect ("waveform: a part-filled last block counts", b.lo == 0.3f && b.hi == 0.9f && all.lo == -0.5f && all.hi == 0.9f);
        expect ("waveform: each channel apart, and the peak of both", rr.lo == -0.7f && rr.hi == 0.0f && mm.peak() == 0.9f && mm.samples() == 6);
    }

    // a fixed A/B match: two renders compared after the fact
    {
        AbCompare ab;
        ab.prepare (48000.0, 512, 0);
        ab.setFixedMatch (-6.0f);
        std::vector<float> orig (512, 0.5f), procL (512, 0.5f), procR (512, 0.5f);
        for (int b = 0; b < 20; ++b)   // past the gain's glide
        {
            std::fill (procL.begin(), procL.end(), 0.5f); std::fill (procR.begin(), procR.end(), 0.5f);
            ab.takeOriginal (orig.data(), orig.data(), 512);
            ab.mix (procL.data(), procR.data(), 512);
        }
        expect ("fixed match: the processed version at its gain", near (procL[100], 0.5 * dbToGain (-6.0f), 1e-5), "%.4f", procL[100]);
        ab.setHearOriginal (true);
        for (int b = 0; b < 20; ++b)
        {
            std::fill (procL.begin(), procL.end(), 0.9f); std::fill (procR.begin(), procR.end(), 0.9f);
            ab.takeOriginal (orig.data(), orig.data(), 512);
            ab.mix (procL.data(), procR.data(), 512);
        }
        expect ("fixed match: the original as it is", near (procL[100], 0.5, 1e-6), "%.4f", procL[100]);
        AbCompare plain;
        plain.prepare (48000.0, 512, 0);
        plain.setNoMatch();
        std::fill (procL.begin(), procL.end(), 0.7f); std::fill (procR.begin(), procR.end(), 0.7f);
        plain.takeOriginal (orig.data(), orig.data(), 512);
        plain.mix (procL.data(), procR.data(), 512);
        expect ("no match: the processed version as it is", procL[100] == 0.7f);
    }

    // a streaming service's playback gain
    {
        using loudness::playbackGainDb;
        expect ("playback gain: a loud song is turned down to the service's level", near (playbackGainDb (-8, -0.5, -14, true), -6.0, 1e-12));
        expect ("playback gain: a quiet song is turned up only into its headroom", near (playbackGainDb (-18, -3, -14, true), 2.0, 1e-12));
        expect ("playback gain: a service that never turns up leaves it", playbackGainDb (-18, -3, -14, false) == 0.0);
    }

    // a lossless encoder's predictors: a sine is predicted almost exactly from two samples
    {
        std::vector<std::int32_t> x (4096);
        for (size_t i = 0; i < x.size(); ++i) x[i] = (std::int32_t) std::lround (10000.0 * std::sin (twoPi * 440.0 * (double) i / 48000.0));
        const auto w = windows::tukey (4096, 0.5);
        double err1 = 0, err2 = 0;
        int orders = 0;
        lpc::blockPredictors (x.data(), 4096, w.data(), 12, [&] (int order, const double*, double err) { if (order == 1) err1 = err; if (order == 2) err2 = err; orders = order; });
        expect ("predictors: a sine needs two coefficients, every order up to 12 offered", err2 < err1 * 1e-3 && orders == 12, "%.3g / %.3g", err1, err2);
    }

    // levels, silence and a mono average
    {
        std::vector<float> x { 0.5f, -0.5f, 0.5f, -0.5f };
        expect ("RMS of a square wave is its height", near (channels::rmsOf (x.data(), 4), 0.5, 1e-7));
        std::vector<float> quiet (64, 0.0001f), loud (64, 0.0001f);
        loud[40] = 0.1f;
        expect ("nearly silent: a quiet block is, one with a peak isn't", channels::isNearlySilent (quiet.data(), 64) && ! channels::isNearlySilent (loud.data(), 64));
        const float l[] { 1.0f, 0.0f }, r[] { 0.0f, 1.0f };
        const float* ch[] { l, r };
        float m[2];
        channels::monoAverage (ch, 2, 2, m);
        expect ("mono average halves each side", m[0] == 0.5f && m[1] == 0.5f);
        expect ("gain to a peak: -0.3 dB from a peak of 0.5", near (channels::gainToPeakDb (0.5f, -0.3f), -0.3 + 6.0206, 1e-3) && channels::gainToPeakDb (0.0f, -0.3f) == 0.0f);
    }

    // ranges, axes and the tuner's window
    {
        expect ("tempo range 20 to 400 BPM", tempo::isTempo (20) && tempo::isTempo (400) && ! tempo::isTempo (19.9) && tempo::clampTempo (500) == 400);
        expect ("log axis: 20 Hz to 20 kHz, 632 Hz in the middle", near (axis::logPosition (20.0f), 0.0, 1e-7) && near (axis::logPosition (20000.0f), 1.0, 1e-6)
                                                                    && near (axis::logHz (0.5f), 632.456, 0.01));
        expect ("tuner window: 2 at 48 kHz, 4 at 96 kHz, 4096 long", TunerPitch::window (48000).stride == 2 && TunerPitch::window (96000).stride == 4
                                                                     && TunerPitch::window (44100).n == 4096);
        std::vector<float> tone (48000);
        for (size_t i = 0; i < tone.size(); ++i) tone[i] = (float) (0.4 * std::sin (twoPi * 110.0 * (double) i / 48000.0));
        const float hz = TunerPitch::estimateLatest (tone.data(), (int) tone.size(), 48000.0);
        expect ("tuner: the latest window of a 110 Hz tone reads 110 Hz", near (hz, 110.0, 0.1), "%.2f Hz", hz);
    }

    // hosting: a plugin's dry and wet levels, a rack return's ramps, the clean-up after a plugin
    // (each checked against the arithmetic Tracktion Engine used, written out here)
    {
        std::vector<float> x (256), a, b;
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float) std::sin (0.07 * (double) i) * 0.8f;

        a = x; b = x;
        mix::applyGainRampStepFirst (a.data(), 256, 0.2f, 0.9f);
        { const float step = (0.9f - 0.2f) / 256.0f; float g = 0.2f; for (auto& v : b) { g += step; v *= g; } }
        expect ("rack return: the wet ramp steps before each sample, as before", a == b && near (a[255], x[255] * 0.9, 1e-5));

        a = x; b = x;
        mix::addWithGainRamp (x.data(), a.data(), 256, 1.0f, 0.25f);
        { const float d = (0.25f - 1.0f) / 256.0f; float g = 1.0f; for (size_t i = 0; i < b.size(); ++i) { b[i] += x[i] * g; g += d; } }
        expect ("rack return: the dry signal added with a ramp, as before", a == b);

        expect ("hosted plugin: the dry copy is kept only above 0.00004", ! mix::hostKeepsDry (0.00004f) && mix::hostKeepsDry (0.00005f));
        a = x; float* ch[] { a.data() };
        mix::hostWetGain (0.9995f, ch, 1, 256);
        const bool fullUntouched = a == x;
        mix::hostWetGain (0.5f, ch, 1, 256);
        const bool halved = a[10] == x[10] * 0.5f;
        mix::hostWetGain (0.0f, ch, 1, 256);
        expect ("hosted plugin: wet at full level leaves it, below scales it, zero clears it",
                fullUntouched && halved && std::all_of (a.begin(), a.end(), [] (float v) { return v == 0.0f && ! std::signbit (v); }));

        std::vector<float> s { 0.5f, -0.5f, 1e-7f, -1e-7f, 150.0f, -150.0f, std::nanf (""), 1.0f / 262144.0f };
        float* sc[] { s.data() };
        channels::sanitise (sc, 1, (int) s.size(), 100.0f);
        expect ("clean-up: tiny samples and NaN to zero, loud ones held at the limit",
                s == std::vector<float> { 0.5f, -0.5f, 0.0f, 0.0f, 100.0f, -100.0f, 0.0f, 1.0f / 262144.0f });
        std::vector<float> nn { 0.25f, std::nanf (""), -2.0f };
        float* nc[] { nn.data() };
        channels::zeroNaN (nc, 1, 3);
        expect ("NaN to zero, everything else kept", nn == std::vector<float> { 0.25f, 0.0f, -2.0f });
    }

    // the compressor's key input (a sidechain)
    {
        constexpr int n = 48000;
        std::vector<float> tone (n), kick (n, 0.0f);
        for (int i = 0; i < n; ++i) tone[(size_t) i] = (float) (0.1 * std::sin (twoPi * 220.0 * i / 48000.0));
        for (int i = 24000; i < 30000; ++i) kick[(size_t) i] = 0.9f;
        auto run = [&] (bool keyed)
        {
            Compressor c;
            c.prepare (48000.0);
            c.setDetail ({ -10.0f, 8.0f, 1.0f, 50.0f, 0.0f, false });
            std::vector<float> y = tone;
            float* ch[] { y.data() };
            const float* key[] { kick.data() };
            c.process (ch, 1, n, keyed ? key : nullptr, keyed ? 1 : 0);
            return y;
        };
        const auto plain = run (false), ducked = run (true);
        const float* before[] { ducked.data() + 12000 };
        const float* during[] { ducked.data() + 26000 };
        const float quietDb = ampToDb (channels::peakOf (before, 1, 2000)), duckDb = ampToDb (channels::peakOf (during, 1, 2000));
        expect ("sidechain: the quiet tone passes until the key is loud", near (quietDb, -20.0, 0.1), "%.2f dB", quietDb);
        expect ("sidechain: a loud key ducks it", duckDb < quietDb - 5.0f, "%.2f dB", duckDb);
        expect ("sidechain: without a key, a tone 10 dB under the threshold is untouched", plain == tone);
    }

    // a section of a clip's fade, bit for bit as a DAW's block-by-block loop
    // (the position in double, one gain for every channel)
    {
        bool same = true;
        const channels::FadeCurve curves[] { channels::FadeCurve::quarterSine, channels::FadeCurve::sineSquared,
                                             channels::FadeCurve::linear, channels::FadeCurve::concave, channels::FadeCurve::sCurve };
        const float alphas[][2] { { 0.2f, 0.9f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } };
        for (auto curve : curves)
            for (auto& a : alphas)
                for (int n : { 1, 7, 4096 })
                {
                    std::vector<float> l (n), r (n), refL, refR, src (n), dst (n, 0.25f), refDst;
                    for (int i = 0; i < n; ++i) { l[i] = std::sin (0.01f * i); r[i] = std::cos (0.013f * i); src[i] = 0.5f - 0.0003f * i; }
                    refL = l; refR = r; refDst = dst;
                    const double delta = ((double) a[1] - (double) a[0]) / n;
                    double alpha = a[0];
                    for (int i = 0; i < n; ++i)
                    {
                        const float g = channels::fadeGainFloat (curve, alpha);
                        refL[i] *= g; refR[i] *= g; refDst[i] += src[i] * g;
                        alpha += delta;
                    }
                    float* ch[] { l.data(), r.data() };
                    channels::fadeSection (ch, 2, n, curve, a[0], a[1]);
                    channels::addFadeSection (src.data(), dst.data(), n, curve, a[0], a[1]);
                    same = same && l == refL && r == refR && dst == refDst;
                }
        expect ("fade section: every curve and block size, bit for bit", same);
    }

    // a clip's de-click when playback jumps
    {
        std::vector<float> x (64, 0.3f), y (64, 0.3f), z (64, 0.3f), refX = x, refY = y;
        const int len = std::min (64, channels::heldFadeSamples);
        for (int i = 0; i < len; ++i) { const float a = (float) i / (float) len; refX[i] = 0.8f * (1.0f - a); refY[i] = a * refY[i] + 0.8f * (1.0f - a); }
        channels::fadeFromHeld (x.data(), len, 0.8f);
        channels::joinFromHeld (y.data(), len, 0.8f);
        channels::fadeFromHeld (z.data(), len, 0.0f);
        expect ("de-click: the held sample fades away, bit for bit", x == refX);
        expect ("de-click: new audio joins from the held sample, bit for bit", y == refY);
        expect ("de-click: a held zero leaves the block as it is", z == std::vector<float> (64, 0.3f));
        expect ("de-click: 40 samples, 10 at a clip's start", channels::heldFadeSamples == 40 && channels::heldFadeStartSamples == 10);
        expect ("scrubbing plays at 0.4", mix::scrubGain == 0.4f);
    }

    // what a DAW sends to the sound card: nothing that isn't a normal number
    {
        const float in[] { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::min() / 2, -0.0f,
                           std::numeric_limits<float>::min(), 1.0f, -0.5f };
        std::vector<float> a (std::begin (in), std::end (in)), b = a;
        float* ca[] { a.data() };
        float* cb[] { b.data() };
        channels::zeroNaN (ca, 1, 8, channels::Zero::notNormal);
        channels::zeroNaN (cb, 1, 8);
        bool cleared = true;
        for (int i = 0; i < 5; ++i) cleared = cleared && a[i] == 0.0f && ! std::signbit (a[i]);
        expect ("output clean-up: NaN, infinities, denormals and -0 become +0", cleared);
        expect ("output clean-up: normal numbers pass", a[5] == std::numeric_limits<float>::min() && a[6] == 1.0f && a[7] == -0.5f);
        expect ("zeroNaN on its own clears NaN only", b[0] == 0.0f && std::isinf (b[1]) && b[3] != 0.0f);
    }

    // a meter's highest level since it was last read
    {
        meters::Reading held;
        meters::keepHighest (held, { 10, -20.0f });
        meters::keepHighest (held, { 20, -30.0f });
        meters::keepHighest (held, { 30, -20.0f });
        meters::keepHighest (held, { 40, std::numeric_limits<float>::quiet_NaN() });
        const auto r = meters::takeHighest (held);
        expect ("meter hold: the loudest, the later of two equal ones, never a NaN", r.timeMs == 30 && r.db == -20.0f);
        expect ("meter hold: reading leaves silence at the same time", held.timeMs == 30 && held.db == meters::faderFloorDb);
        expect ("meter level: silence for no signal, dB otherwise", meters::levelDb (0.0f) == meters::faderFloorDb && near (meters::levelDb (0.5f), -6.0206, 1e-3));
        expect ("overload above 0.999", meters::overloadGain == 0.999f);
    }

    // the denormal guard
    {
        if constexpr (noDenormalsMask != 0)
        {
            const auto before = fpStatus();
            volatile float tiny = 1e-30f, scale = 1e-10f;
            float inside;
            {
                ScopedNoDenormals guard;
                inside = tiny * scale;
                expect ("denormals: off inside the guard", denormalsDisabled() && inside == 0.0f);
            }
            const float after = tiny * scale;
            expect ("denormals: the thread's mode back after it", (fpStatus() & noDenormalsMask) == (before & noDenormalsMask) && after != 0.0f);
            disableDenormals();
            const bool off = denormalsDisabled();
            disableDenormals (false);
            // (only the mode bits: x86 keeps sticky flags that arithmetic sets in the same register)
            expect ("denormals: turned off and on again", off && ! denormalsDisabled() && (fpStatus() & noDenormalsMask) == (before & noDenormalsMask));
        }
        else
            expect ("denormals: no mode on this system, nothing to do", ! denormalsDisabled());
    }

    // the click's soft count
    {
        metronome::Buffer s (2000);
        for (size_t i = 0; i < s.size(); ++i) s[i] = 0.7f * std::sin (0.05f * (float) i) * std::exp (-0.002f * (float) i);
        auto ref = s;
        const float* ch[1] { ref.data() };
        channels::toPeakWithEndFade (ref, channels::peakOf (ch, 1, (int) ref.size()) * (float) metronome::softCount, 256);
        expect ("soft count: 0.4 of the peak with the end fade, bit for bit", metronome::atLevel (s, (float) metronome::softCount) == ref);
        expect ("soft count: full level leaves a sound as it is", metronome::atLevel (s, 1.0f) == s);
    }

    // a note readout held through gaps (a plugin's screen, one reading per frame)
    {
        TunerFollow f (noteReadout);
        int hold = 0;
        float shown = -1.0f;
        bool same = true;
        const float notes[] { 0, 60, 60.4f, 0, 0, 0, 62, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        for (float note : notes)
        {
            if (note > 0.0f) { shown = note; hold = 10; }
            else if (hold > 0 && --hold == 0) shown = -1.0f;
            const float got = f.push (note);
            same = same && (got > 0.0f ? got : -1.0f) == shown;
        }
        expect ("note readout: each note as it is, held through nine empty frames, cleared on the tenth", same);
    }

    std::printf (failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
