// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// The guide a singer sings along with: notes scheduled to the sample (a soft
// piano that strikes bright, mellows and fades, or a soft steady note), sirens
// that glide up and back down, and a drone to sing against, all mixed into one
// channel a block at a time. A voice coach's exercises schedule them; the
// listening and the scoring are VoiceTracker.h's. Plain C++ with no JUCE, so
// the browser (master.wasm) plays the same sound.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>
#include "Biquad.h"
#include "Music.h"
#include "Oscillator.h"
#include "Util.h"

namespace vs
{
    class VoiceGuide
    {
    public:
        // piano: struck, bright then mellow, fading; soft: a steady tone (also every siren and the drone)
        enum class Sound { piano, soft };

        // The two waves, each harmonic's amplitude
        static const HarmonicWave& waveOf (Sound s)
        {
            static const HarmonicWave piano ({ 1, 0.5, 0.32, 0.2, 0.13, 0.08, 0.05, 0.03 });
            static const HarmonicWave soft ({ 1, 0.25, 0.08, 0.03 });
            return s == Sound::piano ? piano : soft;
        }

        // The volume slider (0 to 100), squared, at most 0.6
        static double levelOf (double slider) { const double v = std::clamp (slider, 0.0, 100.0) / 100; return v * v * 0.6; }

        void prepare (double sampleRate) { sr = sampleRate; voices.clear(); drone.reset(); }

        void setVolume (double slider)
        {
            vol = levelOf (slider);
            if (drone && ! drone->ending) drone->target = vol * 0.5;
        }

        // A note (MIDI) starting at frame `at` for `seconds`, its level a share of the volume.
        // A glide (from, to, up and down seconds) makes it a siren: from up to to and back,
        // each way in equal ratios of pitch.
        struct Glide { double from = 0, to = 0, up = 0, down = 0; };

        void add (double midi, std::int64_t at, double seconds, Sound sound, double level = 1.0, const Glide* glide = nullptr)
        {
            Voice v;
            v.sound = glide ? Sound::soft : sound;
            v.at = at;
            v.end = at + (std::int64_t) std::llround (seconds * sr);
            v.stopAt = v.end + (std::int64_t) std::llround (0.3 * sr);
            v.hz = music::midiToHz (glide ? glide->from : midi);
            if (glide)
            {
                v.glide = true;
                v.hzTo = music::midiToHz (glide->to);
                v.upFrames = std::max (1.0, glide->up * sr);
                v.downFrames = std::max (1.0, glide->down * sr);
            }
            v.peak = vol * level * (v.sound == Sound::piano ? 1.0 : 0.75);
            v.attack = (v.sound == Sound::piano ? 0.006 : 0.06) * sr;
            const double cut = std::min (12000.0, v.hz * (v.sound == Sound::piano ? 9.0 : 6.0));
            v.cutoff = cut;
            v.cutoffTarget = v.sound == Sound::piano ? std::min (12000.0, v.hz * 3.5) : cut;
            v.filter.c = BiquadCoefs::lowPass (sr, cut, filterQ);
            voices.push_back (v);
        }

        // The drone on `midi` from frame `at` (a negative midi turns it off)
        void setDrone (double midi, std::int64_t at)
        {
            if (drone) { drone->ending = true; drone->target = 0; drone->stopAt = at + (std::int64_t) std::llround (0.3 * sr); voices.push_back (*drone); drone.reset(); }
            if (midi < 0) return;
            Voice d;
            d.sound = Sound::soft;
            d.isDrone = true;
            d.at = at;
            d.end = d.stopAt = std::numeric_limits<std::int64_t>::max();
            d.hz = music::midiToHz (midi);
            d.target = vol * 0.5;
            d.attack = 0.3 * sr;
            d.cutoff = d.cutoffTarget = sr * 0.49;
            d.unfiltered = true;
            drone = d;
        }

