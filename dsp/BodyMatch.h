#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>
#include "JuceInclude.h"
#include "Biquad.h"
#include "Channels.h"
#include "Convolution.h"
#include "Fft.h"
#include "Spectrum.h"
#include "Util.h"
#include "Windows.h"

// Strum's Body stage: make a pickup (DI) sound like a mic on the same guitar.
//
//   BodyCurve   a tone curve, dB at 1/12-octave points from 20 Hz to 20.5 kHz
//   BodyLearner measures that curve from a DI (main input) and a mic of the
//               same performance (side-chain): the long-term mic/DI spectrum ratio
//   BodyFilter  turns a curve into a filter and runs it on the audio
//
// How the curve becomes a filter: a minimum-phase FIR, made with the cepstrum
// method, run through the zero-latency convolution (Convolution.h). Why that and not a
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
            return lerp (db[(size_t) i], db[(size_t) i + 1], t);
        }

        // The dB Body applies at a frequency at an amount (0..1 and beyond):
        // the curve scaled, what the filter is built from and what an editor draws
        float appliedDb (float hz, float amount) const { return amount * at (hz); }

        bool operator== (const BodyCurve& o) const { return db == o.db && levelDb == o.levelDb && valid == o.valid; }

        // Saved in the plugin state (and in user presets) as plain text:
        // "1", the level, the seconds and the points, separated by spaces,
        // with a point for decimals whatever the system's language.
        std::string toString() const
        {
            std::string s = "1 " + fixed (levelDb, 2) + " " + fixed (seconds, 1);
            for (float v : db) s += " " + fixed (v, 2);
            return s;
        }

        // Anything but that text (a different count, or not starting with "1")
        // gives a curve that isn't valid. A word is read up to where it stops
        // being a number, and reads as 0 if it doesn't start with one.
        static BodyCurve fromString (const std::string& text)
        {
            BodyCurve c;
            std::vector<std::string> parts;
            for (size_t from = 0; from <= text.size();)
            {
                const size_t to = std::min (text.find (' ', from), text.size());
                if (to > from) parts.push_back (text.substr (from, to - from));
                from = to + 1;
            }
            if (parts.size() != (size_t) (3 + numPoints) || parts[0] != "1") return c;
            c.levelDb = number (parts[1]);
            c.seconds = number (parts[2]);
            for (int i = 0; i < numPoints; ++i)
                c.db[(size_t) i] = std::clamp (number (parts[(size_t) (3 + i)]), -maxDb, maxDb);
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

    private:
        static std::string fixed (double v, int places)
        {
            std::ostringstream o;
            o.imbue (std::locale::classic());
            o.setf (std::ios_base::fixed);
            o.precision (places);
            o << v;
            return o.str();
        }

        // The number at the start of a word: an optional sign, digits with
        // an optional point, an optional exponent, or "nan" or "inf"; 0 when
        // there is none
        static float number (const std::string& word)
        {
            size_t i = word.find_first_not_of (" \t\n\r\f\v");
            if (i == std::string::npos) return 0.0f;
            const size_t start = i;
            if (word[i] == '+' || word[i] == '-') ++i;
            const auto is = [&] (size_t at, const char* name)
            {
                for (size_t k = 0; name[k] != 0; ++k)
                    if (at + k >= word.size() || std::tolower ((unsigned char) word[at + k]) != name[k]) return false;
                return true;
            };
            if (is (i, "nan")) return std::numeric_limits<float>::quiet_NaN();
            if (is (i, "inf")) return word[start] == '-' ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
            const auto digit = [&] (size_t at) { return at < word.size() && word[at] >= '0' && word[at] <= '9'; };
            // significant: a digit after the point, or one from the first
            // non-zero digit on (so "-0" reads as 0, but "-0.00" as -0)
            bool any = false, significant = false;
            while (digit (i)) { significant = significant || word[i] != '0'; ++i; any = true; }
            if (i < word.size() && word[i] == '.') { ++i; while (digit (i)) { ++i; any = significant = true; } }
            if (! any || ! significant) return 0.0f;
            if (i < word.size() && (word[i] == 'e' || word[i] == 'E'))
            {
                size_t e = i + 1;
                if (e < word.size() && (word[e] == '+' || word[e] == '-')) ++e;
                if (digit (e)) { while (digit (e)) ++e; i = e; }
            }
            std::istringstream in (word.substr (start, i - start));
            in.imbue (std::locale::classic());
            double v = 0.0;
            in >> v;
            if (v == 0.0 && word[start] == '-') return -0.0f; // too small to hold, below zero
            if (std::abs (v) == std::numeric_limits<double>::max()) // too large to hold
                return v > 0 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
            return (float) v;
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
            ringDi.assign ((size_t) size, 0.0f);
            ringMic.assign ((size_t) size, 0.0f);
            ltsDi.prepare (size, true);
            ltsMic.prepare (size, true);
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
                b1 = std::clamp (b1, 1, last);
                b2 = std::clamp (b2, b1, last);
                for (int b = b1; b <= b2; ++b) { pd[(size_t) i] += ltsDi.sum()[(size_t) b]; pm[(size_t) i] += ltsMic.sum()[(size_t) b]; }
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
                if (ok[(size_t) i]) ratio[(size_t) i] = (float) db10 (pm[(size_t) i] / pd[(size_t) i], 0.0);
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
                if (f < 40.0f) v *= std::clamp (std::log2 (f / 20.0f), 0.0f, 1.0f);
                if (f > 16000.0f) v *= std::clamp (1.0f - std::log2 (f / 16000.0f) / std::log2 (20480.0f / 16000.0f), 0.0f, 1.0f);
                c.db[(size_t) i] = std::clamp (v, -BodyCurve::maxDb, BodyCurve::maxDb);
            }
            c.seconds = activeSeconds;
            c.valid = true;
            return c;
        }

        float getActiveSeconds() const { return activeSeconds; }

    private:
        void begin()
        {
            ltsDi.clear();
            ltsMic.clear();
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
                return (float) db10 (s / size);
            };
            const float dDb = frameDb (ringDi), mDb = frameDb (ringMic);
            peakDi = std::max (peakDi, dDb);
            peakMic = std::max (peakMic, mDb);
            if (dDb < -65.0f || mDb < -65.0f || dDb < peakDi - 30.0f || mDb < peakMic - 30.0f) return;

            ltsDi.addRing (ringDi.data(), write);
            ltsMic.addRing (ringMic.data(), write);
            activeSeconds += (float) hop / (float) fs;
        }

        double fs = 48000.0;
        int size = 4096, hop = 2048, write = 0, filled = 0, sinceFrame = 0;
        std::vector<float> ringDi, ringMic;
        LongTermSpectrum ltsDi, ltsMic; // each a sum of frame powers
        float activeSeconds = 0.0f, peakDi = -200.0f, peakMic = -200.0f;
        BodyCurve result;
        std::atomic<bool> learning { false }, startRequested { false }, stopRequested { false }, resultReady { false };
        std::atomic<float> progress { 0.0f };
        std::atomic<int> status { idle };
    };

    //==============================================================================
    // Runs a curve on the audio. design() makes the minimum-phase FIR; the
    // convolution (Convolution.h) has no latency and crossfades when a new
    // filter arrives, so Amount moves don't click.
    //
    // Threads: setCurve() on one thread other than the audio thread (it
    // designs the filter and makes it ready there), prepare() off the audio
    // thread, process() and reset() on the audio thread.
    class BodyFilter
    {
    public:
        // FIR length: ~85 ms, plenty for 1/3-octave detail down to ~50 Hz
        static int lengthFor (double fs) { return fs > 100000.0 ? 16384 : fs > 50000.0 ? 8192 : 4096; }

        // Any thread but the audio thread (allocates). amount scales the curve in dB.
        static std::vector<float> design (const BodyCurve& curve, float amount, double fs)
        {
            const int len = lengthFor (fs);
            const int n = len * 4; // generous cepstrum size keeps time-aliasing negligible
            const Fft fft (n);
            std::vector<double> re ((size_t) n, 0.0), im ((size_t) n, 0.0);

            // log magnitude, symmetric
            for (int k = 0; k <= n / 2; ++k)
            {
                const float hz = (float) (k * fs / n);
                const float lm = curve.appliedDb (hz, amount) * (float) (std::log (10.0) / 20.0);
                re[(size_t) k] = lm;
                if (k > 0 && k < n / 2) re[(size_t) (n - k)] = lm;
            }
            fft.inverse (re.data(), im.data()); // real cepstrum

            // fold the cepstrum onto positive time: that's the minimum-phase version
            for (int i = 1; i < n / 2; ++i) re[(size_t) i] *= 2.0;
            std::fill (re.begin() + n / 2 + 1, re.end(), 0.0);
            std::fill (im.begin(), im.end(), 0.0);
            fft.forward (re.data(), im.data());

            for (int k = 0; k < n; ++k)
            {
                const double e = std::exp (re[(size_t) k]), ph = im[(size_t) k];
                re[(size_t) k] = e * std::cos (ph);
                im[(size_t) k] = e * std::sin (ph);
            }
            fft.inverse (re.data(), im.data());

            // keep the start (where a minimum-phase response has its energy),
            // with a gentle fade over the last quarter
            std::vector<float> ir ((size_t) len);
            const int fadeFrom = len * 3 / 4;
            for (int i = 0; i < len; ++i)
            {
                const double t = i >= fadeFrom ? (double) (i - fadeFrom) / (double) (len - fadeFrom) : 0.0;
                const float w = i >= fadeFrom ? (float) channels::fadeOutGain (channels::FadeCurve::sineSquared, t) : 1.0f;
                ir[(size_t) i] = (float) re[(size_t) i] * w;
            }
            return ir;
        }

        // Takes up the newest filter (from setCurve, designed for this sample rate)
        void prepare (const juce::dsp::ProcessSpec& spec)
        {
            conv.prepare (spec.sampleRate, (int) spec.maximumBlockSize);
        }

        void reset() { conv.reset(); }

        // Design a filter for sample rate fs and hand it to the audio thread
        void setCurve (const BodyCurve& curve, float amount, double fs)
        {
            conv.loadImpulseResponse ({ design (curve, amount, fs) });
        }

        // Audio thread, stereo in place
        void process (float* const* ch, int numSamples)
        {
            conv.process (ch, 2, numSamples);
        }

    private:
        Convolution conv;
    };
}
