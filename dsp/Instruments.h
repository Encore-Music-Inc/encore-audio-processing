#pragma once

// The instruments a score plays when it has no recording for them: one
// synthesizer sound for each family (piano, strings, brass, winds, plucked
// strings, bass, organ, voice, mallets, pizzicato and harmonics), each a
// setting of the one synthesizer (Synth.h). A family's sound is the note's
// oscillators through a low-pass that opens with the note's pitch and
// velocity, under its envelope; strings, winds and voices add a vibrato that
// fades in, winds a breath at the start of a note, voices a vowel peak.
//
// Times here are given as the time constant of an exponential curve (the way
// a score instrument fades), turned into the envelope's time to -80 dB.
//
// A recorded instrument is the sampler's zones (Sampler.h), read from its
// manifest: each zone's keys, velocities, root, tuning, loop, envelope,
// low-pass, pan, gain and exclusive group, the recording it plays, and the
// instrument's own gain (the format a sound set's builder writes, version 1).
//
//     vs::Synth s; s.prepare (fs); s.setSettings (*vs::instruments::synthSound ("strings"));
//     vs::Sampler p; p.prepare (fs); vs::instruments::loadRecorded (p, manifestText, recordings, trimDb);

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>
#include "Json.h"
#include "Sampler.h"
#include "Synth.h"

namespace vs::instruments
{
    // The families, in the order a list shows them
    inline constexpr std::array<const char*, 11> families { "piano", "strings", "brass", "winds", "pluck", "bass",
                                                            "organ", "voice", "mallets", "pizz", "harmonic" };

    namespace detail
    {
        struct Partial { Oscillator::Wave wave; double ratio, level; };   // frequency x ratio, level as a gain

        struct Family
        {
            const char* id;
            std::array<Partial, 4> partials;
            int count;
            int levelCount;               // the oscillators its level is set for (the organ keeps its fifth drawbar's share)
            double attack, decayTau, sustain, decay2Tau, sustain2, releaseTau;
            double bright;                // the cutoff's note-following multiple
            double vibratoHz, vibratoDepth;   // depth as a fraction of the frequency (0: none)
            bool pluck, swell, breath, vowel;
        };

        using W = Oscillator::Wave;
        inline const std::array<Family, 11>& table()
        {
            static const std::array<Family, 11> t { {
                { "piano",   { { { W::triangle, 1, 1 }, { W::sine, 2.002, 0.22 }, { W::sine, 3.01, 0.06 } } }, 3, 3, 0.004, 0.09, 0.55, 1.6, 0.0, 0.12, 7, 0, 0, false, false, false, false },
                { "strings", { { { W::sawUp, 1, 0.5 }, { W::sawUp, 1.0045, 0.4 }, { W::sawUp, 0.9962, 0.3 } } }, 3, 3, 0.11, 0, 1, 0, 0, 0.3, 2.8, 5.4, 0.0045, false, false, false, false },
                { "brass",   { { { W::sawUp, 1, 1 }, { W::square, 1, 0.12 } } }, 2, 2, 0.04, 0.18, 0.82, 0, 0, 0.12, 4.6, 0, 0, false, true, false, false },
                { "winds",   { { { W::triangle, 1, 1 }, { W::sine, 2, 0.28 }, { W::sine, 3, 0.1 } } }, 3, 3, 0.05, 0, 1, 0, 0, 0.1, 5, 4.9, 0.003, false, false, true, false },
                { "pluck",   { { { W::sawUp, 1, 0.6 }, { W::triangle, 1, 0.55 } } }, 2, 2, 0.002, 0.06, 0.5, 0.9, 0.0, 0.1, 9, 0, 0, true, false, false, false },
                { "bass",    { { { W::triangle, 1, 1 }, { W::sine, 1, 0.7 }, { W::sawUp, 1, 0.1 } } }, 3, 3, 0.006, 0.15, 0.65, 1.6, 0.2, 0.09, 3, 0, 0, false, false, false, false },
                // the organ's fifth drawbar (the 8th partial, 24 dB down) is left out: a sound has four oscillators
                { "organ",   { { { W::sine, 1, 1 }, { W::sine, 2, 0.55 }, { W::sine, 3, 0.3 }, { W::sine, 4, 0.18 } } }, 4, 5, 0.012, 0, 1, 0, 0, 0.06, 10, 0, 0, false, false, false, false },
                { "voice",   { { { W::sine, 1, 1 }, { W::triangle, 1, 0.3 }, { W::sine, 2, 0.12 } } }, 3, 3, 0.08, 0, 1, 0, 0, 0.2, 2.6, 5.6, 0.006, false, false, false, true },
                { "mallets", { { { W::sine, 1, 1 }, { W::sine, 3.99, 0.25 }, { W::sine, 10.1, 0.05 } } }, 3, 3, 0.002, 0.05, 0.4, 1.1, 0.0, 0.5, 12, 0, 0, true, false, false, false },
                { "pizz",    { { { W::triangle, 1, 1 }, { W::sine, 2, 0.3 } } }, 2, 2, 0.002, 0.04, 0.35, 0.35, 0.0, 0.08, 5, 0, 0, true, false, false, false },
                { "harmonic", { { { W::sine, 1, 1 }, { W::sine, 2, 0.05 } } }, 2, 2, 0.02, 0.3, 0.6, 2.5, 0.2, 0.3, 3, 0, 0, false, false, false, false },
            } };
            return t;
        }
    }

