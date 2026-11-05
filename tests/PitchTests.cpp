// Offline checks for the pitch engine: feed a synthetic voice in, measure
// what comes out. Run: build\tests\Release\PitchTests.exe

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cstdio>
#include "dsp/PitchEngine.h"

namespace
{
    constexpr double fs = 48000.0;
    int failures = 0;

    // Sawtooth "vocal cords" through three formant resonators ("ah"), with
    // gentle vibrato so the detector has to track a moving pitch.
    std::vector<float> makeVoice (float f0, double seconds, float vibratoCents = 15.0f)
    {
        const int n = (int) (seconds * fs);
        std::vector<float> out ((size_t) n);
        juce::dsp::IIR::Filter<float> f1, f2, f3;
        f1.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (fs, 700.0, 5.0);
        f2.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (fs, 1220.0, 6.0);
        f3.coefficients = juce::dsp::IIR::Coefficients<float>::makeBandPass (fs, 2600.0, 8.0);

        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / fs;
            const double f = f0 * std::pow (2.0, vibratoCents / 1200.0 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.0 * t));
            phase += f / fs;
            if (phase >= 1.0) phase -= 1.0;
            const float saw = (float) (2.0 * phase - 1.0);
            out[(size_t) i] = 0.5f * (f1.processSample (saw) + 0.6f * f2.processSample (saw) + 0.3f * f3.processSample (saw));
        }
        return out;
    }

    struct Rendered { std::vector<float> lead, harm; int latency; };

    Rendered render (const std::vector<float>& input, const vs::PitchEngine::Settings& s)
    {
        vs::PitchEngine e;
        e.prepare (fs);
        e.setSettings (s);
        Rendered r;
        r.latency = e.getLatency();
        const int n = (int) input.size();
        r.lead.resize ((size_t) n);
        r.harm.resize ((size_t) n);
        std::vector<float> hl ((size_t) n), hr ((size_t) n);

        for (int start = 0; start < n; start += 256) // host-sized blocks
        {
            const int len = std::min (256, n - start);
            e.process (input.data() + start, r.lead.data() + start, hl.data() + start, hr.data() + start, len);
        }
        for (int i = 0; i < n; ++i) r.harm[(size_t) i] = hl[(size_t) i] + hr[(size_t) i];
        return r;
    }

    // Median detected pitch (Hz) over the settled part of a signal
    float measure (const std::vector<float>& x, float fromSec = 1.0f)
    {
        vs::PitchDetector d;
        d.prepare (fs);
        std::vector<float> hz;
        for (int pos = (int) (fromSec * fs); pos + d.windowSize() < (int) x.size(); pos += 1024)
        {
            auto res = d.analyse (x.data() + pos);
            if (res.voiced) hz.push_back (res.f0);
        }
        if (hz.empty()) return 0.0f;
        std::nth_element (hz.begin(), hz.begin() + (long) hz.size() / 2, hz.end());
        return hz[hz.size() / 2];
    }

    float rmsDb (const std::vector<float>& x, int from)
    {
        double s = 0.0;
        for (size_t i = (size_t) from; i < x.size(); ++i) s += (double) x[i] * x[i];
        return 10.0f * (float) std::log10 (s / (double) (x.size() - (size_t) from) + 1e-20);
    }

    bool allFinite (const std::vector<float>& x)
    {
        return std::all_of (x.begin(), x.end(), [] (float v) { return std::isfinite (v); });
    }

    void expectPitch (const char* name, float measuredHz, float expectedHz, float toleranceCents)
    {
        const float cents = measuredHz > 0.0f ? 1200.0f * std::log2 (measuredHz / expectedHz) : 9999.0f;
        const bool ok = std::abs (cents) <= toleranceCents;
        std::printf ("%s %-44s expected %7.2f Hz, got %7.2f Hz (%+.1f cents)\n", ok ? "PASS" : "FAIL", name, expectedHz, measuredHz, cents);
        if (! ok) ++failures;
    }

    void expect (const char* name, bool ok, const juce::String& detail = {})
    {
        std::printf ("%s %-44s %s\n", ok ? "PASS" : "FAIL", name, detail.toRawUTF8());
        if (! ok) ++failures;
    }

    float midiHz (int note) { return 440.0f * std::pow (2.0f, (float) (note - 69) / 12.0f); }
}

