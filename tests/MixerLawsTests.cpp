// What a DAW's mixer and timeline lean on: pan laws, the volume fader's law,
// fade shapes, automation curves, the click voices a host schedules, the live
// resampler for clips at another rate or speed, the waveform picture, levels and silence, the tempo
// range, drawing axes and the tuner's window. Built without JUCE. Each check
// prints PASS or FAIL; the suite exits non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include "dsp/Channels.h"
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

    std::printf (failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
