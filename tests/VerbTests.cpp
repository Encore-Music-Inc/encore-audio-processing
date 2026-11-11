// Offline checks for the reverb engine. Run: build\VerbTests_artefacts\Release\VerbTests.exe
#include <juce_dsp/juce_dsp.h>
#include <cstdio>
#include "dsp/ReverbEngine.h"

namespace
{
    constexpr double fs = 48000.0;
    int failures = 0;
    const char* names[] { "Room", "Chamber", "Plate", "Hall", "Huge", "Spring" };

    void expect (const juce::String& name, bool ok, const juce::String& detail = {})
    {
        std::printf ("%s %-46s %s\n", ok ? "PASS" : "FAIL", name.toRawUTF8(), detail.toRawUTF8());
        if (! ok) ++failures;
    }

    struct Out { std::vector<float> l, r; };

    Out render (vs::ReverbEngine& e, const std::vector<float>& in, int extraSamples = 0)
    {
        const int n = (int) in.size() + extraSamples;
        std::vector<float> x ((size_t) n, 0.0f);
        std::copy (in.begin(), in.end(), x.begin());
        Out o { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
        for (int s = 0; s < n; s += 256)
        {
            const int len = std::min (256, n - s);
            e.process (x.data() + s, x.data() + s, o.l.data() + s, o.r.data() + s, len);
        }
        return o;
    }

    // RT60 in the 1 kHz octave band (how decay time is normally specified),
    // from the Schroeder backward-integrated energy curve, -5 to -25 dB (T20)
    float measureRt60 (const Out& raw)
    {
        Out o = raw;
        for (auto* ch : { &o.l, &o.r })
        {
            juce::dsp::IIR::Filter<float> bp1, bp2;
            bp1.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (fs, 1000.0, 1.4);
            bp2.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (fs, 1000.0, 1.4);
            for (auto& v : *ch) v = bp2.processSample (bp1.processSample (v));
        }
        const size_t n = o.l.size();
        std::vector<double> edc (n);
        double acc = 0.0;
        for (size_t i = n; i-- > 0;)
        {
            acc += (double) o.l[i] * o.l[i] + (double) o.r[i] * o.r[i];
            edc[i] = acc;
        }
        const double total = edc[0];
        auto timeAt = [&] (double db)
        {
            const double target = total * std::pow (10.0, db / 10.0);
            for (size_t i = 0; i < n; ++i)
                if (edc[i] <= target) return (double) i / fs;
            return (double) n / fs;
        };
        return (float) (3.0 * (timeAt (-25.0) - timeAt (-5.0)));
    }

    float rmsDb (const std::vector<float>& x, size_t from, size_t to)
    {
        double s = 0.0;
        for (size_t i = from; i < to; ++i) s += (double) x[i] * x[i];
        return 10.0f * (float) std::log10 (s / (double) (to - from) + 1e-20);
    }

    bool finiteAndBounded (const Out& o, float limit, float& peak)
    {
        peak = 0.0f;
        for (size_t i = 0; i < o.l.size(); ++i)
        {
            if (! std::isfinite (o.l[i]) || ! std::isfinite (o.r[i])) return false;
            peak = std::max ({ peak, std::abs (o.l[i]), std::abs (o.r[i]) });
        }
        return peak < limit;
    }

    std::vector<float> noise (double seconds, float amp, int seed = 7)
    {
        juce::Random r (seed);
        std::vector<float> x ((size_t) (seconds * fs));
        for (auto& v : x) v = (r.nextFloat() * 2.0f - 1.0f) * amp;
        return x;
    }
}

int main()
{
    using S = vs::ReverbEngine::Settings;

    // 1. Decay time accuracy (bright tone so damping doesn't shorten it)
    for (int mode = 0; mode < vs::ReverbEngine::numModes; ++mode)
    {
        for (float rt : { 1.0f, 3.0f })
        {
            vs::ReverbEngine e;
            e.prepare (fs, 256);
            S s; s.mode = mode; s.decaySec = rt; s.toneHz = 18000.0f; s.lowCutHz = 20.0f; s.predelayMs = 0.0f; s.mod = 0.3f;
            e.setSettings (s);
            render (e, std::vector<float> ((size_t) (0.05 * fs), 0.0f)); // let the start-up fade finish
            std::vector<float> imp ((size_t) 1, 1.0f);
            auto o = render (e, imp, (int) ((rt * 2.0 + 0.5) * fs));
            const float got = measureRt60 (o);
            const float err = (got - rt) / rt;
            expect (juce::String (names[mode]) + " decay " + juce::String (rt, 1) + " s", std::abs (err) < 0.3f,
                    "measured " + juce::String (got, 2) + " s (" + juce::String (err * 100.0f, 0) + "%)");
        }
    }

    // 2. Loudness consistency across modes (2 s decay, noise burst)
    {
        float lv[vs::ReverbEngine::numModes];
        for (int mode = 0; mode < vs::ReverbEngine::numModes; ++mode)
        {
            vs::ReverbEngine e;
            e.prepare (fs, 256);
            S s; s.mode = mode; s.decaySec = 2.0f;
            e.setSettings (s);
            auto o = render (e, noise (3.0, 0.25f));
            lv[mode] = rmsDb (o.l, (size_t) fs, (size_t) (3.0 * fs));
        }
        const float ref = lv[vs::ReverbEngine::plate];
        juce::String detail;
        bool ok = true;
        for (int mode = 0; mode < vs::ReverbEngine::numModes; ++mode)
        {
            detail << names[mode] << " " << juce::String (lv[mode] - ref, 1) << "  ";
            ok = ok && std::abs (lv[mode] - ref) < 5.0f;
        }
        expect ("modes within 5 dB of Plate (dB):", ok, detail);
    }

    // 3. Stability torture: Huge, 20 s decay, size 1, full shimmer, full mod
    {
        vs::ReverbEngine e;
        e.prepare (fs, 256);
        S s; s.mode = vs::ReverbEngine::huge; s.decaySec = 20.0f; s.size = 1.0f; s.shimmer = 1.0f; s.mod = 1.0f; s.toneHz = 20000.0f;
        e.setSettings (s);
        auto o = render (e, noise (2.0, 0.5f), (int) (40.0 * fs));
        float peak = 0.0f;
        const bool ok = finiteAndBounded (o, 4.0f, peak);
        expect ("Huge + full shimmer, 20 s decay stays bounded", ok, "peak " + juce::String (peak, 2));
        const float early = rmsDb (o.l, (size_t) (2 * fs), (size_t) (4 * fs));
        const float late = rmsDb (o.l, (size_t) (38 * fs), (size_t) (42 * fs));
        expect ("  ...and is decaying, not building up", late < early - 6.0f,
                juce::String (early, 1) + " dB -> " + juce::String (late, 1) + " dB");
    }

    for (int mode = 0; mode < vs::ReverbEngine::numModes; ++mode)
    {
        vs::ReverbEngine e;
        e.prepare (fs, 256);
        S s; s.mode = mode; s.decaySec = 20.0f; s.size = 1.0f; s.mod = 1.0f; s.toneHz = 20000.0f; s.width = 1.5f; s.duck = 1.0f;
        e.setSettings (s);
        auto o = render (e, noise (1.0, 0.9f), (int) (20.0 * fs));
        float peak = 0.0f;
        const bool ok = finiteAndBounded (o, 4.0f, peak);
        expect (juce::String (names[mode]) + " at extreme settings stays bounded", ok, "peak " + juce::String (peak, 2));
    }

    // 4. Mode switching mid-tail: no NaN, no jump bigger than the signal itself
    {
        vs::ReverbEngine e;
        e.prepare (fs, 256);
        S s; s.mode = vs::ReverbEngine::hall; s.decaySec = 3.0f;
        e.setSettings (s);
        std::vector<float> in = noise (1.0, 0.3f);
        Out all;
        for (int mode = 0; mode < vs::ReverbEngine::numModes * 2; ++mode)
        {
            s.mode = mode % vs::ReverbEngine::numModes;
            e.setSettings (s);
            auto o = render (e, in);
            all.l.insert (all.l.end(), o.l.begin(), o.l.end());
            all.r.insert (all.r.end(), o.r.begin(), o.r.end());
        }
        float peak = 0.0f, maxStep = 0.0f;
        const bool ok = finiteAndBounded (all, 4.0f, peak);
        for (size_t i = 1; i < all.l.size(); ++i) maxStep = std::max (maxStep, std::abs (all.l[i] - all.l[i - 1]));
        expect ("switching modes mid-tail is smooth", ok && maxStep < 0.5f,
                "peak " + juce::String (peak, 2) + ", max sample step " + juce::String (maxStep, 3));
    }

    // 5. Ducking actually ducks while the dry signal plays
    {
        auto tail = [] (float duck)
        {
            vs::ReverbEngine e;
            e.prepare (fs, 256);
            S s; s.mode = vs::ReverbEngine::hall; s.decaySec = 2.0f; s.duck = duck;
            e.setSettings (s);
            auto o = render (e, noise (2.0, 0.3f));
            return rmsDb (o.l, (size_t) fs, (size_t) (2 * fs));
        };
        const float off = tail (0.0f), on = tail (1.0f);
        expect ("ducking lowers the tail under a loud input", on < off - 6.0f, juce::String (off, 1) + " dB -> " + juce::String (on, 1) + " dB");
    }

    // 6. CPU per mode
    {
        juce::String detail;
        for (int mode = 0; mode < vs::ReverbEngine::numModes; ++mode)
        {
            vs::ReverbEngine e;
            e.prepare (fs, 256);
            S s; s.mode = mode; s.shimmer = mode == vs::ReverbEngine::huge ? 0.5f : 0.0f;
            e.setSettings (s);
            auto in = noise (10.0, 0.2f);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            render (e, in);
            detail << names[mode] << " " << juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 10000.0 * 100.0, 2) << "%  ";
        }
        std::printf ("INFO CPU (one core, 48 kHz stereo): %s\n", detail.toRawUTF8());
    }

    // The early reflections a picture draws are the ones the engine plays
    {
        using E = vs::ReverbEngine;
        const auto room = E::earlyReflections (E::room, 0.5f), hall = E::earlyReflections (E::hall, 0.5f);
        const auto huge = E::earlyReflections (E::huge, 1.0f), plate = E::earlyReflections (E::plate, 0.5f);
        const auto spring = E::earlyReflections (E::spring, 0.5f);
        float longest = 0.0f;
        for (auto& t : huge.taps) longest = std::max (longest, t.ms);
        expect ("reflections: Huge has them, later than Hall's; Plate and Spring have none",
                huge.level > 0.0f && huge.taps[0].ms > hall.taps[0].ms && hall.taps[0].ms > room.taps[0].ms
                    && plate.level == 0.0f && spring.level == 0.0f,
                juce::String (huge.taps[0].ms, 2) + " ms first in Huge");
        expect ("reflections: none later than the engine plays them", longest <= E::maxReflectionSec * 1000.0f, juce::String (longest, 1) + " ms");
    }

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
