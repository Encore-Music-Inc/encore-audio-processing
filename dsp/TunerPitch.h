#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "Music.h"
#include "Ramp.h"
#include "Util.h"
#include "Yin.h"

namespace vs
{
    // The tuner: its pitch, vs::Yin with Yin::tuner() on one block of
    // samples, from a bass dropped to low A to a piccolo's top, and
    // TunerReading below, what that pitch means on the instrument. Plain C++
    // with no JUCE, so the desktop tuner and the browser (master.wasm) run the
    // same code.
    struct TunerPitch
    {
        // The pitch in Hz of x[0..n), or 0 when there's no clear note.
        static float estimate (const float* x, int n, double sampleRate, float minHz = 25.0f, float maxHz = 2100.0f)
        {
            Yin yin (Yin::tuner (minHz, maxHz));
            const auto& r = yin.analyse (x, n, sampleRate);
            return r.voiced ? (float) r.hz : 0.0f;
        }

        // What a live tuner listens to at a device rate: the newest
        // window x stride samples, averaged down by stride (boxDownsample) to
        // about 24 kHz, which covers 25 Hz to 2.1 kHz and stays cheap.
        struct Window { int stride, n; };
        static Window window (double sampleRate)
        {
            return { std::max (1, (int) std::round (sampleRate / 24000.0)), 4096 };
        }

        // The pitch of the newest window of a live input (oldest first, at
        // least window (sampleRate) stride x n samples): the tuner's whole
        // reading of a stream.
        static float estimateLatest (const float* x, int count, double sampleRate)
        {
            const auto w = window (sampleRate);
            const int need = w.n * w.stride;
            if (count < need) return 0.0f;
            const auto down = boxDownsample (x + (count - need), need, w.stride);
            return estimate (down.data(), w.n, sampleRate / w.stride);
        }
    };

    // What a heard pitch means on the instrument: the string it's nearest
    // (within 3.5 semitones), or else the nearest note, and how many cents off,
    // in the chosen temperament and reference pitch.
    struct TunerReading
    {
        int note = -1;          // the target, as a MIDI note (concert pitch)
        int string = -1;        // which string of the tuning, or -1 when it's chromatic
        float cents = 0;        // + sharp, - flat, from the target (in the chosen temperament)
        float targetHz = 0;

        // hz: what was heard. strings: the tuning's notes (empty: chromatic). offsets: the temperament on C, rotated to root
        static TunerReading of (float hz, double a4, const std::vector<int>& strings, const std::array<float, 12>& offsets, int root)
        {
            TunerReading r;
            if (! (hz > 0)) return r;
            const double m = music::hzToMidi ((double) hz, a4);
            auto target = [&] (int n) { return targetMidi (n, offsets, root); };
            if (! strings.empty())
            {
                double best = 1.0e9;
                for (int i = 0; i < (int) strings.size(); ++i)
                {
                    const double c = (m - target (strings[(size_t) i])) * 100.0;
                    if (std::abs (c) < std::abs (best)) { best = c; r.string = i; }
                }
                if (std::abs (best) <= 350.0) { r.note = strings[(size_t) r.string]; r.cents = (float) best; }
                else r.string = -1;
            }
            if (r.string < 0)
            {
                int n = (int) std::round (m);
                for (int k : { n - 1, n + 1 }) if (std::abs (m - target (k)) < std::abs (m - target (n))) n = k;
                r.note = n;
                r.cents = (float) ((m - target (n)) * 100.0);
            }
            r.targetHz = (float) music::midiToHz (target (r.note), a4);
            return r;
        }

        // Where a note sits in the temperament (the offsets on C, turned to root), as a
        // fractional MIDI note, and its pitch in Hz: what a string should sound, and
        // what its reference tone plays
        static double targetMidi (int note, const std::array<float, 12>& offsets, int root)
        {
            return note + offsets[(size_t) (((note - root) % 12 + 12) % 12)] / 100.0;
        }
        static double targetHzOf (int note, double a4, const std::array<float, 12>& offsets, int root)
        {
            return music::midiToHz (targetMidi (note, offsets, root), a4);
        }
    };

    // A tuner's reference tone: tap a string and hear the note to tune to.
    // Plucked, it rings out and dies away (each overtone faster than the last,
    // over five seconds); steady, it holds until stopped. Its spectrum is a full
    // fundamental with fading overtones, so a low note still comes through
    // small speakers, at exactly the pitch asked for. The volume slider (0 to
    // 100) is squared and halved; the tone fades in (4 ms plucked, 50 ms
    // steady), a new volume glides over 30 ms, and stopping fades it out over
    // 30 ms and ends it after 200 ms.
    class TunerTone
    {
    public:
        enum Kind { pluck = 0, steady = 1 };

        static constexpr std::array<float, 8> harmonics { 1.0f, 0.62f, 0.42f, 0.3f, 0.2f, 0.14f, 0.09f, 0.06f };
        static constexpr double pluckSeconds = 5.0, glideSeconds = 0.03, stopSeconds = 0.2;

        static float levelOf (double volume) { const double v = std::clamp (volume, 0.0, 100.0) / 100; return (float) (v * v * 0.5); }

        void prepare (double sampleRate) { sr = sampleRate; playing = false; }

        void start (double hz, Kind k, double volume)
        {
            kind = k;
            freq = hz;
            pos = 0;
            phase = 0;
            stopping = false;
            stopLeft = 0;
            target = levelOf (volume);
            gain = target;
            glide = onePole ((float) (glideSeconds * 1000), sr);
            attack.reset (sr, kind == pluck ? 0.004 : 0.05);
            attack.setCurrentAndTarget (0.0f);
            attack.setTarget (1.0f);
            if (kind == pluck) renderPluck();
            else steadyScale = steadyPeak();
            playing = true;
        }

