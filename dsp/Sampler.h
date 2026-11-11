#pragma once

// A sampler: sounds (mono or stereo recordings) laid across the keyboard,
// each over a range of notes from a root note, played back pitched by how
// far the note is from the root (4-point Hermite interpolation, DelayLine.h's
// one), at the sound's gain and pan and the note's velocity. A one-shot sound
// plays to its end whatever the note-off; otherwise a note-off releases it.
// A choke group stops the others in it when one plays (an open hat cut by a
// closed one). Tracktion Engine's sampler (GPLv3), rewritten.
//
//     Sampler s; s.prepare (fs); s.addSound (sound);
//     s.noteOn (36, 1.0f); ... s.render (l, r, n) (adds into l and r);

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
#include "DelayLine.h"
#include "Envelope.h"
#include "Util.h"

namespace vs
{
    class Sampler
    {
    public:
        static constexpr int maxVoices = 32;

        struct Sound
        {
            std::vector<float> left, right;    // right empty: mono
            double sampleRate = 48000.0;
            int rootNote = 60, lowNote = 0, highNote = 127;
            float gainDb = 0.0f, pan = 0.0f;   // pan -1 left .. 1 right
            bool oneShot = true;               // plays to its end (drums); else a note-off releases it
            int choke = 0;                     // 0 for none
            float releaseSeconds = 0.05f;      // how fast a released (or choked) note fades
        };

        void prepare (double sampleRate) { fs = sampleRate; allNotesOff (true); }

        // Adds a sound (allocates; not on the audio thread). Returns its index.
        int addSound (Sound s)
        {
            sounds.push_back (std::make_shared<const Sound> (std::move (s)));
            return (int) sounds.size() - 1;
        }
        void clearSounds() { allNotesOff (true); sounds.clear(); }
        int numSounds() const { return (int) sounds.size(); }
        const Sound& sound (int i) const { return *sounds[(size_t) i]; }

        void noteOn (int note, float velocity)
        {
            for (int si = 0; si < (int) sounds.size(); ++si)
            {
                const auto& snd = sounds[(size_t) si];
                if (note < snd->lowNote || note > snd->highNote || snd->left.empty()) continue;
                if (snd->choke != 0)
                    for (auto& v : voices)
                        if (v.active && v.sound != nullptr && v.sound->choke == snd->choke) v.release();
                Voice* pick = nullptr;
                for (auto& v : voices) if (! v.active) { pick = &v; break; }
                if (pick == nullptr)
                    for (auto& v : voices) if (pick == nullptr || v.age < pick->age) pick = &v;
                pick->start (snd, note, std::clamp (velocity, 0.0f, 1.0f), ++age, fs);
            }
        }

        void noteOff (int note)
        {
            for (auto& v : voices) if (v.active && v.note == note && ! v.sound->oneShot) v.release();
        }

        void allNotesOff (bool hard = false)
        {
            for (auto& v : voices) { if (hard) v.active = false; else v.release(); }
        }

        bool isSounding() const
        {
            for (auto& v : voices) if (v.active) return true;
            return false;
        }

        // Adds n samples into l and r
        void render (float* l, float* r, int n)
        {
            for (auto& v : voices) if (v.active) v.render (l, r, n);
        }

    private:
        struct Voice
        {
            std::shared_ptr<const Sound> sound;
            int note = -1;
            bool active = false;
            std::uint64_t age = 0;
            double pos = 0.0, step = 1.0;
            float gainL = 1.0f, gainR = 1.0f;
            Envelope env;

            void start (const std::shared_ptr<const Sound>& s, int n, float velocity, std::uint64_t a, double fs)
            {
                sound = s; note = n; age = a; active = true; pos = 0.0;
                step = std::pow (2.0, (n - s->rootNote) / 12.0) * s->sampleRate / fs;
                const float g = dbToGain (s->gainDb) * velocity;
                const double p = (std::clamp (s->pan, -1.0f, 1.0f) + 1.0) * 0.7853981633974483;
                // equal power, at unity in the centre
                gainL = g * (float) (std::cos (p) * 1.4142135623730951);
                gainR = g * (float) (std::sin (p) * 1.4142135623730951);
                env.prepare (fs);
                env.setSettings ({ 0.0f, 0.0f, 1.0f, s->releaseSeconds });
                env.reset();
                env.noteOn();
            }

            void release() { if (active) env.noteOff(); }

            void render (float* l, float* r, int n)
            {
                const auto& L = sound->left;
                const auto& R = sound->right.empty() ? sound->left : sound->right;
                const int len = (int) L.size();
                auto at = [len] (const std::vector<float>& x, int i) { return i >= 0 && i < len ? x[(size_t) i] : 0.0f; };
                for (int i = 0; i < n; ++i)
                {
                    if (pos >= len) { active = false; return; }
                    const int i0 = (int) pos;
                    const float t = (float) (pos - i0);
                    const float e = env.next();
                    if (! env.isActive()) { active = false; return; }
                    l[i] += hermite (at (L, i0 - 1), at (L, i0), at (L, i0 + 1), at (L, i0 + 2), t) * gainL * e;
                    r[i] += hermite (at (R, i0 - 1), at (R, i0), at (R, i0 + 1), at (R, i0 + 2), t) * gainR * e;
                    pos += step;
                }
            }
        };

        double fs = 48000.0;
        std::vector<std::shared_ptr<const Sound>> sounds;
        std::array<Voice, maxVoices> voices;
        std::uint64_t age = 0;
    };
}
