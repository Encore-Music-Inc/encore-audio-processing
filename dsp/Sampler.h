#pragma once

// A sampler: sounds (mono or stereo recordings) laid across the keyboard,
// each over a range of notes from a root note, played back pitched by how
// far the note is from the root (4-point Hermite interpolation, DelayLine.h's
// one), at the sound's gain and pan and the note's velocity. A one-shot sound
// plays to its end whatever the note-off; otherwise a note-off releases it.
// A choke group stops the others in it when one plays (an open hat cut by a
// closed one). A sound can also play just a region of its recording (start and
// end), fade to silence over its last fadeSeconds, be tuned by a fraction of a
// semitone, and cut itself off when hit again (retrigger: a drum pad); the
// velocity can follow a curve (velocity to the power velocityCurve). Tracktion
// Engine's sampler (GPLv3), rewritten.
//
//     Sampler s; s.prepare (fs); s.addSound (sound);
//     s.noteOn (36, 1.0f); ... s.render (l, r, n) (adds into l and r);

#include <algorithm>
#include <utility>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
#include "DelayLine.h"
#include "Envelope.h"
#include "Mix.h"
#include "Saturator.h"
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
            double startSeconds = 0.0;         // the region played, in the recording's seconds
            double endSeconds = 0.0;           // (at or before startSeconds: to the recording's end)
            double fadeSeconds = 0.0;          // a straight fade to silence over the region's last fadeSeconds as heard
            double pitchSemitones = 0.0;       // a tuning on top of the note's distance from the root
            bool retrigger = false;            // a hit cuts the sound's own earlier notes
        };

        // Velocity (0..1) to gain: velocity to the power `curve` (1: straight)
        void setVelocityCurve (float curve) { velocityCurve = std::max (0.0f, curve); }

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
                for (auto& v : voices)
                    if (v.active && v.sound != nullptr && ((snd->choke != 0 && v.sound->choke == snd->choke) || (snd->retrigger && v.sound == snd)))
                        v.release();
                Voice* pick = nullptr;
                for (auto& v : voices) if (! v.active) { pick = &v; break; }
                if (pick == nullptr)
                    for (auto& v : voices) if (pick == nullptr || v.age < pick->age) pick = &v;
                const float v = std::clamp (velocity, 0.0f, 1.0f);
                pick->start (snd, note, velocityCurve == 1.0f ? v : std::pow (v, velocityCurve), ++age, fs);
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
            double pos = 0.0, step = 1.0, end = 0.0;
            double fadeFrom = 0.0, fadeLen = 0.0;   // where the end fade starts and how long it is, in output samples
            std::int64_t played = 0;
            float gainL = 1.0f, gainR = 1.0f;
            Envelope env;

            void start (const std::shared_ptr<const Sound>& s, int n, float velocity, std::uint64_t a, double fs)
            {
                sound = s; note = n; age = a; active = true; played = 0;
                step = std::pow (2.0, (n - s->rootNote + s->pitchSemitones) / 12.0) * s->sampleRate / fs;
                // the region, in the recording's samples
                const double len = (double) s->left.size(), secs = len / s->sampleRate;
                const double from = std::min (std::max (0.0, s->startSeconds), std::max (0.0, secs - 0.001));
                pos = from * s->sampleRate;
                end = s->endSeconds > from ? std::max (pos + 1.0, std::min (s->endSeconds, secs) * s->sampleRate) : len;
                // the end fade, in output samples
                const double heard = (end - pos) / step;
                fadeLen = std::min (std::max (0.0, s->fadeSeconds * fs), heard);
                fadeFrom = heard - fadeLen;
                const float g = dbToGain (s->gainDb) * velocity;
                // the 3 dB pan law, raised 3 dB so the centre is at unity
                const auto pg = mix::panGains (g * 1.4142135623730951f, std::clamp (s->pan, -1.0f, 1.0f), mix::PanLaw::centre3dB);
                gainL = pg.left;
                gainR = pg.right;
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
                    if (pos >= end) { active = false; return; }
                    const int i0 = (int) pos;
                    const float t = (float) (pos - i0);
                    float e = env.next();
                    if (! env.isActive()) { active = false; return; }
                    if (fadeLen > 0.0 && (double) played >= fadeFrom) e *= (float) std::max (0.0, 1.0 - ((double) played - fadeFrom) / fadeLen);
                    l[i] += hermite (at (L, i0 - 1), at (L, i0), at (L, i0 + 1), at (L, i0 + 2), t) * gainL * e;
                    r[i] += hermite (at (R, i0 - 1), at (R, i0), at (R, i0 + 1), at (R, i0 + 2), t) * gainR * e;
                    pos += step;
                    ++played;
                }
            }
        };

        double fs = 48000.0;
        std::vector<std::shared_ptr<const Sound>> sounds;
        std::array<Voice, maxVoices> voices;
        std::uint64_t age = 0;
        float velocityCurve = 1.0f;
    };

    namespace sampler
    {
        // A hit in a loop: the note it plays, where (beats from the loop's start) and how hard (0..1)
        struct LoopHit { int note; double beat; float velocity; };

        // A loop as a sound that loops cleanly: the hits played through the sampler
        // (already loaded), each at its beat, with tailSeconds more so what rings past
        // the end can be folded back into the start, then the bus gain and the safety
        // curve (softClip), as a pad player sounds live. Returns loopBeats beats of
        // audio at fs (left, right). Hits at or after the loop's end are left out.
        inline std::pair<std::vector<float>, std::vector<float>> renderLoop (Sampler& s, std::vector<LoopHit> hits, double bpm, double loopBeats,
                                                                             double fs, double tailSeconds = 3.0, float busGain = 0.85f)
        {
            const double spb = 60.0 / bpm;
            const auto len = (std::int64_t) std::floor (loopBeats * spb * fs + 0.5);
            const auto total = (std::int64_t) std::ceil ((loopBeats * spb + tailSeconds) * fs);
            hits.erase (std::remove_if (hits.begin(), hits.end(), [&] (const LoopHit& h) { return ! (h.beat < loopBeats); }), hits.end());
            std::stable_sort (hits.begin(), hits.end(), [] (const LoopHit& a, const LoopHit& b) { return a.beat < b.beat; });
            s.allNotesOff (true);
            std::vector<float> l ((size_t) total, 0.0f), r ((size_t) total, 0.0f);
            std::int64_t at = 0;
            size_t next = 0;
            while (at < total)
            {
                while (next < hits.size() && (std::int64_t) std::floor (hits[next].beat * spb * fs + 0.5) <= at)
                {
                    s.noteOn (hits[next].note, hits[next].velocity);
                    ++next;
                }
                std::int64_t to = total;
                if (next < hits.size()) to = std::min (to, std::max (at + 1, (std::int64_t) std::floor (hits[next].beat * spb * fs + 0.5)));
                s.render (l.data() + at, r.data() + at, (int) (to - at));
                at = to;
            }
            std::vector<float> L ((size_t) len), R ((size_t) len);
            for (std::int64_t i = 0; i < total; ++i)
            {
                L[(size_t) (i % len)] += l[(size_t) i] * busGain;
                R[(size_t) (i % len)] += r[(size_t) i] * busGain;
            }
            for (auto& v : L) v = softClip (v);
            for (auto& v : R) v = softClip (v);
            return { std::move (L), std::move (R) };
        }
    }
}