    // The family's synthesizer sound, or nothing for a name it doesn't know
    inline std::optional<Synth::Settings> synthSound (std::string_view family)
    {
        const detail::Family* f = nullptr;
        for (const auto& x : detail::table()) if (family == x.id) f = &x;
        if (f == nullptr) return std::nullopt;
        const auto envTime = [] (double tau) { return (float) Envelope::timeForTimeConstant (tau); };

        Synth::Settings s;
        for (int i = 0; i < Synth::numOscillators; ++i)
        {
            auto& o = s.osc[(size_t) i];
            o = {};
            if (i >= f->count) continue;
            const auto& p = f->partials[(size_t) i];
            const double cents = 1200.0 * std::log2 (p.ratio);
            o.on = true;
            o.wave = p.wave;
            o.semitones = (int) std::lround (cents / 100.0);
            o.cents = (float) (cents - 100.0 * o.semitones);
            // a band-limited saw or square overshoots at its jumps; Compose's played at a peak of 1, overshoot included
            const double overshoot = p.wave == Oscillator::Wave::sawUp || p.wave == Oscillator::Wave::square ? 1.0 / 1.1789797 : 1.0;
            o.levelDb = (float) (20.0 * std::log10 (p.level * overshoot));
            o.pulseWidth = 0.5f;
            // every wave starts rising through zero, as a sine does, so waves at one pitch add up
            o.startPhase = p.wave == Oscillator::Wave::triangle ? 0.25f : p.wave == Oscillator::Wave::sawUp ? 0.5f : 0.0f;
            o.unison = 1;
            o.spread = 0.0f;
        }
        // its level: the oscillators together, so a family with more of them isn't louder
        s.levelDb = (float) (20.0 * std::log10 (0.34 / std::sqrt (f->levelCount * 0.6)));
        s.velocityCurve = 1.5f;
        s.velocityToLevel = 1.0f;
        s.voices = Synth::maxVoices;

        s.ampEnv = { (float) f->attack, f->decayTau > 0 ? envTime (f->decayTau) : 0.001f, (float) f->sustain, envTime (f->releaseTau),
                     f->decay2Tau > 0 ? envTime (f->decay2Tau) : 0.0f, (float) f->sustain2 };

        // the low-pass: 450 Hz plus the note's frequency times its brightness, which velocity shades
        // from 0.45 (softest) to 1.35 (hardest), at most 14 kHz (Q 1.07: 0.6 dB of resonance)
        s.filter.type = Synth::FilterType::lowPass;
        s.filter.steep = false;
        s.filter.hz = 450.0f;
        s.filter.resonance = 1.0715193f;
        s.filter.noteFollow = (float) f->bright;
        s.filter.followSoft = 0.45f;
        s.filter.followHard = 1.35f;
        s.filter.maxHz = 14000.0f;
        s.filterEnv = { 0.0001f, 0.3f, 0.0f, 0.3f };
        if (f->pluck)
        {
            // a plucked note's cutoff falls from there toward 300 Hz plus 2.2 times the note's frequency
            s.filter.hz = 300.0f;
            s.filter.noteFollow = 2.2f;
            s.filter.followSoft = s.filter.followHard = 1.0f;
            s.filter.envSemitones = (float) (12.0 * std::log2 ((450.0 + 261.63 * f->bright * 1.088) / (300.0 + 2.2 * 261.63)));
            s.filter.velocity = 0.25f;
            s.filterEnv = { 0.0001f, envTime (0.18), 0.0f, envTime (f->releaseTau) };
        }
        if (f->swell)
        {
            // brass opens over the start of each note, from 350 Hz plus the note's frequency
            s.filter.envSemitones = (float) -(12.0 * std::log2 ((450.0 + 261.63 * f->bright * 1.088) / (350.0 + 261.63)));
            s.filterEnv = { 0.0001f, 0.25f, 0.0f, envTime (f->releaseTau) };
        }
        if (f->vibratoDepth > 0)
        {
            s.lfo[0] = { Oscillator::Wave::sine, (float) f->vibratoHz, (float) (12.0 * std::log2 (1.0 + f->vibratoDepth)), Synth::LfoTarget::pitch, 0.45f };
        }
        if (f->breath)
        {
            // a breath of noise as the note starts (not when slurred)
            auto& b = s.osc[(size_t) f->count];
            b = {};
            b.on = true;
            b.wave = Oscillator::Wave::noise;
            b.levelDb = (float) (20.0 * std::log10 (0.05 * 90.0 / 127.0));
            b.burstSeconds = 0.08f;
            b.unison = 1;
            b.spread = 0.0f;
        }
        if (f->vowel) s.peak = { true, 800.0f, 1.4f, 6.0f };
        return s;
    }

