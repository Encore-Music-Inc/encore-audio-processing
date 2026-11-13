#pragma once

// A subtractive synthesizer, the controls of Tracktion Engine's 4OSC (GPLv3)
// rewritten on this repository's building blocks: up to four oscillators per
// voice (sine, triangle, saw up or down, pulse, noise; each with its tune,
// level, pulse width and up to eight detuned copies spread across the stereo
// field), a state-variable filter (low, band or high pass, 12 or 24 dB per
// octave) with its own envelope, key tracking and velocity, an amp envelope,
// two LFOs (to pitch, filter, level or pulse width), glide, and poly, mono or
// legato play. Its sound is its own; effects go after it in the chain.
//
//     Synth s; s.prepare (fs); s.setSettings (settings);
//     s.noteOn (60, 0.8f); ... s.render (l, r, n) (adds into l and r); s.noteOff (60);

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Envelope.h"
#include "Music.h"
#include "Oscillator.h"
#include "Svf.h"
#include "Util.h"

namespace vs
{
    class Synth
    {
    public:
        static constexpr int numOscillators = 4, maxUnison = 8, maxVoices = 16, numLfos = 2;

        struct Osc
        {
            bool on = false;
            Oscillator::Wave wave = Oscillator::Wave::sawUp;
            int semitones = 0;          // -36 .. 36
            float cents = 0.0f;         // -100 .. 100
            float levelDb = -6.0f;      // -60 .. 0
            float pulseWidth = 0.5f;    // pulse only
            int unison = 1;             // 1 .. maxUnison copies
            float detuneCents = 10.0f;  // spread of the copies' tuning
            float spread = 0.5f;        // 0 centre .. 1 copies across left and right
            float pan = 0.0f;           // -1 left .. 1 right
        };

        enum class FilterType { off, lowPass, bandPass, highPass };
        struct Filter
        {
            FilterType type = FilterType::lowPass;
            bool steep = false;         // 24 dB per octave instead of 12
            float hz = 8000.0f;         // 20 .. 20000
            float resonance = 0.7f;     // Q, 0.5 .. 10
            float keyTrack = 0.0f;      // 0 .. 1: the cutoff follows the note (1: one octave per octave)
            float envSemitones = 0.0f;  // -60 .. 60: how far the filter envelope moves the cutoff
            float velocity = 0.0f;      // 0 .. 1: how much harder notes open the cutoff
        };

        enum class LfoTarget { none, pitch, filter, level, pulseWidth };
        struct Lfo
        {
            Oscillator::Wave wave = Oscillator::Wave::sine;
            float hz = 5.0f;
            float depth = 0.0f;         // pitch: semitones; filter: semitones; level: 0..1; pulse width: 0..0.45
            LfoTarget target = LfoTarget::none;
        };

        enum class VoiceMode { poly, mono, legato };

        struct Settings
        {
            std::array<Osc, numOscillators> osc { { Osc { true }, Osc {}, Osc {}, Osc {} } };
            Filter filter;
            Envelope::Settings ampEnv { 0.005f, 0.3f, 0.8f, 0.3f };
            Envelope::Settings filterEnv { 0.005f, 0.3f, 0.0f, 0.3f };
            std::array<Lfo, numLfos> lfo {};
            VoiceMode mode = VoiceMode::poly;
            int voices = 8;             // poly: 1 .. maxVoices
            float glideSeconds = 0.0f;  // mono and legato
            float velocityToLevel = 1.0f; // 0: every note as loud .. 1: velocity sets the level
            float levelDb = -6.0f;
        };

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            for (auto& v : voices) v.prepare (fs);
            for (auto& l : lfos) l = Oscillator {};
            setSettings (set);
            allNotesOff (true);
        }

        void setSettings (const Settings& s)
        {
            set = s;
            set.voices = std::clamp (s.voices, 1, maxVoices);
            for (auto& o : set.osc) o.unison = std::clamp (o.unison, 1, maxUnison);
            for (auto& v : voices) v.setEnvelopes (set);
            for (int i = 0; i < numLfos; ++i) lfos[(size_t) i].setWave (set.lfo[(size_t) i].wave);
        }
        const Settings& getSettings() const { return set; }

        // Pitch bend, in semitones (the host maps its wheel range)
        void setPitchBend (float semitones) { bend = semitones; }

