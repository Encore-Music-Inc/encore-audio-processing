#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>
#include "Biquad.h"
#include "Util.h"

// Strum's Body stage: make a pickup (DI) sound like a mic on the same guitar.
//
//   BodyCurve   a tone curve, dB at 1/12-octave points from 20 Hz to 20.5 kHz
//   BodyLearner measures that curve from a DI (main input) and a mic of the
//               same performance (side-chain): the long-term mic/DI spectrum ratio
//   BodyFilter  turns a curve into a filter and runs it on the audio
//
// How the curve becomes a filter: a minimum-phase FIR, made with the cepstrum
// method, run through JUCE's zero-latency convolution. Why that and not a
// fitted EQ:
//   - a learned curve can have any shape; an FIR follows it point for point,
//     where an EQ fit always leaves fitting error and needs a band count chosen
//   - minimum phase means no pre-ringing on pick attacks and no delay, so the
//     plugin reports zero latency (it behaves like an analog EQ would). A
//     linear-phase FIR would add ~40 ms of latency and smear attacks forward.
//   - the price is a few FFTs on the message thread whenever Amount or the
//     curve changes, and a modest convolution cost on the audio thread
namespace vs
{
    struct BodyCurve
    {
        static constexpr int numPoints = 121; // 20 Hz * 2^(i/12), up to 20480 Hz
        static constexpr float maxDb = 12.0f; // learned corrections are clamped to +-12 dB

        static float freq (int i) { return 20.0f * std::pow (2.0f, (float) i / 12.0f); }

        std::array<float, numPoints> db {};
        float levelDb = 0.0f;  // how much louder the mic was than the DI (taken out of db, so Body doesn't change the volume)
        float seconds = 0.0f;  // how much playing it was learned from
        bool valid = false;

        // dB at any frequency: straight lines between points on a log axis, flat past the ends
        float at (float hz) const
        {
            const float pos = 12.0f * std::log2 (std::max (hz, 1.0f) / 20.0f);
            if (pos <= 0.0f) return db[0];
            if (pos >= (float) (numPoints - 1)) return db[numPoints - 1];
            const int i = (int) pos;
            const float t = pos - (float) i;
            return db[(size_t) i] + t * (db[(size_t) i + 1] - db[(size_t) i]);
        }

        bool operator== (const BodyCurve& o) const { return db == o.db && levelDb == o.levelDb && valid == o.valid; }

        // Saved in the plugin state (and in user presets) as plain text
        juce::String toString() const
        {
            juce::String s ("1 " + juce::String (levelDb, 2) + " " + juce::String (seconds, 1));
            for (float v : db) s << " " << juce::String (v, 2);
            return s;
        }

        static BodyCurve fromString (const juce::String& text)
        {
            BodyCurve c;
            auto parts = juce::StringArray::fromTokens (text, " ", "");
            parts.removeEmptyStrings();
            if (parts.size() != 3 + numPoints || parts[0] != "1") return c;
            c.levelDb = parts[1].getFloatValue();
            c.seconds = parts[2].getFloatValue();
            for (int i = 0; i < numPoints; ++i)
                c.db[(size_t) i] = juce::jlimit (-maxDb, maxDb, parts[3 + i].getFloatValue());
            c.valid = true;
            return c;
        }

        // The no-Learn fallback: a typical piezo/pickup correction, not
        // measured from anything. Pickups tend to sound quacky and hard around
        // 1-3 kHz and thin in the low mids next to a mic, so this adds a little
        // body, takes some of that 1-3 kHz edge off and adds a touch of air.
        static BodyCurve generic()
        {
            const double fs = 192000.0; // shapes evaluated well below Nyquist, so they're the analog curves
            const BiquadCoefs bands[] {
                BiquadCoefs::peak (fs, 220.0, 0.8, 2.5),      // low-mid body
                BiquadCoefs::peak (fs, 1600.0, 0.9, -4.0),    // piezo quack
                BiquadCoefs::peak (fs, 3200.0, 1.5, -1.5),    // hard edge
                BiquadCoefs::highShelf (fs, 10000.0, 0.7, 2.0) // air
            };
            BodyCurve c;
            for (int i = 0; i < numPoints; ++i)
            {
                double d = 0.0;
                for (auto& b : bands) d += b.magnitudeDb (freq (i), fs);
                c.db[(size_t) i] = (float) d;
            }
            c.valid = true;
            return c;
        }
    };

