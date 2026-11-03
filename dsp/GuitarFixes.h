#pragma once

#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <vector>
#include "Biquad.h"
#include "Util.h"

// Strum's dynamic fixes for acoustic guitar: Squeak, Pick and Boom. Each one
// listens to the incoming guitar (mono sum), decides how much to cut, and
// turns down one frequency range with a bell that's exactly flat (0 dB, so
// bit-for-bit pass-through) whenever it isn't needed. Detection is relative
// (a band against its own recent past, or against another band), so one
// setting works on a quiet fingerpicked part and a hard strum alike.
// Zero latency: nothing looks ahead.
namespace vs
{
    // A bell cut whose depth changes over time. Coefficients are refreshed
    // per sub-block (16 samples) when the cut moves by more than 0.05 dB.
    class DynamicBell
    {
    public:
        void prepare (double sampleRate, float freqHz, float bellQ)
        {
            fs = sampleRate; freq = freqHz; q = bellQ;
            eq.reset();
            eq.c = BiquadCoefs {};
            applied = 0.0f;
        }

        void setFreq (float hz)
        {
            if (std::abs (hz - freq) < 0.01f) return;
            freq = hz;
            applied = -1.0f; // force a redesign
        }

        void process (float* const* ch, int numCh, int start, int len, float cutDb)
        {
            if (std::abs (cutDb - applied) > 0.05f)
            {
                applied = cutDb;
                eq.c = cutDb < 0.01f ? BiquadCoefs {} : BiquadCoefs::peak (fs, freq, q, -cutDb);
            }
            for (int c = 0; c < numCh; ++c)
                for (int i = start; i < start + len; ++i)
                    ch[c][i] = eq.process (c, ch[c][i]);
        }

        void reset() { eq.reset(); eq.c = BiquadCoefs {}; applied = 0.0f; }

    private:
        double fs = 48000.0;
        float freq = 1000.0f, q = 1.0f, applied = 0.0f;
        Biquad eq;
    };

    // Attack/release smoothing of a cut amount (dB), per sub-block
    inline void glideCut (float& cut, float target, float blockSec, float attackSec, float releaseSec)
    {
        const float tau = target > cut ? attackSec : releaseSec;
        cut += (1.0f - std::exp (-blockSec / tau)) * (target - cut);
        if (cut < 0.001f && target <= 0.0f) cut = 0.0f;
    }

    //==============================================================================
    // Squeak: finger noise on wound strings during slides and chord changes.
    // A squeak is a short noisy burst, mostly 1.5-8 kHz, that arrives without
    // a new note and stands far above the guitar's own top end. Squeak looks at:
    //
    //   1. how far the high band (1.5-8 kHz) jumps above the lowest it was in
    //      the last 30 ms. A new strum over strings that are still ringing
    //      lifts it by roughly 5-11 dB (in the tests; even over-bright
    //      white-noise strings stayed near 12); the test squeaks jumped 15-23 dB.
    //      The jump needed is 16 dB at 0%, 13 dB at 50%, 10 dB at 100%.
    //   2. the mids (400 Hz-1.2 kHz), against their average over the 30 ms
    //      before. A chord's mids wobble by about +-35% within 30 ms on their
    //      own (strings beat against each other), so this can't tell a squeak
    //      from a re-strum. But a strum after a pause or a muted beat moves
    //      them far more than that, so if they move by over 60% within the
    //      first 15 ms, it was an attack and Squeak lets go.
    //
    // Once a squeak is detected, the high band is pulled back toward the level
    // just before it until it dies away (400 ms at most). Nothing looks ahead,
    // so the swell up to the jump gets through. Squeaks that don't stand well
    // above the guitar's own highs are left alone: it fails safe, toward doing
    // nothing, rather than ever dulling a strum.
    class SqueakTamer
    {
    public:
        void prepare (double sampleRate)
        {
            fs = sampleRate;
            hiA.c = BiquadCoefs::highPass (fs, 1500.0, 0.7071);
            hiB.c = BiquadCoefs::lowPass (fs, std::min (8000.0, fs * 0.45), 0.7071);
            midA.c = BiquadCoefs::highPass (fs, 400.0, 0.7071);
            midB.c = BiquadCoefs::lowPass (fs, 1200.0, 0.7071);
            for (auto* b : { &hiA, &hiB, &midA, &midB }) b->reset();
            hiAtk = onePole (1.0f, fs);  hiRel = onePole (12.0f, fs);
            smoothC = onePole (4.0f, fs); // two in series: mid power that ignores the fastest beating
            eh = 1.0e-12f;
            sM = sM2 = 0.0f;
            histLen = std::max (4, (int) std::ceil (0.03 * fs / sub));
            histDb.assign ((size_t) histLen, -120.0f);
            histM.assign ((size_t) histLen, 0.0f);
            histPos = 0;
            holdSec = activeSec = 0.0f;
            refDb = -120.0f;
            refM = 0.0f;
            active = false;
            cutDb = 0.0f;
            bell.prepare (fs, 3400.0f, 0.55f); // spans roughly 1.5-8 kHz
        }

