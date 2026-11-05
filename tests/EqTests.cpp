// Offline checks for the EQ building blocks. Run: build\EqTests_artefacts\Release\EqTests.exe
#include <juce_dsp/juce_dsp.h>
#include <cstdio>
#include "dsp/EqBand.h"
#include "dsp/ResonanceFinder.h"

namespace
{
    constexpr double fs = 48000.0;
    int failures = 0;

    void expect (const juce::String& name, bool ok, const juce::String& detail = {})
    {
        std::printf ("%s %-50s %s\n", ok ? "PASS" : "FAIL", name.toRawUTF8(), detail.toRawUTF8());
        if (! ok) ++failures;
    }

    std::vector<float> sine (float hz, double sec, float amp)
    {
        std::vector<float> x ((size_t) (sec * fs));
        for (size_t i = 0; i < x.size(); ++i) x[i] = amp * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (double) i / fs);
        return x;
    }

    std::vector<float> noise (double sec, float amp, int seed = 3)
    {
        juce::Random r (seed);
        std::vector<float> x ((size_t) (sec * fs));
        for (auto& v : x) v = (r.nextFloat() * 2.0f - 1.0f) * amp;
        return x;
    }

    float rmsDb (const std::vector<float>& x, size_t from)
    {
        double s = 0.0;
        for (size_t i = from; i < x.size(); ++i) s += (double) x[i] * x[i];
        return 10.0f * (float) std::log10 (s / (double) (x.size() - from) + 1e-24);
    }

    // Level in a 1/3-octave band around hz
    float bandDb (const std::vector<float>& x, float hz, size_t from)
    {
        vs::Biquad a, b;
        a.c = vs::BiquadCoefs::bandPass (fs, hz, 4.3);
        b.c = a.c;
        std::vector<float> y (x.size());
        for (size_t i = 0; i < x.size(); ++i) y[i] = b.process (0, a.process (0, x[i]));
        return rmsDb (y, from);
    }

    std::vector<float> runBand (const vs::EqBand::Settings& s, std::vector<float> x)
    {
        vs::EqBand band;
        band.prepare (fs);
        band.setTarget (s);
        float* ch[1] { x.data() };
        for (int start = 0; start < (int) x.size(); start += 32)
            band.process (ch, 1, start, std::min (32, (int) x.size() - start));
        return x;
    }

    // measured gain of a band at hz vs. what the UI curve says
    void checkResponse (const char* name, const vs::EqBand::Settings& s, float hz, float expectDb, float tol)
    {
        auto in = sine (hz, 1.0, 0.25f);
        auto out = runBand (s, in);
        const float measured = rmsDb (out, (size_t) (0.3 * fs)) - rmsDb (in, (size_t) (0.3 * fs));
        const float drawn = (float) vs::EqBand::magnitudeDb (s, hz, fs);
        expect (juce::String (name) + " @ " + juce::String (hz, 0) + " Hz",
                std::abs (measured - expectDb) < tol && std::abs (measured - drawn) < 0.3f,
                "expected " + juce::String (expectDb, 1) + ", audio " + juce::String (measured, 2) + ", curve " + juce::String (drawn, 2) + " dB");
    }
}

