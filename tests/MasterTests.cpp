// Offline checks for the mastering chain. Run: build\MasterTests_artefacts\Release\MasterTests.exe
#include <juce_dsp/juce_dsp.h>
#include <cstdio>
#include "dsp/MasterChain.h"

namespace
{
    constexpr double fs = 48000.0;
    int failures = 0;

    void expect (const juce::String& name, bool ok, const juce::String& detail = {})
    {
        std::printf ("%s %-52s %s\n", ok ? "PASS" : "FAIL", name.toRawUTF8(), detail.toRawUTF8());
        if (! ok) ++failures;
    }

    struct Stereo { std::vector<float> l, r; };

    Stereo sine (float hz, double sec, float dbfs, bool left = true, bool right = true)
    {
        const float a = std::pow (10.0f, dbfs / 20.0f);
        Stereo s { std::vector<float> ((size_t) (sec * fs)), std::vector<float> ((size_t) (sec * fs)) };
        for (size_t i = 0; i < s.l.size(); ++i)
        {
            const float v = a * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (double) i / fs);
            s.l[i] = left ? v : 0.0f;
            s.r[i] = right ? v : 0.0f;
        }
        return s;
    }

    // "Music-ish": pinkish noise with a beat (kick-like thumps + snare-ish bursts),
    // slightly different left/right. 'tilt' shapes the tone: <0 dull, >0 bright.
    Stereo music (double sec, float gain, int seed = 1, float tilt = 0.0f)
    {
        juce::Random r (seed);
        Stereo s { std::vector<float> ((size_t) (sec * fs)), std::vector<float> ((size_t) (sec * fs)) };
        float b[2][3] {};
        vs::Biquad tone;
        tone.c = tilt >= 0 ? vs::BiquadCoefs::highShelf (fs, 3000.0, 0.7, tilt) : vs::BiquadCoefs::highShelf (fs, 2000.0, 0.7, tilt);
        for (size_t i = 0; i < s.l.size(); ++i)
        {
            const double t = (double) i / fs, beat = std::fmod (t, 0.5);
            const float kick = (float) (std::exp (-beat * 30.0) * std::sin (juce::MathConstants<double>::twoPi * 55.0 * beat) * 0.9);
            const float snareEnv = (float) (std::fmod (t + 0.25, 1.0) < 0.12 ? std::exp (-std::fmod (t + 0.25, 1.0) * 25.0) : 0.0);
            for (int c = 0; c < 2; ++c)
            {
                const float w = r.nextFloat() * 2.0f - 1.0f;
                b[c][0] = 0.99f * b[c][0] + 0.05f * w; b[c][1] = 0.9f * b[c][1] + 0.1f * w; b[c][2] = 0.5f * b[c][2] + 0.2f * w;
                const float pink = (b[c][0] + b[c][1] + b[c][2]) * 0.6f;
                const float v = (pink * 0.35f + kick + snareEnv * w * 0.6f) * gain;
                (c == 0 ? s.l : s.r)[i] = tone.process (c, v);
            }
        }
        return s;
    }

    float lufsOf (const Stereo& s, size_t from = 0)
    {
        vs::LoudnessMeter m;
        m.prepare (fs);
        m.process (s.l.data() + from, s.r.data() + from, (int) (s.l.size() - from));
        return m.integrated();
    }

    // 4x-oversampled peak, the way a true-peak meter measures it
    float truePeakDb (const Stereo& s)
    {
        juce::dsp::Oversampling<float> os (2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true);
        os.initProcessing (512);
        float peak = 0.0f;
        std::vector<float> l (512), r (512);
        for (size_t start = 0; start < s.l.size(); start += 512)
        {
            const size_t len = std::min ((size_t) 512, s.l.size() - start);
            std::copy (s.l.begin() + (long) start, s.l.begin() + (long) (start + len), l.begin());
            std::copy (s.r.begin() + (long) start, s.r.begin() + (long) (start + len), r.begin());
            float* ch[2] { l.data(), r.data() };
            juce::dsp::AudioBlock<float> blk (ch, 2, len);
            auto up = os.processSamplesUp (blk);
            for (size_t c = 0; c < 2; ++c)
                for (size_t i = 0; i < up.getNumSamples(); ++i) peak = std::max (peak, std::abs (up.getSample ((int) c, (int) i)));
        }
        return 20.0f * std::log10 (peak + 1e-9f);
    }

    float samplePeakDb (const Stereo& s)
    {
        float p = 0.0f;
        for (size_t i = 0; i < s.l.size(); ++i) p = std::max ({ p, std::abs (s.l[i]), std::abs (s.r[i]) });
        return 20.0f * std::log10 (p + 1e-9f);
    }

    Stereo runChain (vs::MasterChain& chain, Stereo in)
    {
        for (size_t start = 0; start < in.l.size(); start += 512)
        {
            const int len = (int) std::min ((size_t) 512, in.l.size() - start);
            chain.process (in.l.data() + start, in.r.data() + start, len);
        }
        return in;
    }

    bool finite (const Stereo& s)
    {
        for (size_t i = 0; i < s.l.size(); ++i) if (! std::isfinite (s.l[i]) || ! std::isfinite (s.r[i])) return false;
        return true;
    }
}