        void process (float* const* ch, int numCh, int n, float amount, bool on)
        {
            const float thr = 16.0f - 6.0f * amount;  // how far the high band must jump (dB)
            const float pull = 0.6f + 0.4f * amount;  // how much of the jump is taken back
            const float maxCut = 4.0f + 14.0f * amount;
            float maxSeen = 0.0f;

            for (int start = 0; start < n; start += sub)
            {
                const int len = std::min (sub, n - start);
                const float dt = (float) len / (float) fs;
                for (int i = start; i < start + len; ++i)
                {
                    float m = 0.0f;
                    for (int c = 0; c < numCh; ++c) m += ch[c][i];
                    m /= (float) numCh;
                    const float h = hiB.process (0, hiA.process (0, m));
                    const float md = midB.process (0, midA.process (0, m));
                    eh += (h * h > eh ? hiAtk : hiRel) * (h * h - eh);
                    sM += smoothC * (md * md - sM);  sM2 += smoothC * (sM - sM2);
                }

                // jumps vs. the last 30 ms
                const float hDb = powerToDb (eh);
                float minDb = hDb, avgM = 0.0f;
                for (int k = 0; k < histLen; ++k)
                {
                    minDb = std::min (minDb, histDb[(size_t) k]);
                    avgM += histM[(size_t) k];
                }
                avgM /= (float) histLen;
                histDb[(size_t) histPos] = hDb;
                histM[(size_t) histPos] = sM2;
                histPos = (histPos + 1) % histLen;

                const float riseH = hDb - minDb;
                holdSec = std::max (0.0f, holdSec - dt);
                if (! active && holdSec <= 0.0f && hDb > -75.0f && riseH > thr)
                {
                    active = true;
                    activeSec = 0.0f;
                    refDb = minDb;
                    refM = avgM;
                }

                float target = 0.0f;
                if (active)
                {
                    activeSec += dt;
                    // how much the mids have moved since just before the jump
                    // (from near-silence, any mids at all count as a big move)
                    const float midMove = refM > 1.0e-10f ? std::abs (sM2 / refM - 1.0f) : (sM2 > 1.0e-9f ? 10.0f : 0.0f);
                    const float over = hDb - refDb;
                    if (midMove > (activeSec < 0.015f ? 0.6f : 1.0f)) { active = false; holdSec = 0.06f; } // a strum: let go
                    else if (over < 1.5f || activeSec > 0.4f) active = false;  // the burst has died away
                    else target = std::min (maxCut, over * pull);
                }
                if (! on) target = 0.0f;

                glideCut (cutDb, target, dt, 0.001f, 0.03f);
                maxSeen = std::max (maxSeen, cutDb);
                if (on || cutDb > 0.0f)
                    bell.process (ch, numCh, start, len, cutDb);
            }
            meterDb = maxSeen;
        }

        std::atomic<float> meterDb { 0.0f };

    private:
        static constexpr int sub = 16;

        double fs = 48000.0;
        Biquad hiA, hiB, midA, midB;
        float hiAtk = 0, hiRel = 0, smoothC = 0, eh = 0, sM = 0, sM2 = 0;
        std::vector<float> histDb, histM;
        int histLen = 90, histPos = 0;
        bool active = false;
        float holdSec = 0.0f, activeSec = 0.0f, refDb = -120.0f, refM = 0.0f, cutDb = 0.0f;
        DynamicBell bell;
    };