        void noteOn (int note, float velocity)
        {
            velocity = std::clamp (velocity, 0.0f, 1.0f);
            if (set.mode != VoiceMode::poly)
            {
                auto& v = voices[0];
                held.erase (std::remove (held.begin(), held.end(), note), held.end());
                held.push_back (note);
                const bool legato = set.mode == VoiceMode::legato && v.amp.isActive() && ! v.amp.isReleasing();
                v.start (note, velocity, ++age, ! legato, set.glideSeconds > 0.0f && v.amp.isActive(), set, fs);
                return;
            }
            Voice* pick = nullptr;
            for (int i = 0; i < set.voices; ++i)
                if (! voices[(size_t) i].amp.isActive()) { pick = &voices[(size_t) i]; break; }
            if (pick == nullptr)   // all busy: the oldest (a released one first)
                for (int i = 0; i < set.voices; ++i)
                {
                    auto& v = voices[(size_t) i];
                    if (pick == nullptr || (v.amp.isReleasing() && ! pick->amp.isReleasing())
                        || (v.amp.isReleasing() == pick->amp.isReleasing() && v.age < pick->age)) pick = &v;
                }
            pick->start (note, velocity, ++age, true, false, set, fs);
        }

        void noteOff (int note)
        {
            if (set.mode != VoiceMode::poly)
            {
                held.erase (std::remove (held.begin(), held.end(), note), held.end());
                auto& v = voices[0];
                if (v.note != note) return;
                if (! held.empty()) v.start (held.back(), v.velocity, ++age, set.mode == VoiceMode::mono, set.glideSeconds > 0.0f, set, fs);
                else v.release();
                return;
            }
            for (auto& v : voices) if (v.note == note && v.amp.isActive() && ! v.amp.isReleasing()) v.release();
        }

        // Every note released (or, with hard, silenced at once)
        void allNotesOff (bool hard = false)
        {
            held.clear();
            for (auto& v : voices) { if (hard) v.reset(); else v.release(); }
        }

        bool isSounding() const
        {
            for (auto& v : voices) if (v.amp.isActive()) return true;
            return false;
        }

        // Adds n samples of the synth into l and r
        void render (float* l, float* r, int n)
        {
            const float master = dbToGain (set.levelDb);
            for (int i = 0; i < n; ++i)
            {
                std::array<float, numLfos> lfo {};
                for (int k = 0; k < numLfos; ++k)
                    lfo[(size_t) k] = set.lfo[(size_t) k].target == LfoTarget::none ? 0.0f : lfos[(size_t) k].next (set.lfo[(size_t) k].hz, fs);
                float sl = 0.0f, sr = 0.0f;
                for (auto& v : voices)
                    if (v.amp.isActive()) v.renderSample (sl, sr, lfo, bend, set, fs);
                l[i] += sl * master;
                r[i] += sr * master;
            }
        }

    private:
        struct Voice
        {
            Envelope amp, filt;
            std::array<std::array<Oscillator, maxUnison>, numOscillators> osc;
            double s1[2][2] {}, s2[2][2] {};   // the filter's states: two sections, two sides
            int note = -1;
            float velocity = 0.0f;
            std::uint64_t age = 0;
            double pitch = 60.0, target = 60.0, glidePerSample = 0.0;
            int coefCountdown = 0;
            double g = 0.1, k = 1.4, h = 0.5;

            void prepare (double fs) { amp.prepare (fs); filt.prepare (fs); reset(); }
            void setEnvelopes (const Settings& s) { amp.setSettings (s.ampEnv); filt.setSettings (s.filterEnv); }
            void reset() { amp.reset(); filt.reset(); note = -1; std::fill (&s1[0][0], &s1[0][0] + 4, 0.0); std::fill (&s2[0][0], &s2[0][0] + 4, 0.0); }

            void start (int n, float vel, std::uint64_t a, bool retrigger, bool glide, const Settings& s, double fs)
            {
                const bool wasSilent = ! amp.isActive();
                note = n; velocity = vel; age = a; target = n;
                if (! glide || wasSilent) { pitch = n; glidePerSample = 0.0; }
                else glidePerSample = std::abs (target - pitch) / std::max (1.0, s.glideSeconds * fs);
                if (wasSilent)
                {
                    std::fill (&s1[0][0], &s1[0][0] + 4, 0.0);
                    std::fill (&s2[0][0], &s2[0][0] + 4, 0.0);
                    for (int o = 0; o < numOscillators; ++o)
                        for (int u = 0; u < maxUnison; ++u)
                        {
                            auto& x = osc[(size_t) o][(size_t) u];
                            x.setWave (s.osc[(size_t) o].wave);
                            x.setPhase (u * 0.37 + o * 0.11);   // copies start apart, so they don't sum to one loud click
                            x.seed ((std::uint32_t) (1 + o * maxUnison + u + n * 131));
                        }
                }
                if (retrigger || wasSilent) { amp.noteOn(); filt.noteOn(); }
                coefCountdown = 0;
            }

