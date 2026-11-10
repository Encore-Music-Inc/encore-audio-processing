// The loudness meter (dsp/Loudness.h, ITU-R BS.1770) built without JUCE, as the
// browser build is: reference values, and the per-channel 100 ms block energies
// it records for reports made after measuring. Each check prints PASS or FAIL;
// the suite exits non-zero on any failure.
#include <cmath>
#include <cstdio>
#include <random>
#include "dsp/Loudness.h"

namespace
{
    int failures = 0;
    constexpr double twoPi = 6.283185307179586;

    void expect (const char* name, bool ok, const char* fmt = "", double a = 0, double b = 0, double c = 0)
    {
        std::printf ("%s %-62s ", ok ? "PASS" : "FAIL", name);
        std::printf (fmt, a, b, c);
        std::printf ("\n");
        if (! ok) ++failures;
    }

    struct Stereo { std::vector<float> l, r; };

    Stereo noisySong (double fs, double seconds, unsigned seed)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<float> u (-1.0f, 1.0f);
        const size_t n = (size_t) (fs * seconds);
        Stereo s { std::vector<float> (n), std::vector<float> (n) };
        for (size_t i = 0; i < n; ++i)
        {
            const double t = (double) i / fs, swell = 0.5 + 0.5 * std::sin (twoPi * 0.2 * t);
            s.l[i] = (float) (swell * (0.3 * std::sin (twoPi * 220 * t) + 0.05 * u (rng)));
            s.r[i] = (float) (0.2 * swell * u (rng));
        }
        return s;
    }

    // measures in blocks of `block` samples; r may be null (one channel)
    vs::LoudnessMeter& measure (vs::LoudnessMeter& m, const std::vector<float>& l, const std::vector<float>* r, int block = 1000)
    {
        for (size_t s = 0; s < l.size(); s += (size_t) block)
        {
            const int n = (int) std::min ((size_t) block, l.size() - s);
            m.process (l.data() + s, r != nullptr ? r->data() + s : nullptr, n);
        }
        return m;
    }

    double lufs (double meanSquare) { return -0.691 + 10.0 * std::log10 (meanSquare); }
}

int main()
{
    // ---- reference values ----
    for (double fs : { 44100.0, 48000.0 })
    {
        std::vector<float> l ((size_t) (fs * 5)), r (l.size(), 0.0f);
        for (size_t i = 0; i < l.size(); ++i) l[i] = (float) std::sin (twoPi * 997.0 * (double) i / fs);
        vs::LoudnessMeter m;
        m.prepare (fs);
        const double v = measure (m, l, &r).integrated();
        expect (fs == 48000.0 ? "BS.1770: a full-scale 997 Hz sine in one channel, 48 kHz" : "BS.1770: a full-scale 997 Hz sine in one channel, 44.1 kHz",
                std::abs (v + 3.01) < 0.01, "%.3f LUFS", v);
    }
    {
        vs::LoudnessMeter m;
        m.prepare (48000.0);
        std::vector<float> silence (48000 * 2, 0.0f);
        expect ("silence measures -100", measure (m, silence, &silence).integrated() == -100.0f);
    }

    // ---- recording blocks ----
    const double fs = 48000.0;
    const auto song = noisySong (fs, 12.3, 7);
    vs::LoudnessMeter plain, rec;
    plain.prepare (fs);
    rec.prepare (fs);
    rec.setRecordBlocks (true);
    measure (plain, song.l, &song.r);
    measure (rec, song.l, &song.r, 777);
    expect ("recording blocks changes nothing the meter reports",
            plain.integrated() == rec.integrated() && plain.shortTerm.load() == rec.shortTerm.load() && plain.momentary.load() == rec.momentary.load(),
            "%.6f LUFS", plain.integrated());

    const auto& eL = rec.blockEnergy (0);
    const auto& eR = rec.blockEnergy (1);
    const int len = rec.blockLength();
    expect ("a block is 100 ms, and every whole block is recorded", len == 4800 && (int) eL.size() == (int) (song.l.size() / (size_t) len)
                                                                      && eR.size() == eL.size(), "%.0f blocks", (double) eL.size());
    expect ("an unrecorded meter keeps no blocks", plain.blockEnergy (0).empty());

    // each channel's blocks are what that channel measures on its own
    {
        vs::LoudnessMeter left, right;
        left.prepare (fs);
        right.prepare (fs);
        left.setRecordBlocks (true);
        right.setRecordBlocks (true);
        measure (left, song.l, nullptr);
        measure (right, song.r, nullptr);
        bool same = left.blockEnergy (0).size() == eL.size();
        bool rightSilent = true;
        for (size_t k = 0; same && k < eL.size(); ++k)
        {
            same = left.blockEnergy (0)[k] == eL[k] && right.blockEnergy (0)[k] == eR[k];
            rightSilent = rightSilent && left.blockEnergy (1)[k] == 0.0;
        }
        expect ("each channel's blocks match it measured on its own", same && rightSilent);
    }

    // short-term loudness (3 s) made from the blocks agrees with the meter's own
    {
        double e = 0;
        for (size_t k = eL.size() - 30; k < eL.size(); ++k) e += eL[k] + eR[k];
        // the meter's short-term covers the last 30 blocks it closed, which are the last 30 recorded
        const double fromBlocks = lufs (e / (30.0 * len));
        expect ("short-term loudness from the blocks matches the meter's", std::abs (fromBlocks - rec.shortTerm.load()) < 0.001,
                "%.4f / %.4f LUFS", fromBlocks, rec.shortTerm.load());
    }

    // block lengths at other rates: round (rate / 10)
    {
        bool ok = true;
        for (double r : { 11025.0, 44100.0, 96000.0 })
        {
            vs::LoudnessMeter m;
            m.prepare (r);
            ok = ok && m.blockLength() == (int) std::round (r * 0.1);
        }
        expect ("a block is a tenth of a second at 11.025, 44.1 and 96 kHz", ok);
    }

    rec.reset();
    expect ("reset clears the recorded blocks", rec.blockEnergy (0).empty() && rec.blockEnergy (1).empty());

    std::printf ("%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