    //==============================================================================
    // Pick: softens the hard click of a pick at the start of each note or
    // strum, leaving the ringing part alone. A fast and a slow follower watch
    // the 2-6 kHz band; only while the fast one is well above the slow one
    // (the first few tens of ms of an attack) does the band come down.
    class PickTamer
    {
    public:
        void prepare (double sampleRate)
        {
            fs = sampleRate;
            bandA.c = BiquadCoefs::highPass (fs, 2000.0, 0.7071);
            bandB.c = BiquadCoefs::lowPass (fs, std::min (6000.0, fs * 0.45), 0.7071);
            bandA.reset(); bandB.reset();
            fastAtk = onePole (0.3f, fs); fastRel = onePole (10.0f, fs);
            slowAtk = onePole (20.0f, fs); slowRel = onePole (150.0f, fs);
            fast = slow = 1.0e-12f;
            cutDb = 0.0f;
            bell.prepare (fs, 3460.0f, 0.87f); // spans roughly 2-6 kHz
        }

        void process (float* const* ch, int numCh, int n, float amount, bool on)
        {
            const float thr = 3.0f - 1.5f * amount;  // how far the attack must stand above the ring (dB)
            const float maxCut = 2.0f + 10.0f * amount;
            const float slope = 0.4f + 0.8f * amount;
            float maxSeen = 0.0f;

            for (int start = 0; start < n; start += sub)
            {
                const int len = std::min (sub, n - start);
                for (int i = start; i < start + len; ++i)
                {
                    float m = 0.0f;
                    for (int c = 0; c < numCh; ++c) m += ch[c][i];
                    m /= (float) numCh;
                    const float b = bandB.process (0, bandA.process (0, m));
                    const float p = b * b;
                    fast += (p > fast ? fastAtk : fastRel) * (p - fast);
                    slow += (p > slow ? slowAtk : slowRel) * (p - slow);
                }
                if (! on && cutDb == 0.0f) continue;

                const float dt = (float) len / (float) fs;
                const float trans = powerToDb (fast) - powerToDb (slow);
                float target = 0.0f;
                if (on && powerToDb (fast) > -75.0f && trans > thr)
                    target = std::min (maxCut, (trans - thr) * slope);

                glideCut (cutDb, target, dt, 0.0003f, 0.025f);
                maxSeen = std::max (maxSeen, cutDb);
                bell.process (ch, numCh, start, len, cutDb);
            }
            meterDb = maxSeen;
        }

        std::atomic<float> meterDb { 0.0f };

    private:
        static constexpr int sub = 16;
        double fs = 48000.0;
        Biquad bandA, bandB;
        float fastAtk = 0, fastRel = 0, slowAtk = 0, slowRel = 0, fast = 0, slow = 0, cutDb = 0;
        DynamicBell bell;
    };

    //==============================================================================
    // Boom: a mic close to the soundhole piles up low end around the body's
    // main resonance. Boom finds that resonance in the long-term spectrum
    // (the same idea as ResonanceFinder: a narrow average that stands out
    // above its surroundings, here 1/6 octave against 1/3-octave bands either
    // side at f/1.6 and f*1.6, searched only 70-320 Hz and allocation-free so
    // it can run on the audio thread). Then, live, it compares that band with
    // the same two neighbours. How far it has stood out over the last ~3 s
    // (beyond 3 dB at 0%, down to -2 dB at 100%) sets how deep the cut can
    // go; the last quarter second scales that, so the cut is full while the
    // band is at or above its usual level and eases off as it quiets down.
    // A single loud low note doesn't move the 3-second figure much, so normal
    // low notes aren't treated as boom. Everything is relative to
    // the guitar's own neighbouring lows, so it doesn't depend on level or on
    // how bright the guitar is, and a signal with no low resonance gets no cut.
    class BoomTamer
    {
    public:
        static constexpr float minHz = 70.0f, maxHz = 320.0f;

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            const int order = sampleRate > 100000.0 ? 16 : sampleRate > 50000.0 ? 15 : 14; // ~3 Hz bins
            size = 1 << order;
            hop = size / 2;
            fft = std::make_unique<juce::dsp::FFT> (order);
            window.resize ((size_t) size);
            for (int i = 0; i < size; ++i)
                window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);
            ring.assign ((size_t) size, 0.0f);
            work.assign ((size_t) size * 2, 0.0f);
            power.assign ((size_t) size / 2 + 1, 0.0);
            prefix.assign ((size_t) size / 2 + 2, 0.0);
            write = filled = sinceFrame = frames = 0;
            found = false;
            foundHz = 0.0f;
            prominenceDb = 0.0f;

