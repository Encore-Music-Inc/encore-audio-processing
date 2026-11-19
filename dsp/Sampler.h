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
// A sound can instead be one recording of an instrument (its zone): a range of
// notes and velocities, a tuning in cents and how far each key moves it, loop
// points, its own envelope (delay, attack, hold, decay, sustain, release, with
// decay and release falling to -100 dB over their times), a low-pass that
// softer notes close, and an exclusive group. A note plays the zones its key
// and velocity fall in (the nearest velocity layer if none does; the nearest
// key's zones for a note out of the recorded range), each through its
// envelope and filter, with the note's expression (NoteExpression.h), panned by
// the equal-power law (a mono recording at the centre 3 dB down each side, as
// it sits in a mix; one panned hard to a side at full level there).
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
#include "Biquad.h"
#include "Envelope.h"
#include "Mix.h"
#include "NoteExpression.h"
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

            // One recording of an instrument (on), played as the top of this file says
            struct Zone
            {
                bool on = false;
                int lowVelocity = 1, highVelocity = 127;   // MIDI velocities
                double tuneCents = 0.0, centsPerKey = 100.0;
                double loopStart = -1.0, loopEnd = -1.0;   // in the recording's samples; loops when loopEnd > loopStart + 8
                double delay = 0.0, attack = 0.0, hold = 0.0, decay = 0.0, sustain = 1.0, release = 0.03;   // seconds; sustain 0..1
                double lowPassHz = 0.0, lowPassQDb = 0.0;   // 0 Hz: none set (open, and velocity darkens it half as much)
                int exclusive = 0;                          // a note in a group cuts the group's earlier notes
                float gain = 1.0f;
                // the recording, shared by the zones that play it (instead of left; mono). None yet: the
                // zone is known but can't sound (ready() says so)
                std::shared_ptr<const std::vector<float>> recording;
            };
            Zone zone;

            // The samples a voice plays: a zone's shared recording, or left
            const std::vector<float>& leftData() const { return zone.on && zone.recording ? *zone.recording : left; }
            bool hasData() const { return ! leftData().empty(); }
        };

        // Velocity (0..1) to gain: velocity to the power `curve` (1: straight)
        void setVelocityCurve (float curve) { velocityCurve = std::max (0.0f, curve); }

        void prepare (double sampleRate) { fs = sampleRate; allNotesOff (true); }

        // How many notes can sound at once (allocates; not on the audio thread)
        void setVoiceCount (int n) { allNotesOff (true); voices.assign ((size_t) std::max (1, n), Voice {}); }

        // Adds a sound (allocates; not on the audio thread). Returns its index.
        int addSound (Sound s)
        {
            sounds.push_back (std::make_shared<const Sound> (std::move (s)));
            return (int) sounds.size() - 1;
        }
        void clearSounds() { allNotesOff (true); sounds.clear(); }
        int numSounds() const { return (int) sounds.size(); }
        const Sound& sound (int i) const { return *sounds[(size_t) i]; }

        // Whether the instrument's zones can play a note: it has zones, and every one the note plays has its recording
        bool ready (int note, float velocity) const
        {
            const auto zs = zonesFor (note, std::clamp (velocity, 0.0f, 1.0f));
            if (zs.empty()) return false;
            for (int si : zs) if (! sounds[(size_t) si]->hasData()) return false;
            return true;
        }

        // Plays a note on the instrument's zones (and any plain sounds in its range)
        // with expression; returns the note's id, for noteOffId (0: its zones aren't
        // ready, and nothing played)
        std::uint64_t noteOn (int note, float velocity, const NoteExpression& e)
        {
            const float vel = std::clamp (velocity, 0.0f, 1.0f);
            bool zones = false;
            for (const auto& x : sounds) zones = zones || x->zone.on;
            if (zones && ! ready (note, vel)) return 0;
            const std::uint64_t id = ++age;
            for (int si : zonesFor (note, vel))
            {
                const auto& snd = sounds[(size_t) si];
                if (snd->zone.exclusive != 0)
                    for (auto& v : voices)
                        if (v.active && v.sound != nullptr && v.sound->zone.exclusive == snd->zone.exclusive && v.id != id) v.release();
                pickVoice().start (snd, note, vel, velocityCurve == 1.0f ? vel : std::pow (vel, velocityCurve), id, fs, &e);
            }
            for (int si = 0; si < (int) sounds.size(); ++si)
                if (! sounds[(size_t) si]->zone.on) playPlain (si, note, vel, id);
            return id;
        }

        // Releases the voices of the note noteOn returned
        void noteOffId (std::uint64_t id)
        {
            for (auto& v : voices) if (v.active && v.id == id && ! (v.sound->oneShot && ! v.sound->zone.on)) v.release();
        }

        void noteOn (int note, float velocity)
        {
            const std::uint64_t id = ++age;
            if (ready (note, std::clamp (velocity, 0.0f, 1.0f)))
                for (int si : zonesFor (note, std::clamp (velocity, 0.0f, 1.0f)))
                    pickVoice().start (sounds[(size_t) si], note, std::clamp (velocity, 0.0f, 1.0f), std::pow (std::clamp (velocity, 0.0f, 1.0f), velocityCurve), id, fs, nullptr);
            for (int si = 0; si < (int) sounds.size(); ++si)
                if (! sounds[(size_t) si]->zone.on) playPlain (si, note, std::clamp (velocity, 0.0f, 1.0f), id);
        }

    private:
        void playPlain (int si, int note, float velocity, std::uint64_t id)
        {
                const auto& snd = sounds[(size_t) si];
                if (note < snd->lowNote || note > snd->highNote || snd->left.empty()) return;
                for (auto& v : voices)
                    if (v.active && v.sound != nullptr && ((snd->choke != 0 && v.sound->choke == snd->choke) || (snd->retrigger && v.sound == snd)))
                        v.release();
                const float v = std::clamp (velocity, 0.0f, 1.0f);
                pickVoice().start (snd, note, v, velocityCurve == 1.0f ? v : std::pow (v, velocityCurve), id, fs, nullptr);
        }

        // A silent voice, or the oldest
        struct Voice;
        Voice& pickVoice()
        {
            Voice* pick = nullptr;
            for (auto& v : voices) if (! v.active) return v;
            for (auto& v : voices) if (pick == nullptr || v.age < pick->age) pick = &v;
            return *pick;
        }

        // The zones a note plays: those its key and velocity fall in; within its key's zones, the
        // nearest velocity layer if none matches; out of every zone's keys, the nearest key's zones
        // at its velocity (or the first of them)
        std::vector<int> zonesFor (int note, float velocity) const
        {
            const int v = std::clamp ((int) std::lround (velocity * 127.0f), 1, 127);
            std::vector<int> byKey, hit;
            for (int i = 0; i < (int) sounds.size(); ++i)
            {
                const auto& s = *sounds[(size_t) i];
                if (s.zone.on && note >= s.lowNote && note <= s.highNote) byKey.push_back (i);
            }
            auto inVel = [&] (int i) { const auto& z = sounds[(size_t) i]->zone; return v >= z.lowVelocity && v <= z.highVelocity; };
            for (int i : byKey) if (inVel (i)) hit.push_back (i);
            if (! hit.empty()) return hit;
            if (! byKey.empty())
            {
                auto d = [&] (int i) { const auto& z = sounds[(size_t) i]->zone; return v < z.lowVelocity ? z.lowVelocity - v : v - z.highVelocity; };
                int best = 1 << 30;
                for (int i : byKey) best = std::min (best, d (i));
                for (int i : byKey) if (d (i) == best) hit.push_back (i);
                return hit;
            }
            int best = 1 << 30;
            auto dk = [&] (int i) { const auto& s = *sounds[(size_t) i]; return note < s.lowNote ? s.lowNote - note : note - s.highNote; };
            std::vector<int> all;
            for (int i = 0; i < (int) sounds.size(); ++i) if (sounds[(size_t) i]->zone.on) all.push_back (i);
            for (int i : all) best = std::min (best, dk (i));
            for (int i : all) if (dk (i) == best && inVel (i)) hit.push_back (i);
            if (hit.empty()) for (int i : all) if (dk (i) == best) { hit.push_back (i); break; }
            return hit;
        }

    public:

        void noteOff (int note)
        {
            for (auto& v : voices) if (v.active && v.note == note && (v.sound->zone.on || ! v.sound->oneShot)) v.release();
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
            std::uint64_t age = 0, id = 0;
            // an instrument zone's note
            NoteExpression ex;
            double fsOut = 48000.0, peak = 1.0, startLevel = 0.0, attackEnd = 0.0, heldAt = 0.0, decayTau = 0.0, target = 0.0;
            double releaseAt = -1.0, releaseFrom = 0.0, releaseTau = 0.0, releaseLen = 0.0, level = 0.0, bendCents = 0.0, rampFrom = 0.0;
            bool decays = false, swells = false, filtered = false, releasing = false;
            Biquad lp;
            double pos = 0.0, step = 1.0, end = 0.0;
            double fadeFrom = 0.0, fadeLen = 0.0;   // where the end fade starts and how long it is, in output samples
            std::int64_t played = 0;
            float gainL = 1.0f, gainR = 1.0f;
            Envelope env;

            void start (const std::shared_ptr<const Sound>& s, int n, float rawVelocity, float velocity, std::uint64_t a, double fs, const NoteExpression* e)
            {
                sound = s; note = n; age = a; id = a; active = true; played = 0;
                if (s->zone.on) { startZone (rawVelocity, velocity, fs, e != nullptr ? *e : NoteExpression {}); return; }
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

            void startZone (float rawVelocity, float velocity, double fs, const NoteExpression& e)
            {
                const auto& z = sound->zone;
                ex = e;
                fsOut = fs;
                step = std::pow (2.0, ((note - sound->rootNote) * z.centsPerKey + z.tuneCents + sound->pitchSemitones * 100.0) / 1200.0) * sound->sampleRate / fs;
                bendCents = e.bendSemitones * z.centsPerKey;
                pos = 0.0;
                end = (double) sound->leftData().size();
                peak = dbToGain (sound->gainDb) * z.gain * velocity * e.gain;
                // the envelope: from silence (or at once, with no attack; a slurred note from part-way) to the top
                const bool ramp = e.slurred || z.attack > 0.001;
                const double att = e.slurred ? std::max (z.attack, (double) e.slurAttack) : z.attack;
                startLevel = e.slurred ? peak * e.slurLevel : ramp ? 0.0 : peak;
                attackEnd = ramp ? z.delay + att : 0.0;
                heldAt = z.delay + att + z.hold;
                decays = z.decay > 0.0 && z.sustain < 0.999;
                decayTau = std::max (0.004, z.decay / 11.512925464970229);
                target = peak * z.sustain;
                swells = ! decays && e.swellUntil > 0.0f;
                rampFrom = ramp ? attackEnd : z.delay;
                releaseAt = -1.0;
                releasing = false;
                releaseTau = std::max (0.004, std::max (0.03, z.release) / 11.512925464970229);
                releaseLen = std::max (0.03, z.release) + 0.1;
                level = startLevel;
                // the low-pass: softer notes darker (by up to 24 semitones; 12 with none set), shaded by the note's brightness
                const double v = rawVelocity * 127.0;
                double fc = z.lowPassHz > 0.0 ? z.lowPassHz : 20000.0;
                fc *= std::pow (2.0, -2400.0 * (1.0 - v / 127.0) / 1200.0 * (z.lowPassHz > 0.0 ? 1.0 : 0.5));
                fc *= e.brightness;
                if (e.brightness > 1.0f) fc = std::min (20000.0, fc);
                filtered = fc < 16000.0;
                lp.reset();
                if (filtered)
                {
                    const auto k = BiquadCoefs::rbjRaw (BiquadCoefs::Type::lowPass, fs, std::min (std::max (80.0, fc), fs * 0.5),
                                                        std::pow (10.0, (z.lowPassQDb != 0.0 ? z.lowPassQDb : 0.0001) / 20.0));
                    lp.c = BiquadCoefs::make (k.b0, k.b1, k.b2, k.a0, k.a1, k.a2);
                }
                // the equal-power law
                const auto pg = mix::panGains (1.0f, std::clamp (sound->pan, -1.0f, 1.0f), mix::PanLaw::centre3dB);
                gainL = pg.left;
                gainR = pg.right;
            }

            // The zone's level t seconds into the note
            double zoneLevel (double t)
            {
                if (releasing) return releaseFrom * std::exp (-(t - releaseAt) / releaseTau);
                if (t < attackEnd) return startLevel + (peak - startLevel) * t / attackEnd;
                if (decays) return t < heldAt ? peak : target + (peak - target) * std::exp (-(t - heldAt) / decayTau);
                if (swells && t >= rampFrom)
                {
                    const double to = ex.swellUntil, top = peak * ex.swellTo;
                    return t >= to ? top : peak + (top - peak) * (t - rampFrom) / std::max (1.0e-6, to - rampFrom);
                }
                return peak;
            }

            void release()
            {
                if (! active) return;
                if (sound != nullptr && sound->zone.on)
                {
                    // never before 20 ms into the note
                    if (releaseAt < 0.0) releaseAt = std::max ((double) played / fsOut, 0.02);
                    return;
                }
                env.noteOff();
            }

            void renderZone (float* l, float* r, int n)
            {
                const auto& L = sound->leftData();
                const auto& R = sound->right.empty() ? L : sound->right;
                const int len = (int) L.size();
                const auto& z = sound->zone;
                // a loop past the recording's end turns at its end (a recording can come out a little shorter than its loop)
                const bool loops = z.loopEnd > z.loopStart + 8.0 && z.loopStart >= 0.0 && z.loopStart < len;
                const double loopEnd = std::min (z.loopEnd, (double) len);
                auto at = [len] (const std::vector<float>& x, int i) { return i >= 0 && i < len ? x[(size_t) i] : 0.0f; };
                for (int i = 0; i < n; ++i)
                {
                    const double t = (double) played / fsOut;
                    // the release falls from where the level is when it starts
                    if (releaseAt >= 0.0 && t >= releaseAt && ! releasing) { releaseFrom = level; releasing = true; }
                    if (releasing && t >= releaseAt + releaseLen) { active = false; return; }
                    if (pos >= end && ! loops) { active = false; return; }
                    level = zoneLevel (t);
                    const int i0 = (int) pos;
                    const float u = (float) (pos - i0);
                    double xl = hermite (at (L, i0 - 1), at (L, i0), at (L, i0 + 1), at (L, i0 + 2), u);
                    double xr = sound->right.empty() ? xl : (double) hermite (at (R, i0 - 1), at (R, i0), at (R, i0 + 1), at (R, i0 + 2), u);
                    if (filtered) { xl = lp.process (0, (float) xl); xr = lp.process (1, (float) xr); }
                    l[i] += (float) (xl * level * gainL);
                    r[i] += (float) (xr * level * gainR);
                    double st = step;
                    if (bendCents != 0.0 && t > ex.bendFrom)
                        st *= std::pow (2.0, bendCents / 1200.0 * std::min (1.0, (t - ex.bendFrom) / std::max (1.0e-6, (double) (ex.bendTo - ex.bendFrom))));
                    pos += st;
                    if (loops && pos >= loopEnd) pos -= loopEnd - z.loopStart;
                    ++played;
                }
            }

            void render (float* l, float* r, int n)
            {
                if (sound->zone.on) { renderZone (l, r, n); return; }
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
        std::vector<Voice> voices = std::vector<Voice> ((size_t) maxVoices);
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
