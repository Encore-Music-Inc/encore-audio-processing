#pragma once

#include "JuceInclude.h"
#include <array>
#include <atomic>
#include "Biquad.h"
#include "Loudness.h"
#include "Saturator.h"
#include "ReverbEngine.h"
#include "Util.h"

namespace vs
{
    // One-knob mastering chain.
    //
    //   learned pre-gain  ->  tone EQ (auto-matched + style + trims)
    //   ->  3-band glue compression  ->  saturation  ->  width (mono bass)
    //   ->  optional ambience  ->  drive  ->  true-peak limiter
    //
    // "Learning" (the Listen button): while the song plays, the chain measures
    // its loudness and tonal balance, normalises it to a fixed working level
    // (so every style behaves the same on any mix), sets the tone correction,
    // and rides the drive until the output hits the loudness target. After
    // ~20 s of music it locks, so nothing moves between verse and chorus.
    class MasterChain
    {
    public:
        static constexpr int numTone = 5, numStyles = 8, numAnalysis = 24;
        static constexpr float workingLufs = -18.0f;

        enum Style { universal = 0, fire, clarity, tape, natural, spatial, cinematic, punch };

        struct StyleDef
        {
            const char* name;
            const char* blurb;
            float tone[numTone];   // dB added at full intensity: low, low-mid, mid, presence, air
            float comp[3];         // glue amount per band (low, mid, high), 0..1
            bool fast;             // punchier attack/release
            float sat;             // saturation 0..1
            float width;           // 1 = unchanged
            float space;           // ambience send 0..1
            float matchStrength;   // how strongly to correct the mix's tonal balance
            float releaseMs;       // limiter release
            float monoBassHz;      // sides below this are removed (tight, centred low end)
        };

        static const StyleDef& style (int i) { return styleTable()[juce::jlimit (0, numStyles - 1, i)]; }

        // For calibration tools: adjust a style at runtime (shared by all instances)
        static void overrideStyle (int i, const float* tone5, const float* comp3, float sat, float width, float monoBassHz)
        {
            auto& d = styleTable()[juce::jlimit (0, numStyles - 1, i)];
            for (int b = 0; b < numTone; ++b) d.tone[b] = tone5[b];
            for (int b = 0; b < 3; ++b) d.comp[b] = comp3[b];
            d.sat = sat; d.width = width; d.monoBassHz = monoBassHz;
        }

    private:
        static StyleDef* styleTable()
        {
            // Universal, Fire, Clarity and Tape are fitted to measurements of
            // BandLab's same-named styles at "Normal" intensity (tone per
            // octave and dynamics, on "And it Rained", 2026-10-23). Tone values
            // apply at full intensity, so Normal (0.5) gets half.
            static StyleDef defs[numStyles] {
                //  name         blurb                                              tone: low, lowmid, mid, pres, air     glue low/mid/high       fast   sat    width  space match  rel  mono Hz
                { "Universal", "Balanced tone and solid glue for any mix",        { -2.84f, -1.01f, -1.42f, -1.16f, 3.81f },                   { 1.29f, 1.24f, 1.19f }, false, 0.10f, 1.05f, 0.00f, 1.0f, 80,  60 },
                { "Fire",      "Huge lows, scooped mids, bite up top",            { 15.34f, -8.49f, -6.72f, 0.33f, 4.83f },                   { 2.30f, 2.30f, 2.30f }, true,  0.30f, 1.00f, 0.00f, 1.0f, 60,  120 },
                { "Clarity",   "Open and airy: lows and highs up, mids clear",    { 1.09f, -1.65f, -6.10f, -0.51f, 5.30f },                   { 0.71f, 0.66f, 0.66f }, false, 0.05f, 1.05f, 0.00f, 1.0f, 120, 60 },
                { "Tape",      "Smooth upper mids, sparkle on top, warm glue",    { -4.80f, -0.48f, -3.75f, -6.46f, 8.14f },                   { 0.83f, 0.78f, 0.73f }, false, 0.40f, 1.00f, 0.00f, 1.0f, 100, 60 },
                { "Natural",   "Barely there: light glue, keeps the dynamics",    { 0, 0, 0, 0, 0 },                   { 0.20f, 0.20f, 0.15f }, false, 0.00f, 1.00f, 0.00f, 0.5f, 150, 60 },
                { "Spatial",   "Wide stereo and a touch of air and ambience",     { 0, -0.5f, 0, 0.5f, 1.0f },         { 0.30f, 0.30f, 0.25f }, false, 0.05f, 1.35f, 0.12f, 0.9f, 120, 60 },
                { "Cinematic", "Big, saturated and wide",                         { 1.5f, 0, -0.5f, 0.5f, 1.0f },      { 0.50f, 0.45f, 0.40f }, false, 0.70f, 1.25f, 0.06f, 0.9f, 100, 60 },
                { "Punch",     "Energetic bass and bright highs",                 { 2.0f, 0, 0, 1.0f, 1.5f },          { 0.50f, 0.45f, 0.40f }, true,  0.25f, 1.10f, 0.00f, 0.9f, 60,  60 },
            };
            return defs;
        }