            det.reset(); detLo.reset(); detHi.reset();
            envC = onePole (250.0f, fs); // a quarter-second average: boom builds up, single low notes come and go
            envN = envLo = envHi = 1.0e-12f;
            cutDb = 0.0f;
            longFirst = true;
            longRelDb = 0.0f;
            bell.prepare (fs, 120.0f, 2.0f);
            detHz = 0.0f;
        }

        void process (float* const* ch, int numCh, int n, float amount, bool on)
        {
            const float thr = 3.0f - 5.0f * amount;  // how far the resonance must stand out (dB)
            const float maxCut = std::min (15.0f, 18.0f * amount);
            float maxSeen = 0.0f;

            for (int start = 0; start < n; start += sub)
            {
                const int len = std::min (sub, n - start);
                for (int i = start; i < start + len; ++i)
                {
                    float m = 0.0f;
                    for (int c = 0; c < numCh; ++c) m += ch[c][i];
                    m /= (float) numCh;

                    ring[(size_t) write] = m;
                    write = (write + 1) % size;
                    filled = std::min (size, filled + 1);
                    if (++sinceFrame >= hop) { sinceFrame = 0; if (filled >= size) analyse(); }

                    if (found)
                    {
                        const float b = det.process (0, m), lo = detLo.process (0, m), hi = detHi.process (0, m);
                        envN += envC * (b * b - envN);
                        envLo += envC * (lo * lo - envLo);
                        envHi += envC * (hi * hi - envHi);
                    }
                }

                const float dt = (float) len / (float) fs;
                float target = 0.0f;
                if (found && powerToDb (envN) > -80.0f)
                {
                    // neighbours power-averaged, so an empty one (below the low E string) can't fake a peak
                    relDb = powerToDb (envN) - powerToDb (useLo ? 0.5f * (envLo + envHi) : envHi);
                    // How much it stands out over the last few seconds sets the
                    // depth (this guitar, this mic, booms here); how it's doing
                    // right now scales it: full while the band is at or above
                    // its usual level, easing off as it drops 4 dB below.
                    longRelDb = longFirst ? relDb : longRelDb + longC * dt * (relDb - longRelDb);
                    longFirst = false;
                    const float depth = std::min (maxCut, 3.0f * (longRelDb - thr));
                    const float activity = juce::jlimit (0.0f, 1.0f, 1.0f + (relDb - longRelDb) / 4.0f);
                    if (on && depth > 0.0f) target = depth * activity;
                }
                glideCut (cutDb, target, dt, 0.015f, 0.2f);
                maxSeen = std::max (maxSeen, cutDb);
                if (on || cutDb > 0.0f)
                    bell.process (ch, numCh, start, len, cutDb);
            }
            meterDb = maxSeen;
            foundMeterHz = found ? foundHz : 0.0f;
        }

        bool hasFound() const { return found; }
        float getFoundHz() const { return foundHz; }
        float getProminenceDb() const { return prominenceDb; }
        float getRelDb() const { return relDb; }         // found band vs its neighbours, last ~250 ms
        float getLongRelDb() const { return longRelDb; } // the same over the last few seconds

        std::atomic<float> meterDb { 0.0f }, foundMeterHz { 0.0f };

    private:
        static constexpr int sub = 32;

        void analyse()
        {
            for (int i = 0; i < size; ++i)
                work[(size_t) i] = ring[(size_t) ((write + i) % size)] * window[(size_t) i];
            std::fill (work.begin() + size, work.end(), 0.0f);
            fft->performFrequencyOnlyForwardTransform (work.data(), true);

            // long-term average: a plain mean for the first frames, then ~3 s memory
            ++frames;
            const double k = frames < 18 ? 1.0 / frames : 0.06;
            for (int b = 0; b <= size / 2; ++b)
                power[(size_t) b] += k * ((double) work[(size_t) b] * work[(size_t) b] - power[(size_t) b]);
            if (frames < 4) return; // need ~0.7 s before trusting it

            for (int b = 0; b <= size / 2; ++b) prefix[(size_t) b + 1] = prefix[(size_t) b] + power[(size_t) b];
            const double binHz = fs / size;
            const auto avgDb = [&] (double f, double octaves)
            {
                const int b1 = juce::jlimit (1, size / 2, (int) std::lround (f * std::pow (2.0, -octaves * 0.5) / binHz));
                const int b2 = juce::jlimit (b1, size / 2, (int) std::lround (f * std::pow (2.0, octaves * 0.5) / binHz));
                return 10.0 * std::log10 ((prefix[(size_t) b2 + 1] - prefix[(size_t) b1]) / (b2 - b1 + 1) + 1e-30);
            };

            double maxLevel = -400.0;
            for (double f = minHz; f <= maxHz; f *= std::pow (2.0, 1.0 / 48.0))
                maxLevel = std::max (maxLevel, avgDb (f, 1.0 / 6.0));

            double bestProm = -100.0, bestHz = 0.0;
            for (double f = minHz; f <= maxHz; f *= std::pow (2.0, 1.0 / 48.0))
            {
                const double narrow = avgDb (f, 1.0 / 6.0);
                if (narrow < maxLevel - 20.0) continue; // a bump nobody hears isn't boom
                const double prom = narrow - (f / 1.6 >= lowestNeighbourHz ? 0.5 * (avgDb (f / 1.6, 1.0 / 3.0) + avgDb (f * 1.6, 1.0 / 3.0))
                                                                            : avgDb (f * 1.6, 1.0 / 3.0));
                if (prom > bestProm) { bestProm = prom; bestHz = f; }
            }
            prominenceDb = (float) bestProm;

            // hysteresis so it doesn't flicker on and off
            const bool nowFound = found ? bestProm > 1.5 : bestProm > 2.5;
            if (nowFound)
            {
                // glide to a new resonance instead of jumping
                foundHz = found ? (float) std::exp (std::log (foundHz) + 0.5 * (std::log (bestHz) - std::log (foundHz))) : (float) bestHz;
                if (std::abs (std::log2 (foundHz / std::max (1.0f, detHz))) > 0.02f)
                {
                    detHz = foundHz;
                    det.c = BiquadCoefs::bandPass (fs, detHz, 5.0);
                    detLo.c = BiquadCoefs::bandPass (fs, detHz / 1.6, 3.0);
                    useLo = detHz / 1.6f >= lowestNeighbourHz;
                    detHi.c = BiquadCoefs::bandPass (fs, detHz * 1.6, 3.0);
                    bell.setFreq (detHz);
                }
            }
            found = nowFound;
        }

        double fs = 48000.0;
        int size = 16384, hop = 8192, write = 0, filled = 0, sinceFrame = 0, frames = 0;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, ring, work;
        std::vector<double> power, prefix;
        bool found = false;
        float foundHz = 0.0f, detHz = 0.0f, prominenceDb = 0.0f, relDb = -100.0f;

        // A guitar has nothing below its low E (82 Hz), so a neighbour down
        // there would make every low note look like a bump: below 75 Hz only
        // the upper neighbour is used.
        static constexpr float lowestNeighbourHz = 75.0f;
        bool useLo = true, longFirst = true;
        float longRelDb = 0.0f;
        static constexpr float longC = 1.0f / 3.0f; // ~3 s memory
        Biquad det, detLo, detHi;
        float envC = 0.0f, envN = 0.0f, envLo = 0.0f, envHi = 0.0f, cutDb = 0.0f;
        DynamicBell bell;
    };
}