    //==============================================================================
    // Learn: the DI on the main input, the mic of the same performance on the
    // side-chain. While learning, every FFT frame where both are playing adds
    // its power spectrum to a running sum (per signal). At the end the curve is
    // the ratio of those long-term sums, averaged in 1/3-octave bands so it
    // follows the guitar's tone, not individual notes or room reflections.
    // Only magnitudes are compared, so a few ms of mic delay don't matter, and
    // the levels are normalised, so the send level doesn't matter either.
    //
    // Runs on the audio thread (allocation-free after prepare). The UI asks to
    // start / stop through atomics and collects the result with takeResult().
    class BodyLearner
    {
    public:
        static constexpr float targetSeconds = 30.0f; // of both playing, then it finishes by itself
        static constexpr float minSeconds = 5.0f;     // less than this and Stop gives up instead

        enum Status { idle = 0, listening, noMic, tooShort, done };

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            const int order = sampleRate > 100000.0 ? 14 : sampleRate > 50000.0 ? 13 : 12; // ~85 ms frames
            size = 1 << order;
            hop = size / 2;
            fft = std::make_unique<juce::dsp::FFT> (order);
            window.resize ((size_t) size);
            for (int i = 0; i < size; ++i)
                window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) size);
            ringDi.assign ((size_t) size, 0.0f);
            ringMic.assign ((size_t) size, 0.0f);
            work.assign ((size_t) size * 2, 0.0f);
            sumDi.assign ((size_t) size / 2 + 1, 0.0);
            sumMic.assign ((size_t) size / 2 + 1, 0.0);
            learning = false;
            progress = 0.0f;
        }

        // UI thread
        void requestStart() { startRequested = true; }
        void requestStop()  { stopRequested = true; }
        bool isLearning() const { return learning.load(); }
        float getProgress() const { return progress.load(); }
        Status getStatus() const { return (Status) status.load(); }

        // UI thread: true once, when a new curve has been learned
        bool takeResult (BodyCurve& out)
        {
            if (! resultReady.load (std::memory_order_acquire)) return false;
            out = result;
            resultReady.store (false, std::memory_order_release);
            return true;
        }

        // Audio thread. mic == nullptr means no side-chain is connected.
        void process (const float* di, const float* mic, int n)
        {
            if (! resultReady.load() && startRequested.exchange (false))
                begin();

            if (! learning.load()) { stopRequested = false; return; }

            if (mic == nullptr)
            {
                status = noMic;
                if (stopRequested.exchange (false)) learning = false;
                return;
            }
            if (status.load() == noMic) status = listening;

            for (int i = 0; i < n; ++i)
            {
                ringDi[(size_t) write] = di[i];
                ringMic[(size_t) write] = mic[i];
                write = (write + 1) % size;
                filled = std::min (size, filled + 1);
                if (++sinceFrame >= hop)
                {
                    sinceFrame = 0;
                    if (filled >= size) analyseFrame();
                }
            }

            progress = std::min (1.0f, activeSeconds / targetSeconds);
            const bool stop = stopRequested.exchange (false);
            if (activeSeconds >= targetSeconds || (stop && activeSeconds >= minSeconds))
                finish();
            else if (stop)
            {
                learning = false;
                status = tooShort;
            }
        }

        // Exposed for tests: turn the summed spectra into a curve
        BodyCurve computeCurve() const
        {
            BodyCurve c;
            const double binHz = fs / size;
            const int last = size / 2;
            std::array<double, BodyCurve::numPoints> pd {}, pm {};
            std::array<bool, BodyCurve::numPoints> ok {};
            double maxD = 0.0, maxM = 0.0;

            for (int i = 0; i < BodyCurve::numPoints; ++i)
            {
                const double f = BodyCurve::freq (i);
                int b1 = (int) std::lround (f * std::pow (2.0, -1.0 / 6.0) / binHz);
                int b2 = (int) std::lround (f * std::pow (2.0, 1.0 / 6.0) / binHz);
                b1 = juce::jlimit (1, last, b1);
                b2 = juce::jlimit (b1, last, b2);
                for (int b = b1; b <= b2; ++b) { pd[(size_t) i] += sumDi[(size_t) b]; pm[(size_t) i] += sumMic[(size_t) b]; }
                pd[(size_t) i] /= (b2 - b1 + 1);
                pm[(size_t) i] /= (b2 - b1 + 1);
                if (f >= 60.0 && f <= 12000.0) { maxD = std::max (maxD, pd[(size_t) i]); maxM = std::max (maxM, pm[(size_t) i]); }
            }

            // Trust a point only where both signals actually had something
            // (within 50 dB of their loudest band) and inside 40 Hz .. 16 kHz.
            const double hiLimit = std::min (16000.0, fs * 0.45);
            int firstOk = -1, lastOk = -1;
            for (int i = 0; i < BodyCurve::numPoints; ++i)
            {
                const double f = BodyCurve::freq (i);
                ok[(size_t) i] = f >= 40.0 && f <= hiLimit && pd[(size_t) i] > maxD * 1e-5 && pm[(size_t) i] > maxM * 1e-5 && maxD > 0.0;
                if (ok[(size_t) i]) { if (firstOk < 0) firstOk = i; lastOk = i; }
            }
            if (firstOk < 0) return c;

            std::array<float, BodyCurve::numPoints> ratio {};
            for (int i = 0; i < BodyCurve::numPoints; ++i)
                if (ok[(size_t) i]) ratio[(size_t) i] = (float) (10.0 * std::log10 (pm[(size_t) i] / pd[(size_t) i]));
            // untrusted points take the nearest trusted value
            for (int i = 0; i < BodyCurve::numPoints; ++i)
                if (! ok[(size_t) i])
                {
                    int best = -1;
                    for (int d = 1; d < BodyCurve::numPoints && best < 0; ++d)
                    {
                        if (i - d >= 0 && ok[(size_t) (i - d)]) best = i - d;
                        else if (i + d < BodyCurve::numPoints && ok[(size_t) (i + d)]) best = i + d;
                    }
                    ratio[(size_t) i] = ratio[(size_t) best];
                }

            // The mic/DI level difference (average over 200 Hz .. 4 kHz) comes
            // out, so Body changes the tone, not the volume.
            double sum = 0.0; int count = 0;
            for (int i = 0; i < BodyCurve::numPoints; ++i)
                if (BodyCurve::freq (i) >= 200.0f && BodyCurve::freq (i) <= 4000.0f) { sum += ratio[(size_t) i]; ++count; }
            c.levelDb = (float) (sum / std::max (1, count));

            for (int i = 0; i < BodyCurve::numPoints; ++i)
            {
                const float f = BodyCurve::freq (i);
                float v = ratio[(size_t) i] - c.levelDb;
                // fade to flat below 40 Hz and above 16 kHz (nothing useful there, only rumble and hiss to boost)
                if (f < 40.0f) v *= juce::jlimit (0.0f, 1.0f, std::log2 (f / 20.0f));
                if (f > 16000.0f) v *= juce::jlimit (0.0f, 1.0f, 1.0f - std::log2 (f / 16000.0f) / std::log2 (20480.0f / 16000.0f));
                c.db[(size_t) i] = juce::jlimit (-BodyCurve::maxDb, BodyCurve::maxDb, v);
            }
            c.seconds = activeSeconds;
            c.valid = true;
            return c;
        }

        float getActiveSeconds() const { return activeSeconds; }

    private:
        void begin()
        {
            std::fill (sumDi.begin(), sumDi.end(), 0.0);
            std::fill (sumMic.begin(), sumMic.end(), 0.0);
            std::fill (ringDi.begin(), ringDi.end(), 0.0f);
            std::fill (ringMic.begin(), ringMic.end(), 0.0f);
            write = filled = sinceFrame = 0;
            activeSeconds = 0.0f;
            peakDi = peakMic = -200.0f;
            progress = 0.0f;
            status = listening;
            stopRequested = false;
            learning = true;
        }

        void finish()
        {
            result = computeCurve();
            learning = false;
            if (result.valid)
            {
                status = done;
                resultReady.store (true, std::memory_order_release);
            }
            else
            {
                status = tooShort;
            }
        }

        // One frame from each ring (oldest sample first), windowed. Counted
        // only if both are playing: above -65 dBFS and within 30 dB of their
        // own loudest frame, so silence, noise floor and ring-outs are skipped.
        void analyseFrame()
        {
            const auto frameDb = [this] (const std::vector<float>& ring)
            {
                double s = 0.0;
                for (float v : ring) s += (double) v * v;
                return 10.0f * (float) std::log10 (s / size + 1e-20);
            };
            const float dDb = frameDb (ringDi), mDb = frameDb (ringMic);
            peakDi = std::max (peakDi, dDb);
            peakMic = std::max (peakMic, mDb);
            if (dDb < -65.0f || mDb < -65.0f || dDb < peakDi - 30.0f || mDb < peakMic - 30.0f) return;

            accumulate (ringDi, sumDi);
            accumulate (ringMic, sumMic);
            activeSeconds += (float) hop / (float) fs;
        }

        void accumulate (const std::vector<float>& ring, std::vector<double>& sum)
        {
            for (int i = 0; i < size; ++i)
                work[(size_t) i] = ring[(size_t) ((write + i) % size)] * window[(size_t) i];
            std::fill (work.begin() + size, work.end(), 0.0f);
            fft->performFrequencyOnlyForwardTransform (work.data(), true);
            for (int b = 0; b <= size / 2; ++b)
                sum[(size_t) b] += (double) work[(size_t) b] * work[(size_t) b];
        }

        double fs = 48000.0;
        int size = 4096, hop = 2048, write = 0, filled = 0, sinceFrame = 0;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, ringDi, ringMic, work;
        std::vector<double> sumDi, sumMic;
        float activeSeconds = 0.0f, peakDi = -200.0f, peakMic = -200.0f;
        BodyCurve result;
        std::atomic<bool> learning { false }, startRequested { false }, stopRequested { false }, resultReady { false };
        std::atomic<float> progress { 0.0f };
        std::atomic<int> status { idle };
    };

    //==============================================================================
    // Runs a curve on the audio. design() makes the minimum-phase FIR; the
    // convolution is JUCE's zero-latency one, which crossfades when a new
    // filter arrives, so Amount moves don't click.
    class BodyFilter
    {
    public:
        // FIR length: ~85 ms, plenty for 1/3-octave detail down to ~50 Hz
        static int lengthFor (double fs) { return fs > 100000.0 ? 16384 : fs > 50000.0 ? 8192 : 4096; }

        // Any thread but the audio thread (allocates). amount scales the curve in dB.
        static juce::AudioBuffer<float> design (const BodyCurve& curve, float amount, double fs)
        {
            using C = juce::dsp::Complex<float>;
            const int len = lengthFor (fs);
            const int n = len * 4; // generous cepstrum size keeps time-aliasing negligible
            const int order = juce::roundToInt (std::log2 ((double) n));
            juce::dsp::FFT fft (order);
            std::vector<C> a ((size_t) n), b ((size_t) n);

            // log magnitude, symmetric
            for (int k = 0; k <= n / 2; ++k)
            {
                const float hz = (float) (k * fs / n);
                const float lm = amount * curve.at (hz) * (float) (std::log (10.0) / 20.0);
                a[(size_t) k] = { lm, 0.0f };
                if (k > 0 && k < n / 2) a[(size_t) (n - k)] = { lm, 0.0f };
            }
            fft.perform (a.data(), b.data(), true); // real cepstrum

            // fold the cepstrum onto positive time: that's the minimum-phase version
            std::fill (a.begin(), a.end(), C {});
            a[0] = { b[0].real(), 0.0f };
            for (int i = 1; i < n / 2; ++i) a[(size_t) i] = { 2.0f * b[(size_t) i].real(), 0.0f };
            a[(size_t) n / 2] = { b[(size_t) n / 2].real(), 0.0f };
            fft.perform (a.data(), b.data(), false);

            for (int k = 0; k < n; ++k)
            {
                const float e = std::exp (b[(size_t) k].real()), ph = b[(size_t) k].imag();
                a[(size_t) k] = { e * std::cos (ph), e * std::sin (ph) };
            }
            fft.perform (a.data(), b.data(), true);

            // keep the start (where a minimum-phase response has its energy),
            // with a gentle fade over the last quarter
            juce::AudioBuffer<float> ir (2, len);
            const int fadeFrom = len * 3 / 4;
            for (int i = 0; i < len; ++i)
            {
                float w = 1.0f;
                if (i >= fadeFrom) w = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (i - fadeFrom) / (float) (len - fadeFrom));
                const float v = b[(size_t) i].real() * w;
                ir.setSample (0, i, v);
                ir.setSample (1, i, v);
            }
            return ir;
        }

        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            takePending();
            conv.prepare (spec);
            conv.reset();
        }

        void reset() { conv.reset(); }

        // Message thread: design and queue a filter for the audio thread
        void setCurve (const BodyCurve& curve, float amount, double fs)
        {
            auto ir = design (curve, amount, fs);
            const juce::SpinLock::ScopedLockType sl (lock);
            pending = std::move (ir);
            pendingRate = fs;
            hasPending = true;
        }

        // Audio thread, stereo in place
        void process (float* const* ch, int numSamples)
        {
            takePending();
            juce::dsp::AudioBlock<float> block (ch, 2, (size_t) numSamples);
            conv.process (juce::dsp::ProcessContextReplacing<float> (block));
        }

    private:
        // Hand the newest filter to the convolution (wait-free: if the
        // message thread holds the lock right now, try again next block)
        void takePending()
        {
            const juce::SpinLock::ScopedTryLockType sl (lock);
            if (! sl.isLocked() || ! hasPending) return;
            conv.loadImpulseResponse (std::move (pending), pendingRate, juce::dsp::Convolution::Stereo::yes,
                                      juce::dsp::Convolution::Trim::no, juce::dsp::Convolution::Normalise::no);
            hasPending = false;
        }

        juce::dsp::Convolution conv; // default = zero latency
        juce::SpinLock lock;
        juce::AudioBuffer<float> pending;
        double pendingRate = 48000.0;
        bool hasPending = false;
    };
}