        // Every note fades out from frame `at` (the drone keeps going)
        void stopAll (std::int64_t at)
        {
            for (auto& v : voices)
                if (! v.ending)
                {
                    v.ending = true;
                    v.fadeFrom = at;
                    v.stopAt = std::min (v.stopAt, at + (std::int64_t) std::llround (0.15 * sr));
                }
        }

        // Mixes frames [frame0, frame0 + n) into out (written, not added); returns how many are sounding
        int process (float* out, int n, std::int64_t frame0)
        {
            std::fill (out, out + n, 0.0f);
            for (auto& v : voices) render (v, out, n, frame0);
            if (drone) render (*drone, out, n, frame0);
            const std::int64_t end = frame0 + n;
            voices.erase (std::remove_if (voices.begin(), voices.end(), [end] (const Voice& v) { return v.stopAt <= end; }), voices.end());
            return (int) voices.size() + (drone ? 1 : 0);
        }

    private:
        // the browser's lowpass default (Q 1, in dB), as a plain Q
        static constexpr double filterQ = 1.1220184543019633;

        struct Voice
        {
            Sound sound = Sound::soft;
            std::int64_t at = 0, end = 0, stopAt = 0, fadeFrom = -1;
            double hz = 440, hzTo = 440, upFrames = 1, downFrames = 1, phase = 0;
            double peak = 0, attack = 1, gain = 0, target = 0, cutoff = 1000, cutoffTarget = 1000;
            bool glide = false, ending = false, isDrone = false, unfiltered = false;
            Biquad filter;
        };

        double sr = 48000, vol = levelOf (60);
        std::vector<Voice> voices;
        std::optional<Voice> drone;

        void render (Voice& v, float* out, int n, std::int64_t frame0)
        {
            const double tcRelease = std::exp (-1.0 / (0.04 * sr)), tcStop = std::exp (-1.0 / (0.02 * sr));
            const double tcMellow = std::exp (-1.0 / (0.4 * sr)), tcCut = std::exp (-1.0 / (0.35 * sr)), tcDrone = std::exp (-1.0 / (0.05 * sr));
            for (int i = 0; i < n; ++i)
            {
                const std::int64_t f = frame0 + i;
                if (f < v.at || f >= v.stopAt) continue;
                const double t = (double) (f - v.at);
                // the level
                if (v.isDrone) v.gain = t < v.attack && ! v.ending ? v.target * t / v.attack : v.target + (v.gain - v.target) * tcDrone;
                else if (v.fadeFrom >= 0 && f >= v.fadeFrom) v.gain *= tcStop;
                else if (f >= v.end) v.gain *= tcRelease;
                else if (t < v.attack) v.gain = v.peak * t / v.attack;
                else if (v.sound == Sound::piano) v.gain = 0.3 * v.peak + (v.gain - 0.3 * v.peak) * tcMellow;
                else v.gain = v.peak;
                // the pitch: a siren goes up, then back down, each way in equal ratios
                double hz = v.hz;
                if (v.glide)
                {
                    if (t < v.upFrames) hz = v.hz * std::pow (v.hzTo / v.hz, t / v.upFrames);
                    else if (t < v.upFrames + v.downFrames) hz = v.hzTo * std::pow (v.hz / v.hzTo, (t - v.upFrames) / v.downFrames);
                }
                // the piano's brightness mellows
                if (v.sound == Sound::piano && ! v.unfiltered)
                {
                    v.cutoff = v.cutoffTarget + (v.cutoff - v.cutoffTarget) * tcCut;
                    if (((f - v.at) & 15) == 0) v.filter.c = BiquadCoefs::lowPass (sr, v.cutoff, filterQ);
                }
                double x = waveOf (v.sound).at (v.phase, hz, sr);
                v.phase += hz / sr;
                v.phase -= std::floor (v.phase);
                if (! v.unfiltered) x = v.filter.process (0, (float) x);
                out[i] += (float) (x * v.gain);
            }
        }
    };
}