    public:
        struct Settings
        {
            int style = universal;
            float intensity = 0.5f;     // 0..1, 0.5 = "Normal"
            float inputGainDb = 0.0f;
            float targetLufs = -14.0f;
            float ceilingDb = -1.0f;
            float toneTrim[numTone] {}; // user tweaks on top, dB
            // Broad user EQ (dB): low shelf 160 Hz, mid bell 1.2 kHz, high shelf 5 kHz.
            // Separate from the style's fitted tone bands, so it's easy to hear.
            float userEq[3] {};
            float glue = 0.5f;          // 0.5 = style default, 0 = none, 1 = double
            float widthTrim = 0.0f;     // -0.5..+0.5
            float satTrim = 0.0f;       // -1..+1
        };

        // What learning figured out; saved with the project
        struct Learned
        {
            bool valid = false;
            float preGainDb = 0.0f, driveDb = 6.0f;
            float toneMatch[numTone] {};
            float bandLevelDb[3] { -24.0f, -24.0f, -30.0f };
        };

        void prepare (double sampleRate, int maxBlock)
        {
            fs = sampleRate;
            const juce::dsp::ProcessSpec spec { fs, (juce::uint32) maxBlock, 2 };
            for (auto* f : { &lp1, &hp1, &lp2, &hp2, &ap2 }) { f->prepare (spec); f->reset(); }
            lp1.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);  lp1.setCutoffFrequency (150.0f);
            hp1.setType (juce::dsp::LinkwitzRileyFilterType::highpass); hp1.setCutoffFrequency (150.0f);
            lp2.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);  lp2.setCutoffFrequency (3000.0f);
            hp2.setType (juce::dsp::LinkwitzRileyFilterType::highpass); hp2.setCutoffFrequency (3000.0f);
            ap2.setType (juce::dsp::LinkwitzRileyFilterType::allpass);  ap2.setCutoffFrequency (3000.0f);
            sideHp.prepare (spec); sideHp.setType (juce::dsp::LinkwitzRileyFilterType::highpass); sideHp.setCutoffFrequency (120.0f);

            for (auto& b : bands) b.assign ((size_t) maxBlock, 0.0f);
            for (auto& b : bandsR) b.assign ((size_t) maxBlock, 0.0f);
            wetL.assign ((size_t) maxBlock, 0.0f); wetR.assign ((size_t) maxBlock, 0.0f);

            saturator.prepare (fs, maxBlock);
            limiter.prepare (fs, maxBlock);
            reverb.prepare (fs, maxBlock);
            ReverbEngine::Settings rs; rs.mode = ReverbEngine::hall; rs.decaySec = 1.8f; rs.lowCutHz = 300.0f; rs.toneHz = 7000.0f; rs.duck = 0.4f; rs.predelayMs = 25.0f;
            reverb.setSettings (rs);