        void setVolume (double volume) { target = levelOf (volume); }

        void stop()
        {
            if (! playing || stopping) return;
            stopping = true;
            target = 0;
            stopLeft = (int) std::round (stopSeconds * sr);
        }

        bool sounding() const { return playing; }

        // Writes n samples of the tone (silence once it's over); returns whether it's still sounding
        bool process (float* out, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                if (! playing) { out[i] = 0.0f; continue; }
                gain += glide * (target - gain);
                float v;
                if (kind == pluck)
                {
                    if (pos >= (int) buf.size()) { playing = false; out[i] = 0.0f; continue; }
                    v = buf[(size_t) pos++];
                }
                else
                {
                    v = (float) (steadyAt (phase) * steadyScale);
                    phase += freq / sr;
                    phase -= std::floor (phase);
                }
                out[i] = v * gain * attack.next();
                if (stopping && --stopLeft <= 0) playing = false;
            }
            return playing;
        }

    private:
        double sr = 48000, freq = 440, phase = 0, steadyScale = 1;
        Kind kind = pluck;
        std::vector<float> buf;
        int pos = 0, stopLeft = 0;
        bool playing = false, stopping = false;
        float gain = 0, target = 0, glide = 1;
        Ramp<false> attack;

        // the pluck: each overtone a sine dying away faster than the last (those
        // near Nyquist left out), normalised to full scale, with a 4 ms rise and a
        // 250 ms fall at the end
        void renderPluck()
        {
            const int n = (int) std::floor (sr * pluckSeconds);
            buf.assign ((size_t) n, 0.0f);
            for (size_t k = 0; k < harmonics.size(); ++k)
            {
                const double w = 2 * 3.141592653589793 * freq * (double) (k + 1) / sr;
                if (w >= 3.141592653589793 * 0.9) continue;
                const double decay = std::exp (-(1.1 + (double) k * 0.9) / sr);
                double amp = harmonics[k];
                for (int t = 0; t < n; ++t) { buf[(size_t) t] += (float) (amp * std::sin (w * t)); amp *= decay; }
            }
            float peak = 0;
            for (float v : buf) peak = std::max (peak, std::abs (v));
            const int rise = (int) std::floor (sr * 0.004), fall = (int) std::floor (sr * 0.25);
            for (int t = 0; t < n; ++t)
            {
                float g = 1.0f / (peak > 0 ? peak : 1.0f);
                if (t < rise) g *= (float) t / (float) rise;
                if (t > n - fall) g *= (float) (n - t) / (float) fall;
                buf[(size_t) t] *= g;
            }
        }

        // the steady tone's wave at phase 0..1: the overtones, each 0.8 of the last's share
        static double steadyAt (double ph)
        {
            double v = 0, share = 1;
            for (size_t k = 0; k < harmonics.size(); ++k, share *= 0.8) v += harmonics[k] * share * std::sin (2 * 3.141592653589793 * (double) (k + 1) * ph);
            return v;
        }

        // scaled to full scale over one period
        static double steadyPeak()
        {
            double p = 0;
            for (int i = 0; i < 4096; ++i) p = std::max (p, std::abs (steadyAt (i / 4096.0)));
            return p > 0 ? 1.0 / p : 1.0;
        }
    };

    // Following a tuner's pitch readings, one per estimate: a reading close
    // to the last (within closeRatio) is smoothed into it, a new note jumps;
    // after more than clearAfter readings with no pitch the note clears.
    // steady() counts how many readings in a row a string has held within a
    // tolerance (the product decides the tolerance and when that is "in tune").
    class TunerFollow
    {
    public:
        struct Settings { float closeRatio = 0.03f, keep = 0.6f, take = 0.4f; int clearAfter = 8; };

        TunerFollow() = default;
        explicit TunerFollow (Settings s) : set (s) {}

        // An estimate (0 for no pitch); returns the pitch to show, 0 for none
        float push (float hz)
        {
            if (hz > 0.0f)
            {
                quiet = 0;
                smoothed = (smoothed > 0.0f && std::abs (hz / smoothed - 1.0f) < set.closeRatio) ? smoothed * set.keep + hz * set.take : hz;
                return smoothed;
            }
            if (++quiet > set.clearAfter) { smoothed = 0.0f; steadyCount = 0; lastString = -1; }
            return smoothed;
        }

        // After a reading: how many readings in a row the same string has
        // stayed within toleranceCents (0 when it hasn't)
        int steady (const TunerReading& r, float toleranceCents)
        {
            if (r.string >= 0 && std::abs (r.cents) <= toleranceCents && r.string == lastString) ++steadyCount;
            else steadyCount = 0;
            lastString = r.string;
            return steadyCount;
        }

        bool hasNote() const { return smoothed > 0.0f; }
        void reset() { smoothed = 0.0f; quiet = 0; steadyCount = 0; lastString = -1; }

    private:
        Settings set;
        float smoothed = 0.0f;
        int quiet = 0, steadyCount = 0, lastString = -1;
    };

    // TunerFollow's settings for a note readout (a voice's note on a plugin's
    // screen, one reading per frame): no smoothing, each reading shown as it
    // is, and the last note held through up to nine readings without one,
    // cleared on the tenth.
    inline constexpr TunerFollow::Settings noteReadout { 0.0f, 0.6f, 0.4f, 9 };
}