int main()
{
    using B = vs::EqBand;

    // 1. Each filter type does what the display draws
    {
        B::Settings bell; bell.type = B::bell; bell.freq = 1000; bell.gainDb = 6; bell.q = 1.0f;
        checkResponse ("bell +6 dB at 1 kHz", bell, 1000, 6.0f, 0.2f);
        checkResponse ("bell +6 dB at 1 kHz", bell, 100, 0.0f, 0.3f);

        B::Settings ls; ls.type = B::lowShelf; ls.freq = 200; ls.gainDb = -6; ls.q = 0.7f;
        checkResponse ("low shelf -6 dB at 200 Hz", ls, 40, -6.0f, 0.4f);
        checkResponse ("low shelf -6 dB at 200 Hz", ls, 5000, 0.0f, 0.3f);

        B::Settings hs; hs.type = B::highShelf; hs.freq = 8000; hs.gainDb = 4; hs.q = 0.7f;
        checkResponse ("high shelf +4 dB at 8 kHz", hs, 18000, 4.0f, 0.5f);

        for (int slope : { 0, 1, 2 })
        {
            B::Settings lc; lc.type = B::lowCut; lc.freq = 100; lc.slope = slope; lc.q = 0.7071f;
            const float expectOctBelow = slope == 0 ? -12.3f : slope == 1 ? -24.1f : -48.2f;
            const juce::String nm = "low cut 100 Hz " + juce::String (slope == 0 ? 12 : slope == 1 ? 24 : 48) + " dB/oct";
            checkResponse (nm.toRawUTF8(), lc, 50, expectOctBelow, 1.0f);
            checkResponse (nm.toRawUTF8(), lc, 1000, 0.0f, 0.2f);
            checkResponse (nm.toRawUTF8(), lc, 100, -3.0f, 0.3f);
        }

        // (well below Nyquist, where digital filters match the analog slope)
        B::Settings hc; hc.type = B::highCut; hc.freq = 2000; hc.slope = 1;
        checkResponse ("high cut 2 kHz 24 dB/oct", hc, 4000, -24.1f, 1.0f);

        B::Settings nt; nt.type = B::notch; nt.freq = 60; nt.q = 5.0f;
        {
            auto in = sine (60.0f, 1.0, 0.25f);
            auto out = runBand (nt, in);
            const float measured = rmsDb (out, (size_t) (0.5 * fs)) - rmsDb (in, (size_t) (0.5 * fs));
            expect ("notch 60 Hz removes 60 Hz hum (< -30 dB)", measured < -30.0f, juce::String (measured, 1) + " dB");
        }
        checkResponse ("notch 60 Hz", nt, 200, 0.0f, 0.5f);
    }

    // 2. A flat bell is transparent
    {
        B::Settings flat; flat.type = B::bell; flat.gainDb = 0.0f;
        auto in = noise (1.0, 0.5f);
        auto out = runBand (flat, in);
        double err = 0.0;
        for (size_t i = 0; i < in.size(); ++i) err = std::max (err, (double) std::abs (out[i] - in[i]));
        expect ("0 dB bell is bit-for-bit transparent (<-120 dB)", err < 1e-6, "max error " + juce::String (err, 9));
    }

    // 3. Dragging a node fast doesn't blow up or click hard
    {
        vs::EqBand band;
        band.prepare (fs);
        auto x = noise (2.0, 0.3f);
        float* ch[1] { x.data() };
        bool finite = true;
        for (int start = 0, step = 0; start < (int) x.size(); start += 32, ++step)
        {
            B::Settings s; s.type = B::bell; s.q = 4.0f;
            s.freq = 100.0f * std::pow (100.0f, 0.5f + 0.5f * std::sin ((float) step * 0.05f));
            s.gainDb = 18.0f * std::sin ((float) step * 0.031f);
            band.setTarget (s);
            band.process (ch, 1, start, std::min (32, (int) x.size() - start));
        }
        float peak = 0.0f;
        for (float v : x) { finite = finite && std::isfinite (v); peak = std::max (peak, std::abs (v)); }
        expect ("sweeping freq + gain continuously stays clean", finite && peak < 4.0f, "peak " + juce::String (peak, 2));
    }

    // 4. Smart (dynamic) band: cuts the spike, leaves the rest alone
    {
        auto run = [] (bool withBurst, float& atBand, float& elsewhere)
        {
            auto x = noise (3.0, 0.1f);
            if (withBurst)
            {
                auto s = sine (2500.0f, 3.0, 0.5f);
                for (size_t i = 0; i < x.size(); ++i) x[i] += s[i];
            }
            auto dry = x;
            vs::DynamicBand d;
            d.prepare (fs);
            vs::DynamicBand::Settings ds; ds.on = true; ds.freq = 2500; ds.q = 4; ds.rangeDb = 9; ds.thresholdDb = -12;
            d.setSettings (ds);
            float* ch[1] { x.data() };
            for (int start = 0; start < (int) x.size(); start += 32)
                d.process (ch, 1, start, std::min (32, (int) x.size() - start));
            atBand = bandDb (x, 2500, (size_t) fs) - bandDb (dry, 2500, (size_t) fs);
            elsewhere = bandDb (x, 400, (size_t) fs) - bandDb (dry, 400, (size_t) fs);
        };
        float a = 0, e = 0;
        run (true, a, e);
        expect ("smart band cuts a ringing 2.5 kHz spike", a < -5.0f && std::abs (e) < 0.5f,
                "2.5k " + juce::String (a, 1) + " dB, 400 Hz " + juce::String (e, 2) + " dB");
        run (false, a, e);
        expect ("smart band leaves normal material alone", std::abs (a) < 1.0f, "2.5k " + juce::String (a, 2) + " dB");
    }

    // 5. Carve: guitar-ish noise ducks at the vocal's frequencies only
    {
        auto run = [] (bool vocal, float& at1k, float& at150, float& at9k)
        {
            auto main = noise (3.0, 0.2f, 11);
            auto dry = main;
            auto sc = vocal ? sine (1000.0f, 3.0, 0.25f) : std::vector<float> ((size_t) (3.0 * fs), 0.0f);
            vs::Carve c;
            c.prepare (fs);
            vs::Carve::Settings cs; cs.on = true; cs.depthDb = 6; cs.lowHz = 100; cs.highHz = 10000; cs.speed = 0.5f;
            c.setSettings (cs);
            float* ch[1] { main.data() };
            const float* sch[1] { sc.data() };
            for (int start = 0; start < (int) main.size(); start += 32)
                c.process (ch, 1, sch, 1, start, std::min (32, (int) main.size() - start));
            at1k = bandDb (main, 1000, (size_t) fs) - bandDb (dry, 1000, (size_t) fs);
            at150 = bandDb (main, 150, (size_t) fs) - bandDb (dry, 150, (size_t) fs);
            at9k = bandDb (main, 9000, (size_t) fs) - bandDb (dry, 9000, (size_t) fs);
        };
        float a = 0, b = 0, c = 0;
        run (true, a, b, c);
        expect ("carve dips main at the vocal's 1 kHz", a < -4.0f && a > -8.0f, juce::String (a, 1) + " dB");
        expect ("carve leaves 150 Hz and 9 kHz mostly alone", b > -1.5f && c > -1.5f, juce::String (b, 2) + " / " + juce::String (c, 2) + " dB");
        run (false, a, b, c);
        expect ("carve does nothing when the vocal is silent", std::abs (a) < 0.1f && std::abs (b) < 0.1f, juce::String (a, 3) + " dB");
    }

    // 6. Resonance finder: same averaging the display uses (Hann, 4096 FFT)
    {
        auto avgPower = [] (const std::vector<float>& x)
        {
            constexpr int n = 4096;
            juce::dsp::FFT fft (12);
            std::vector<float> buf ((size_t) n * 2), pw ((size_t) n / 2 + 1, 0.0f);
            int frames = 0;
            for (size_t pos = 0; pos + n <= x.size(); pos += n / 2, ++frames)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                for (int i = 0; i < n; ++i)
                    buf[(size_t) i] = x[pos + (size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (n - 1)));
                fft.performFrequencyOnlyForwardTransform (buf.data(), true);
                for (size_t b = 0; b < pw.size(); ++b) { const float m = buf[b] / (n / 4); pw[b] += m * m; }
            }
            for (auto& v : pw) v /= (float) frames;
            return pw;
        };

        // pink-ish noise (one-pole lowpassed) with a ring at 2.5 kHz
        auto x = noise (4.0, 0.3f, 21);
        float lp = 0.0f;
        for (auto& v : x) { lp += 0.15f * (v - lp); v = lp; }
        auto ring = sine (2500.0f, 4.0, 0.05f);
        for (size_t i = 0; i < x.size(); ++i) x[i] += ring[i];

        auto res = vs::findResonances (avgPower (x), fs, 4096);
        const bool found = ! res.empty() && std::abs (std::log2 (res[0].freq / 2500.0f)) < 1.0f / 12.0f;
        expect ("finder spots a 2.5 kHz ring as the top resonance", found,
                res.empty() ? juce::String ("nothing found")
                            : juce::String (res[0].freq, 0) + " Hz, +" + juce::String (res[0].prominenceDb, 1) + " dB, " + juce::String ((int) res.size()) + " total");

        auto plain = noise (4.0, 0.3f, 22);
        lp = 0.0f;
        for (auto& v : plain) { lp += 0.15f * (v - lp); v = lp; }
        auto none = vs::findResonances (avgPower (plain), fs, 4096);
        expect ("finder stays quiet on plain noise", none.empty(), juce::String ((int) none.size()) + " false alarms");

        // and grabbing it should make a smart band that actually catches the ring
        if (found)
        {
            vs::DynamicBand d;
            d.prepare (fs);
            vs::DynamicBand::Settings ds; ds.on = true; ds.freq = res[0].freq; ds.q = 5.0f;
            ds.rangeDb = std::clamp (res[0].prominenceDb, 3.0f, 9.0f); ds.thresholdDb = res[0].relDb - 0.5f * res[0].prominenceDb; // same rule as the editor
            d.setSettings (ds);
            auto y = x;
            float* ch[1] { y.data() };
            for (int start = 0; start < (int) y.size(); start += 32) d.process (ch, 1, start, std::min (32, (int) y.size() - start));
            const float delta = bandDb (y, 2500, (size_t) fs) - bandDb (x, 2500, (size_t) fs);
            vs::Biquad det; det.c = vs::BiquadCoefs::bandPass (fs, res[0].freq, 5.0);
            double eb = 0, ef = 0;
            for (float v : x) { const float b = det.process (0, v); eb += (double) b * b; ef += (double) v * v; }
            expect ("a smart band made from it tames the ring", delta < -4.0f, juce::String (delta, 1) + " dB at 2.5 kHz (finder relDb "
                    + juce::String (res[0].relDb, 1) + ", band actually sees " + juce::String (10.0 * std::log10 (eb / ef), 1) + ")");
        }
    }

    // 7. CPU: 8 bands (2 sections avg) + 4 smart + carve, stereo
    {
        std::vector<vs::EqBand> bands (8);
        vs::DynamicBand smart[4];
        vs::Carve carve;
        for (auto& b : bands) { b.prepare (fs); B::Settings s; s.type = B::bell; s.gainDb = 3; b.setTarget (s); }
        for (auto& s : smart) { s.prepare (fs); vs::DynamicBand::Settings ds; ds.on = true; s.setSettings (ds); }
        carve.prepare (fs);
        vs::Carve::Settings cs; cs.on = true; carve.setSettings (cs);
        auto l = noise (10.0, 0.2f, 1), r = noise (10.0, 0.2f, 2), sc = noise (10.0, 0.2f, 5);
        float* ch[2] { l.data(), r.data() };
        const float* sch[1] { sc.data() };
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int start = 0; start < (int) l.size(); start += 32)
        {
            const int len = std::min (32, (int) l.size() - start);
            for (auto& b : bands) b.process (ch, 2, start, len);
            for (auto& s : smart) s.process (ch, 2, start, len);
            carve.process (ch, 2, sch, 1, start, len);
        }
        std::printf ("INFO EQ CPU, everything on: %.2f%% of one core (48 kHz stereo)\n",
                     (juce::Time::getMillisecondCounterHiRes() - t0) / 10000.0 * 100.0);
    }

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