int main()
{
    // 1. Loudness meter against the EBU Tech 3341 reference signals
    {
        float got = lufsOf (sine (1000.0f, 20.0, -23.0f));
        expect ("EBU 3341 #1: stereo 1 kHz at -23 dBFS = -23.0 LUFS", std::abs (got + 23.0f) < 0.1f, juce::String (got, 2) + " LUFS");
        got = lufsOf (sine (1000.0f, 20.0, -33.0f));
        expect ("EBU 3341 #2: stereo 1 kHz at -33 dBFS = -33.0 LUFS", std::abs (got + 33.0f) < 0.1f, juce::String (got, 2) + " LUFS");

        auto a = sine (1000.0f, 10.0, -36.0f), b = sine (1000.0f, 60.0, -23.0f), c = sine (1000.0f, 10.0, -36.0f);
        Stereo all;
        for (auto* p : { &a, &b, &c }) { all.l.insert (all.l.end(), p->l.begin(), p->l.end()); all.r.insert (all.r.end(), p->r.begin(), p->r.end()); }
        got = lufsOf (all);
        expect ("EBU 3341 #3: gating ignores quiet intro/outro", std::abs (got + 23.0f) < 0.1f, juce::String (got, 2) + " LUFS");

        got = lufsOf (sine (997.0f, 20.0, 0.0f, true, false));
        expect ("BS.1770: 0 dBFS 997 Hz in one channel = -3.01 LKFS", std::abs (got + 3.01f) < 0.1f, juce::String (got, 2));
    }

    // 2. Limiter: nothing over the ceiling, even between samples
    {
        vs::TruePeakLimiter lim;
        lim.prepare (fs, 512);
        lim.setCeilingDb (-1.0f);
        auto in = music (10.0, 4.0f, 3); // very hot
        // add a few nasty single-sample spikes and near-Nyquist content
        for (size_t i = 10000; i < in.l.size(); i += 37000) { in.l[i] = 6.0f; in.r[i] = -6.0f; }
        auto out = in;
        for (size_t start = 0; start < out.l.size(); start += 512)
        {
            const size_t len = std::min ((size_t) 512, out.l.size() - start);
            float* ch[2] { out.l.data() + start, out.r.data() + start };
            juce::dsp::AudioBlock<float> blk (ch, 2, len);
            lim.process (blk);
        }
        const float sp = samplePeakDb (out), tp = truePeakDb (out);
        expect ("limiter: sample peaks never above -1 dBFS", sp <= -1.0f + 0.01f, juce::String (sp, 2) + " dBFS");
        expect ("limiter: true peak at or under the -1 dBTP ceiling", tp <= -0.95f, juce::String (tp, 2) + " dBTP (ceiling -1)");

        vs::TruePeakLimiter lim2;
        lim2.prepare (fs, 512);
        lim2.setCeilingDb (-1.0f);
        auto quiet = sine (440.0f, 2.0, -12.0f);
        auto q = quiet;
        for (size_t start = 0; start < q.l.size(); start += 512)
        {
            const size_t len = std::min ((size_t) 512, q.l.size() - start);
            float* ch[2] { q.l.data() + start, q.r.data() + start };
            juce::dsp::AudioBlock<float> blk (ch, 2, len);
            lim2.process (blk);
        }
        expect ("limiter: leaves material under the ceiling alone", std::abs (lufsOf (q, (size_t) fs) - lufsOf (quiet, (size_t) fs)) < 0.1f);
    }

    // 3. Every style learns the song and lands on the target, quiet or hot mix
    const char* names[] { "Universal", "Fire", "Clarity", "Tape", "Natural", "Spatial", "Cinematic", "Punch" };
    for (int st = 0; st < vs::MasterChain::numStyles; ++st)
    {
        for (float inGain : { 0.08f, 0.9f })
        {
            vs::MasterChain chain;
            chain.prepare (fs, 512);
            vs::MasterChain::Settings s; s.style = st; s.targetLufs = -14.0f;
            chain.setSettings (s);
            auto in = music (40.0, inGain, 5 + st);
            const float inLufs = lufsOf (in);
            auto out = runChain (chain, in);
            const float got = lufsOf (out, (size_t) (30 * fs));
            const float tp = truePeakDb (out);
            const bool ok = finite (out) && std::abs (got + 14.0f) < 1.0f && tp <= -0.95f && ! chain.isLearning();
            expect (juce::String (names[st]) + (inGain < 0.5f ? " (quiet mix)" : " (hot mix)") + " -> -14 LUFS", ok,
                    "in " + juce::String (inLufs, 1) + ", out " + juce::String (got, 2) + " LUFS, TP " + juce::String (tp, 2));
        }
    }

    // 4. Louder targets
    for (float target : { -11.0f, -9.0f })
    {
        vs::MasterChain chain;
        chain.prepare (fs, 512);
        vs::MasterChain::Settings s; s.style = vs::MasterChain::fire; s.targetLufs = target;
        chain.setSettings (s);
        auto out = runChain (chain, music (40.0, 0.2f, 9));
        const float got = lufsOf (out, (size_t) (30 * fs));
        expect ("Fire -> " + juce::String ((int) target) + " LUFS", std::abs (got - target) < 1.0f, juce::String (got, 2) + " LUFS, TP " + juce::String (truePeakDb (out), 2));
    }

    // 5. Switching style after learning re-levels to the target
    {
        vs::MasterChain chain;
        chain.prepare (fs, 512);
        vs::MasterChain::Settings s; s.style = vs::MasterChain::natural;
        chain.setSettings (s);
        runChain (chain, music (25.0, 0.2f, 11));
        s.style = vs::MasterChain::cinematic; s.intensity = 1.0f;
        chain.setSettings (s);
        auto out = runChain (chain, music (20.0, 0.2f, 12));
        const float got = lufsOf (out, (size_t) (12 * fs));
        expect ("Natural -> Cinematic after learning stays at -14", std::abs (got + 14.0f) < 1.0f, juce::String (got, 2) + " LUFS");
    }

    // 6. Tone matching moves the right way
    {
        auto learnTone = [] (float tilt)
        {
            vs::MasterChain chain;
            chain.prepare (fs, 512);
            chain.setSettings ({});
            runChain (chain, music (25.0, 0.2f, 13, tilt));
            return chain.getLearned();
        };
        const auto dull = learnTone (-12.0f), bright = learnTone (9.0f);
        expect ("dull mix gets more air than a bright one", dull.toneMatch[4] > bright.toneMatch[4] + 2.0f,
                "air: dull " + juce::String (dull.toneMatch[4], 1) + " dB, bright " + juce::String (bright.toneMatch[4], 1) + " dB");
        bool bounded = true;
        for (auto* l : { &dull, &bright }) for (float v : l->toneMatch) bounded = bounded && std::abs (v) <= 4.0f;
        expect ("tone corrections stay within +/-4 dB", bounded);
    }

    // 7. Silence in, silence out (no noise, no NaN, no learning from nothing)
    {
        vs::MasterChain chain;
        chain.prepare (fs, 512);
        chain.setSettings ({});
        Stereo z { std::vector<float> ((size_t) (5 * fs), 0.0f), std::vector<float> ((size_t) (5 * fs), 0.0f) };
        auto out = runChain (chain, z);
        expect ("silence stays silent", finite (out) && samplePeakDb (out) < -100.0f && ! chain.getLearned().valid,
                juce::String (samplePeakDb (out), 1) + " dBFS");
    }

    // 8. CPU
    {
        vs::MasterChain chain;
        chain.prepare (fs, 512);
        vs::MasterChain::Settings s; s.style = vs::MasterChain::cinematic; s.intensity = 1.0f;
        chain.setSettings (s);
        auto in = music (20.0, 0.2f, 21);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        runChain (chain, in);
        std::printf ("INFO Master CPU (Cinematic, full intensity): %.2f%% of one core at 48 kHz\n",
                     (juce::Time::getMillisecondCounterHiRes() - t0) / 20000.0 * 100.0);
    }

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