            void release() { amp.noteOff(); filt.noteOff(); }

            void renderSample (float& outL, float& outR, const std::array<float, numLfos>& lfo, float bend, const Settings& s, double fs)
            {
                if (glidePerSample > 0.0)
                {
                    if (pitch < target) pitch = std::min (target, pitch + glidePerSample);
                    else pitch = std::max (target, pitch - glidePerSample);
                }
                double pitchMod = bend, cutoffMod = 0.0, levelMod = 1.0, pwMod = 0.0;
                for (int k2 = 0; k2 < numLfos; ++k2)
                {
                    const auto& L = s.lfo[(size_t) k2];
                    const double v = lfo[(size_t) k2] * L.depth;
                    switch (L.target)
                    {
                        case LfoTarget::pitch: pitchMod += v; break;
                        case LfoTarget::filter: cutoffMod += v; break;
                        case LfoTarget::level: levelMod *= 1.0 - 0.5 * L.depth * (1.0 - lfo[(size_t) k2]); break;
                        case LfoTarget::pulseWidth: pwMod += v; break;
                        case LfoTarget::none: break;
                    }
                }

                // the oscillators
                double l = 0.0, r = 0.0;
                for (int o = 0; o < numOscillators; ++o)
                {
                    const auto& O = s.osc[(size_t) o];
                    if (! O.on) continue;
                    const double gain = gainFromDb (O.levelDb) / std::sqrt ((double) O.unison);
                    for (int u = 0; u < O.unison; ++u)
                    {
                        const double spreadPos = O.unison > 1 ? (2.0 * u / (O.unison - 1) - 1.0) : 0.0;
                        const double semis = pitch + pitchMod + O.semitones + (O.cents + O.detuneCents * 0.5 * spreadPos) / 100.0;
                        auto& x = osc[(size_t) o][(size_t) u];
                        x.setPulseWidth (O.pulseWidth + pwMod);
                        const double v = x.next (music::midiToHz (semis, 440.0), fs) * gain;
                        const double pan = std::clamp (O.pan + O.spread * spreadPos, -1.0, 1.0);
                        l += v * std::cos ((pan + 1.0) * 0.7853981633974483);
                        r += v * std::sin ((pan + 1.0) * 0.7853981633974483);
                    }
                }

                // the filter: its cutoff moves with the key, the envelope, velocity and an LFO
                const double fe = filt.next();
                if (s.filter.type != FilterType::off)
                {
                    if (--coefCountdown <= 0)
                    {
                        coefCountdown = 16;
                        const double semis = s.filter.keyTrack * (pitch - 60.0) + s.filter.envSemitones * fe
                                           + s.filter.velocity * 24.0 * (velocity - 0.5) + cutoffMod;
                        const double hz = std::clamp ((double) s.filter.hz * std::pow (2.0, semis / 12.0), 20.0, 0.45 * fs);
                        const auto c = svfCoefs<double> (fs, hz, 1.0 / std::clamp ((double) s.filter.resonance, 0.5, 10.0));
                        g = c.g; k = c.k; h = c.h;
                    }
                    auto pickOut = [&s] (const SvfOutputs<double>& o)
                    {
                        return s.filter.type == FilterType::highPass ? o.highPass : s.filter.type == FilterType::bandPass ? o.bandPass : o.lowPass;
                    };
                    l = pickOut (svfStep (l, s1[0], g, k, h));
                    r = pickOut (svfStep (r, s1[1], g, k, h));
                    if (s.filter.steep)
                    {
                        l = pickOut (svfStep (l, s2[0], g, k, h));
                        r = pickOut (svfStep (r, s2[1], g, k, h));
                    }
                }

                const double level = amp.next() * (1.0 - s.velocityToLevel + s.velocityToLevel * velocity) * levelMod;
                outL += (float) (l * level);
                outR += (float) (r * level);
                if (! amp.isActive()) note = -1;
            }
        };

        double fs = 48000.0;
        Settings set;
        std::array<Voice, maxVoices> voices;
        std::array<Oscillator, numLfos> lfos;
        std::vector<int> held;
        std::uint64_t age = 0;
        float bend = 0.0f;
    };
}