int main()
{
    using S = vs::PitchEngine::Settings;

    // Detector sanity on the raw synthetic voice
    for (float f : { 98.0f, 220.0f, 440.0f, 880.0f })
        expectPitch (("detector at " + juce::String (f) + " Hz").toRawUTF8(), measure (makeVoice (f, 2.0, 0.0f), 0.2f), f, 3.0f);

    // Bypass path: Tune and Harmony both off must be a pure delay
    {
        auto in = makeVoice (220.0f, 1.0);
        auto r = render (in, S {});
        bool exact = true;
        for (size_t i = (size_t) r.latency; i < in.size(); ++i)
            if (r.lead[i] != in[i - (size_t) r.latency]) { exact = false; break; }
        expect ("tune off = bit-exact delayed input", exact, "latency " + juce::String (r.latency) + " samples ("
                + juce::String (r.latency / fs * 1000.0, 1) + " ms)");
    }

    // Hard tune: 40 cents sharp A3 should land on A3
    {
        S s; s.tuneOn = true; s.speedMs = 0.0f; s.amount = 1.0f;
        auto in = makeVoice (220.0f * std::pow (2.0f, 0.4f / 12.0f), 3.0, 0.0f);
        auto r = render (in, s);
        expectPitch ("hard tune: A3 +40c -> A3", measure (r.lead), 220.0f, 6.0f);
        expect ("hard tune output finite", allFinite (r.lead));
        const float diff = rmsDb (r.lead, 48000) - rmsDb (in, 48000);
        expect ("hard tune keeps level (within 3 dB)", std::abs (diff) < 3.0f, juce::String (diff, 2) + " dB");
    }

    // Gentle tune with vibrato: 30 cents flat E4 pulled toward E4
    {
        S s; s.tuneOn = true; s.speedMs = 40.0f; s.amount = 1.0f; s.key = 0; s.scale = 1; // C major
        auto in = makeVoice (midiHz (64) * std::pow (2.0f, -0.3f / 12.0f), 3.0);
        auto r = render (in, s);
        expectPitch ("40 ms tune, C major: E4 -30c -> E4", measure (r.lead), midiHz (64), 8.0f);
    }

    // Scale snapping: F#4 is not in C major; slightly flat F#4 should go to the nearer scale note
    {
        S s; s.tuneOn = true; s.speedMs = 0.0f; s.key = 0; s.scale = 1;
        auto in = makeVoice (midiHz (66) * std::pow (2.0f, -0.3f / 12.0f), 3.0, 0.0f); // nearer F4
        auto r = render (in, s);
        expectPitch ("C major snaps F#4 -30c -> F4", measure (r.lead), midiHz (65), 8.0f);
    }

    // Harmonies (lead untouched, harmony bus measured on its own)
    struct H { const char* name; int inNote; int interval; int key; int scale; int expectNote; };
    const H cases[] {
        { "harmony C major: E4 + 3rd up -> G4",   64, 7, 0, 1, 67 },
        { "harmony C major: D4 + 3rd up -> F4",   62, 7, 0, 1, 65 },
        { "harmony A minor: A3 + 3rd up -> C4",   57, 7, 9, 2, 60 },
        { "harmony chromatic: A3 + 5th up -> E4", 57, 9, 0, 0, 64 },
        { "harmony C major: G3 + 6th down -> B2", 55, 3, 0, 1, 47 },
        { "harmony: A3 oct down -> A2",           57, 2, 0, 0, 45 },
        { "harmony: A3 oct up -> A4",             57, 11, 0, 0, 69 },
        { "harmony: A3 double -> A3",             57, 1, 0, 0, 57 },
    };
    for (auto& c : cases)
    {
        S s; s.harmonyOn = true; s.key = c.key; s.scale = c.scale; s.humanize = 0.3f;
        s.voice[0] = { c.interval, 0.0f, 0.0f };
        auto r = render (makeVoice (midiHz (c.inNote), 3.0), s);
        expectPitch (c.name, measure (r.harm), midiHz (c.expectNote), 15.0f);
        if (! allFinite (r.harm)) expect ((juce::String (c.name) + " finite").toRawUTF8(), false);
    }

    // Formant shift must not change the harmony's pitch
    {
        S s; s.harmonyOn = true; s.formantSemis = 3.0f; s.voice[0] = { 9, 0.0f, 0.0f };
        auto r = render (makeVoice (midiHz (57), 3.0), s);
        expectPitch ("formant +3 st keeps harmony on E4", measure (r.harm), midiHz (64), 15.0f);
    }

    // Silence and noise must not blow up
    {
        S s; s.tuneOn = true; s.harmonyOn = true; s.voice[0] = { 7, 0.0f, 0.0f }; s.voice[1] = { 2, 0.0f, 0.0f };
        std::vector<float> noise ((size_t) fs * 2);
        juce::Random rnd (1);
        for (size_t i = fs; i < noise.size(); ++i) noise[i] = (rnd.nextFloat() - 0.5f) * 0.3f;
        auto r = render (noise, s);
        float peak = 0.0f;
        for (float v : r.lead) peak = std::max (peak, std::abs (v));
        for (float v : r.harm) peak = std::max (peak, std::abs (v));
        expect ("silence + noise stays finite and sane", allFinite (r.lead) && allFinite (r.harm) && peak < 1.0f,
                "peak " + juce::String (peak, 3));
    }

    // CPU: Tune + two harmony voices, 20 s of audio
    {
        S s; s.tuneOn = true; s.harmonyOn = true; s.voice[0] = { 7, 0.0f, 0.0f }; s.voice[1] = { 2, 0.0f, 0.0f };
        auto in = makeVoice (220.0f, 20.0);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        render (in, s);
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf ("INFO pitch engine CPU: %.2f%% of one core (tune + 2 harmonies @ 48 kHz)\n", ms / 20000.0 * 100.0);
    }

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