            inMeter.prepare (fs); outMeter.prepare (fs); preLimMeter.prepare (fs);
            for (int k = 0; k < numAnalysis; ++k)
            {
                analysis[k].c = BiquadCoefs::bandPass (fs, analysisFreq (k), 4.3);
                analysis[k].reset();
            }
            for (auto& e : bandEnv) e = 1e-6f;
            for (auto& g : grDb) g = 0.0f;
            for (auto& t : tone) t.reset();
            for (auto& t : user) t.reset();
            lastTone[0] = 999.0f;
            lastUser[0] = lastUser[1] = lastUser[2] = 999.0f;
            preGain.reset (fs, 0.3); drive.reset (fs, 0.3);
            preGain.setCurrentAndTargetValue (dbToGain (learned.preGainDb));
            drive.setCurrentAndTargetValue (dbToGain (learned.driveDb));
            inGain.reset (fs, 0.05);
        }

        int getLatency() const { return saturator.getLatencySamples() + limiter.getLatency(); }

        void setSettings (const Settings& s)
        {
            // Anything that changes loudness (style, intensity, tone, glue...)
            // gets a few seconds of re-levelling so A/B-ing styles is fair.
            const bool changed = s.style != settings.style || s.intensity != settings.intensity || s.glue != settings.glue
                                 || s.satTrim != settings.satTrim || s.widthTrim != settings.widthTrim || s.targetLufs != settings.targetLufs
                                 || s.inputGainDb != settings.inputGainDb
                                 || ! std::equal (std::begin (s.toneTrim), std::end (s.toneTrim), std::begin (settings.toneTrim))
                                 || ! std::equal (std::begin (s.userEq), std::end (s.userEq), std::begin (settings.userEq));
            settings = s;
            if (changed && learned.valid && ! learning) resettle = true;
        }
        const Settings& getSettings() const { return settings; }
        const Learned& getLearned() const { return learned; }
        void setLearned (const Learned& l)
        {
            learned = l;
            preGain.setTargetValue (dbToGain (l.preGainDb));
            drive.setTargetValue (dbToGain (l.driveDb));
        }

        // Use results learned elsewhere (e.g. an offline pass over the whole
        // song) and stop any learning or re-levelling in progress.
        void lockLearned (const Learned& l)
        {
            setLearned (l);
            learning = false;
            resettle = false;
            rideSecondsLeft = 0.0;
        }

        // Start (or restart) learning this song
        void startLearning()
        {
            learning = true;
            learnedSeconds = 0.0;
            inMeter.reset();
            for (auto& a : analysisAcc) a = 0.0;
            analysisSeconds = 0.0;
            preGainLocked = false;
            toneApplied = false;
        }

        bool isLearning() const { return learning; }

        // Offline use: end learning now with whatever has been heard
        void finishLearningNow()
        {
            if (! learning) return;
            if (preGainLocked && ! toneApplied && analysisSeconds > 1.0) applyToneAndBands();
            learned.valid = learned.valid || preGainLocked;
            learning = false;
        }
        bool isSettling() const { return rideSecondsLeft > 0.0; }
        float learnProgress() const { return (float) juce::jlimit (0.0, 1.0, learnedSeconds / learnTime); }

        LoudnessMeter inMeter, outMeter, preLimMeter;
        TruePeakLimiter limiter;

        void process (float* l, float* r, int n)
        {
            const auto& st = style (settings.style);
            const float k = styleAmount (settings.intensity);
            float* ch[2] { l, r };

            // input gain
            inGain.setTargetValue (dbToGain (settings.inputGainDb));
            for (int i = 0; i < n; ++i) { const float g = inGain.getNextValue(); l[i] *= g; r[i] *= g; }
            inMeter.process (l, r, n);

            const bool hasSignal = inMeter.momentary.load() > -50.0f;

            // First music this instance has ever heard: learn automatically
            if (! learned.valid && ! learning && hasSignal) startLearning();
            if (resettle) { resettle = false; preLimMeter.reset(); rideSecondsLeft = 8.0; }

            // learned pre-gain to the working level
            for (int i = 0; i < n; ++i) { const float g = preGain.getNextValue(); l[i] *= g; r[i] *= g; }

            if (learning && hasSignal) analyse (l, r, n);

            // 3-band glue first, so it can't squash the style's EQ moves back down
            const float glueScale = settings.glue * 2.0f;
            glue (l, r, n, st, k * glueScale);

            // tone EQ
            float toneDb[numTone];
            for (int b = 0; b < numTone; ++b)
                toneDb[b] = juce::jlimit (-18.0f, 18.0f, (learned.toneMatch[b] * st.matchStrength + st.tone[b]) * k + settings.toneTrim[b]);
            updateTone (toneDb);
            updateUserEq();
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                {
                    float x = ch[c][i];
                    for (auto& t : tone) x = t.process (c, x);
                    for (auto& t : user) x = t.process (c, x);
                    ch[c][i] = x;
                }

            // saturation
            const float sat = juce::jlimit (0.0f, 1.0f, st.sat * k + settings.satTrim * 0.5f);
            saturator.setDrive (sat * 0.6f);
            juce::dsp::AudioBlock<float> block (ch, 2, (size_t) n);
            saturator.process (block);

            // width with mono bass
            const float width = juce::jlimit (0.0f, 2.0f, 1.0f + (st.width - 1.0f) * k + settings.widthTrim);
            if (st.monoBassHz != lastMonoHz) { lastMonoHz = st.monoBassHz; sideHp.setCutoffFrequency (st.monoBassHz); }
            for (int i = 0; i < n; ++i)
            {
                const float m = 0.5f * (l[i] + r[i]);
                float s = 0.5f * (l[i] - r[i]);
                s = sideHp.processSample (0, s) * width; // centred low end
                l[i] = m + s; r[i] = m - s;
            }

            // ambience
            const float space = st.space * k;
            if (space > 0.001f || reverbTail > 0)
            {
                reverb.process (l, r, wetL.data(), wetR.data(), n);
                for (int i = 0; i < n; ++i) { l[i] += wetL[(size_t) i] * space; r[i] += wetR[(size_t) i] * space; }
                reverbTail = space > 0.001f ? (int) (fs * 3) : std::max (0, reverbTail - n);
            }

            // drive into the limiter
            preLimMeter.process (l, r, n);
            if (hasSignal && ((learning && preGainLocked) || rideSecondsLeft > 0.0)) rideDrive (n);
            rideSecondsLeft = std::max (0.0, rideSecondsLeft - (double) n / fs);
            for (int i = 0; i < n; ++i) { const float g = drive.getNextValue(); l[i] *= g; r[i] *= g; }

            limiter.setCeilingDb (settings.ceilingDb);
            limiter.setReleaseMs (st.releaseMs, fs);
            limiter.process (block);
            outMeter.process (l, r, n);
        }

        float glueGrDb (int band) const { return grDb[band]; }
        float toneDbNow (int b) const { return lastTone[b]; }

    private:
        static float analysisFreq (int k) { return 40.0f * std::pow (16000.0f / 40.0f, (float) k / (numAnalysis - 1)); }
        // How much of the style to apply. 0..0.5 (Light..Normal) is the fitted
        // range; above Normal it climbs faster, so Heavy is 3x Normal.
        static float styleAmount (float intensity)
        {
            const float i = juce::jlimit (0.0f, 1.0f, intensity);
            return i <= 0.5f ? i : 0.5f + (i - 0.5f) * 2.0f;
        }

        void updateUserEq()
        {
            bool changed = false;
            for (int b = 0; b < 3; ++b) changed = changed || std::abs (settings.userEq[b] - lastUser[b]) > 0.01f;
            if (! changed) return;
            for (int b = 0; b < 3; ++b)
            {
                const float want = juce::jlimit (-12.0f, 12.0f, settings.userEq[b]);
                lastUser[b] += (lastUser[b] > 900.0f) ? want - lastUser[b] : 0.2f * (want - lastUser[b]); // glide
            }
            user[0].c = BiquadCoefs::lowShelf (fs, 160.0, 0.7, lastUser[0]);
            user[1].c = BiquadCoefs::peak (fs, 1200.0, 0.7, lastUser[1]);
            user[2].c = BiquadCoefs::highShelf (fs, 5000.0, 0.7, lastUser[2]);
        }

        static float toneFreq (int b) { static const float f[numTone] { 75.0f, 300.0f, 1500.0f, 3500.0f, 9500.0f }; return f[b]; }

        void updateTone (const float* db)
        {
            bool changed = false;
            for (int b = 0; b < numTone; ++b) changed = changed || std::abs (db[b] - lastTone[b]) > 0.01f;
            if (! changed) return;
            for (int b = 0; b < numTone; ++b)
            {
                lastTone[b] += (lastTone[b] > 900.0f) ? db[b] - lastTone[b] : 0.2f * (db[b] - lastTone[b]); // glide
                const double g = lastTone[b];
                static const double q[numTone] { 0.7, 0.8, 0.6, 0.9, 0.7 };
                if (b == 0) tone[b].c = BiquadCoefs::lowShelf (fs, toneFreq (b), q[b], g);
                else        tone[b].c = BiquadCoefs::peak (fs, toneFreq (b), q[b], g);
            }
        }

        // Split at 150 Hz and 3 kHz (Linkwitz-Riley, sums flat), compress each
        // band gently, recombine. Thresholds come from the song's own band
        // levels, so the glue engages on the louder moments of *this* mix.
        void glue (float* l, float* r, int n, const StyleDef& st, float amount)
        {
            if (amount < 0.001f) { for (auto& g : grDb) g = 0.0f; return; }
            float* src[2] { l, r };
            for (int c = 0; c < 2; ++c)
            {
                auto& b = c == 0 ? bands : bandsR;
                for (int i = 0; i < n; ++i)
                {
                    const float x = src[c][i];
                    const float low = lp1.processSample (c, x), rest = hp1.processSample (c, x);
                    b[0][(size_t) i] = ap2.processSample (c, low); // keep the low band phase-aligned with the 3 kHz split
                    b[1][(size_t) i] = lp2.processSample (c, rest);
                    b[2][(size_t) i] = hp2.processSample (c, rest);
                }
            }

            static const float atkMs[3] { 30.0f, 15.0f, 5.0f }, relMs[3] { 200.0f, 150.0f, 80.0f };
            for (int band = 0; band < 3; ++band)
            {
                // up to 1.5: ratio ~5.5:1 for the heaviest styles (Fire)
                const float a = juce::jlimit (0.0f, 1.5f, st.comp[band] * amount);
                const float thr = learned.bandLevelDb[band] + 6.0f - a * 10.0f;
                const float ratio = 1.0f + a * 3.0f;
                const float speed = st.fast ? 0.5f : 1.0f;
                const float atk = onePole (atkMs[band] * speed, fs), rel = onePole (relMs[band] * speed, fs);
                float maxGr = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    const float xl = bands[(size_t) band][(size_t) i], xr = bandsR[(size_t) band][(size_t) i];
                    const float sq = std::max (xl * xl, xr * xr);
                    bandEnv[band] += (sq > bandEnv[band] ? atk : rel) * (sq - bandEnv[band]);
                    const float lvl = powerToDb (bandEnv[band]);
                    const float over = lvl - thr;
                    // 6 dB soft knee
                    float gr = 0.0f;
                    if (over > 3.0f) gr = over * (1.0f - 1.0f / ratio);
                    else if (over > -3.0f) gr = (1.0f - 1.0f / ratio) * (over + 3.0f) * (over + 3.0f) / 12.0f;
                    grDb[band] = gr;
                    maxGr = std::max (maxGr, gr);
                    const float g = dbToGain (-gr);
                    bands[(size_t) band][(size_t) i] = xl * g;
                    bandsR[(size_t) band][(size_t) i] = xr * g;
                }
                grDb[band] = maxGr;
            }
            for (int i = 0; i < n; ++i)
            {
                l[i] = bands[0][(size_t) i] + bands[1][(size_t) i] + bands[2][(size_t) i];
                r[i] = bandsR[0][(size_t) i] + bandsR[1][(size_t) i] + bandsR[2][(size_t) i];
            }
        }

        void analyse (const float* l, const float* r, int n)
        {
            const double sec = (double) n / fs;
            learnedSeconds += sec;

            // After 3 s of music, set the pre-gain from the input loudness
            if (! preGainLocked && learnedSeconds > 3.0)
            {
                const float in = inMeter.integrated();
                if (in > -60.0f)
                {
                    learned.preGainDb = juce::jlimit (-24.0f, 24.0f, workingLufs - in);
                    preGain.setTargetValue (dbToGain (learned.preGainDb));
                    preGainLocked = true;
                    preLimMeter.reset(); // measure the chain from here on
                }
                return;
            }
            if (! preGainLocked) return;

            // 3-12 s: tonal balance + band levels (measured post pre-gain, pre-EQ)
            if (! toneApplied)
            {
                analysisSeconds += sec;
                for (int i = 0; i < n; ++i)
                {
                    const float m = 0.5f * (l[i] + r[i]);
                    for (int k = 0; k < numAnalysis; ++k)
                    {
                        const float b = analysis[k].process (0, m);
                        analysisAcc[k] += (double) b * b;
                    }
                }
                if (learnedSeconds >= toneTime)
                {
                    applyToneAndBands();
                    preLimMeter.reset(); // 12-20 s: set loudness with the tone already in place
                }
                return;
            }

            if (learnedSeconds >= learnTime)
            {
                learned.valid = true;
                learning = false;
            }
        }

        // Set the drive so the output lands on the target. The level going
        // into the drive (pre-limiter) doesn't depend on the drive, so this
        // isn't a feedback loop and can't oscillate; the only feedback is the
        // limiter's own loudness loss, which is small and averaged.
        void rideDrive (int n)
        {
            rideAcc += n;
            if (rideAcc < (int) (fs * 0.1)) return;
            rideAcc = 0;

            const float pre = preLimMeter.integrated();
            if (pre < -60.0f) return;

            const float preM = preLimMeter.momentary.load(), outM = outMeter.momentary.load();
            if (outM > -50.0f && preM > -50.0f)
                lossEma += 0.1f * (juce::jlimit (0.0f, 12.0f, preM + learned.driveDb - outM) - lossEma);

            learned.driveDb = juce::jlimit (-6.0f, 24.0f, settings.targetLufs - pre + lossEma);
            drive.setTargetValue (dbToGain (learned.driveDb));
        }

        void applyToneAndBands()
        {
            // Tone match: compare the song's 1/3-octave balance to a "finished
            // master" reference (flat in 1/3-octave power from 100 Hz to 2 kHz,
            // a slight low-end lift, gently falling top), then fold the
            // difference into the five tone bands.
            double bandDb[numAnalysis];
            for (int k = 0; k < numAnalysis; ++k)
                bandDb[k] = 10.0 * std::log10 (analysisAcc[k] / std::max (1.0, analysisSeconds * fs) + 1e-20);

            auto reference = [] (float f)
            {
                float db = 0.0f;
                if (f < 100.0f) db += 1.5f * std::log2 (100.0f / f) * 0.7f;
                if (f > 2000.0f) db -= 2.5f * std::log2 (f / 2000.0f);
                return db;
            };

            double diff[numAnalysis], mean = 0.0;
            for (int k = 0; k < numAnalysis; ++k)
            {
                diff[k] = reference (analysisFreq (k)) - bandDb[k];
                if (analysisFreq (k) > 150.0f && analysisFreq (k) < 6000.0f) mean += diff[k];
            }
            int midCount = 0;
            for (int k = 0; k < numAnalysis; ++k) if (analysisFreq (k) > 150.0f && analysisFreq (k) < 6000.0f) ++midCount;
            mean /= std::max (1, midCount);

            static const float lo[numTone] { 40.0f, 150.0f, 500.0f, 2000.0f, 7000.0f }, hi[numTone] { 120.0f, 400.0f, 1600.0f, 5000.0f, 14000.0f };
            for (int b = 0; b < numTone; ++b)
            {
                double s = 0.0; int c = 0;
                for (int k = 0; k < numAnalysis; ++k)
                    if (analysisFreq (k) >= lo[b] && analysisFreq (k) <= hi[b]) { s += diff[k] - mean; ++c; }
                // correct a third of the gap, never more than 3 dB: a nudge on
                // top of the style's own curve, not a re-mix
                learned.toneMatch[b] = c > 0 ? juce::jlimit (-3.0f, 3.0f, (float) (0.35 * s / c)) : 0.0f;
            }

            learned.bandLevelDb[0] = bandLevelEstimate (0);
            learned.bandLevelDb[1] = bandLevelEstimate (1);
            learned.bandLevelDb[2] = bandLevelEstimate (2);

            toneApplied = true;
        }

        float bandLevelEstimate (int band) const
        {
            // power of the analysis bands that fall in this glue band
            static const float lo[3] { 20.0f, 150.0f, 3000.0f }, hi[3] { 150.0f, 3000.0f, 20000.0f };
            double s = 0.0;
            for (int k = 0; k < numAnalysis; ++k)
                if (analysisFreq (k) >= lo[band] && analysisFreq (k) < hi[band]) s += analysisAcc[k];
            // 1/3-octave-ish bands overlap ~2x and cover the range: rough but stable
            const double meanSq = s / std::max (1.0, analysisSeconds * fs) * 0.5;
            return juce::jlimit (-60.0f, 0.0f, (float) (10.0 * std::log10 (meanSq + 1e-20)) + 3.0f);
        }

        double fs = 48000.0;
        Settings settings;
        Learned learned;
        bool learning = false, preGainLocked = false;
        double learnedSeconds = 0.0, analysisSeconds = 0.0;
        static constexpr double learnTime = 20.0, toneTime = 12.0;
        bool toneApplied = false;
        int rideAcc = 0, reverbTail = 0;

        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> inGain, preGain, drive;
        Biquad tone[numTone];
        float lastTone[numTone] { 999.0f, 999.0f, 999.0f, 999.0f, 999.0f };
        Biquad user[3];
        float lastUser[3] { 999.0f, 999.0f, 999.0f };

        juce::dsp::LinkwitzRileyFilter<float> lp1, hp1, lp2, hp2, ap2, sideHp;
        float lastMonoHz = -1.0f;
        std::array<std::vector<float>, 3> bands, bandsR;
        float bandEnv[3] {}, grDb[3] {};
        std::vector<float> wetL, wetR;

        Saturator saturator;
        ReverbEngine reverb;

        Biquad analysis[numAnalysis];
        double analysisAcc[numAnalysis] {};
        double rideSecondsLeft = 0.0;
        float lossEma = 0.0f;
        bool resettle = false;
    };
}