    // Whether a family's held notes follow a hairpin (a swell or fade within the note)
    inline bool swellsWithin (std::string_view family)
    {
        return family == "strings" || family == "winds" || family == "voice";
    }

    // ---- recorded instruments

    // A recorded instrument's level next to the synth sounds, and its velocity curve
    inline constexpr double recordedLevel = 0.62;
    inline constexpr float recordedVelocityCurve = 1.7f;
    // How many notes a recorded instrument plays at once (a pedalled piano rings with two zones a note)
    inline constexpr int recordedVoices = 96;

    // The recordings a manifest's zones play, by sample index; a missing one leaves its zones known but silent
    using Recordings = std::function<std::shared_ptr<const std::vector<float>> (int sample)>;

    // Loads a manifest's zones into the sampler (replacing its sounds), at trimDb on top of the
    // instrument's own gain. Returns the number of zones, or -1 for a manifest it can't read.
    inline int loadRecorded (Sampler& s, std::string_view manifest, const Recordings& recordings, double trimDb = 0.0)
    {
        const auto m = json::parse (manifest);
        if (! m) return -1;
        const auto* v = m->get ("v");
        const auto* zones = m->get ("zones");
        const auto* samples = m->get ("samples");
        if (v == nullptr || v->n != 1 || zones == nullptr || samples == nullptr || zones->items.empty()) return -1;
        const auto num = [] (const json::Json* o, std::string_view k, double d = 0.0) { const auto* x = o != nullptr ? o->get (k) : nullptr; return x != nullptr && x->type == json::Json::Type::number ? x->n : d; };
        const double gain = recordedLevel * std::pow (10.0, trimDb / 20.0) * num (&*m, "gain", 1.0);

        s.clearSounds();
        s.setVoiceCount (recordedVoices);
        s.setVelocityCurve (recordedVelocityCurve);
        std::vector<std::shared_ptr<const std::vector<float>>> have (samples->items.size());
        std::vector<bool> asked (samples->items.size(), false);
        int count = 0;
        for (const auto& z : zones->items)
        {
            const int si = (int) num (&z, "s", -1);
            if (si < 0 || si >= (int) samples->items.size()) return -1;
            const auto& smp = samples->items[(size_t) si];
            if (! asked[(size_t) si]) { have[(size_t) si] = recordings ? recordings (si) : nullptr; asked[(size_t) si] = true; }
            const auto* env = z.get ("env");
            Sampler::Sound snd;
            snd.sampleRate = num (&smp, "rate", 44100.0);
            snd.rootNote = (int) num (&z, "root", 60);
            snd.lowNote = (int) num (&z, "lo", 0);
            snd.highNote = (int) num (&z, "hi", 127);
            snd.gainDb = (float) (20.0 * std::log10 (gain));
            snd.pan = (float) num (&z, "pan");
            auto& q = snd.zone;
            q.on = true;
            q.lowVelocity = (int) num (&z, "vlo", 1);
            q.highVelocity = (int) num (&z, "vhi", 127);
            q.tuneCents = num (&z, "tune");
            q.centsPerKey = num (&z, "scale", 100.0);
            const int mode = (int) num (&z, "mode");
            if (mode == 1 || mode == 3)
            {
                q.loopStart = num (&smp, "ls", -1.0) + num (&z, "ls");
                q.loopEnd = num (&smp, "le", -1.0) + num (&z, "le");
            }
            q.delay = num (env, "delay");
            q.attack = num (env, "attack");
            q.hold = num (env, "hold");
            q.decay = num (env, "decay");
            q.sustain = num (env, "sustain", 1.0);
            q.release = num (env, "release", 0.03);
            q.lowPassHz = num (&z, "fc");
            q.lowPassQDb = num (&z, "q");
            q.exclusive = (int) num (&z, "excl");
            q.gain = (float) num (&z, "gain", 1.0);
            q.recording = have[(size_t) si];
            q.sample = si;
            s.addSound (std::move (snd));
            ++count;
        }
        return count;
    }

    // The recordings (by the manifest's sample index, in order) that notes (MIDI note, velocity 1..127)
    // play on a recorded instrument: what to fetch before they can play. Empty for a manifest it can't read.
    inline std::vector<int> recordingsFor (std::string_view manifest, const std::vector<std::pair<int, float>>& notes)
    {
        Sampler s;
        if (loadRecorded (s, manifest, nullptr) < 0) return {};
        std::vector<int> out;
        for (const auto& [midi, vel] : notes)
            for (int i : s.soundsFor (midi, std::clamp (vel, 1.0f, 127.0f) / 127.0f))
                out.push_back (s.sound (i).zone.sample);
        std::sort (out.begin(), out.end());
        out.erase (std::unique (out.begin(), out.end()), out.end());
        return out;
    }
}
