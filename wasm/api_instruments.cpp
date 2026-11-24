// The browser's instruments (in_*): the synth, the sampler and a score played
// by its instruments (the ensemble). Plain C
// functions; notes go in as calls (in_synth_note_on (h, note, velocity)) at
// the block they start in, and in_<instrument>_render (h, n) writes n samples
// into its own left and right buffers (up to maxBlock).
// Built with the other wasm/api_*.cpp files into master.wasm by wasm/build.sh.
#include <emscripten/emscripten.h>
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "dsp/Ensemble.h"
#include "dsp/Sampler.h"
#include "dsp/Synth.h"

namespace
{
    struct SynthIn { vs::Synth s; std::vector<float> l, r; };
    struct SamplerIn { vs::Sampler s; std::vector<float> l, r, loadL, loadR, loopL, loopR; };
    struct EnsembleIn
    {
        vs::Ensemble e;
        std::map<int, std::shared_ptr<std::vector<float>>> staged;   // a recorded instrument's recordings, by sample
        std::vector<float> l, r;
    };
    template <typename T> T* as (void* p) { return static_cast<T*> (p); }
}

extern "C"
{
    // ---- Synth
    EMSCRIPTEN_KEEPALIVE void* in_synth_create (double fs, int maxBlock)
    {
        auto* x = new SynthIn;
        x->s.prepare (fs);
        x->l.assign ((size_t) maxBlock, 0.0f); x->r.assign ((size_t) maxBlock, 0.0f);
        return x;
    }
    EMSCRIPTEN_KEEPALIVE void in_synth_destroy (void* p) { delete as<SynthIn> (p); }
    EMSCRIPTEN_KEEPALIVE float* in_synth_left (void* p)  { return as<SynthIn> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* in_synth_right (void* p) { return as<SynthIn> (p)->r.data(); }

    // One oscillator (i 0..3); wave: 0 sine, 1 triangle, 2 saw up, 3 saw down, 4 pulse, 5 noise
    EMSCRIPTEN_KEEPALIVE void in_synth_osc (void* p, int i, int on, int wave, int semitones, float cents, float levelDb, float pulseWidth,
                                            int unison, float detuneCents, float spread, float pan)
    {
        auto* x = as<SynthIn> (p);
        auto set = x->s.getSettings();
        auto& o = set.osc[(size_t) std::clamp (i, 0, vs::Synth::numOscillators - 1)];
        o.on = on != 0; o.wave = (vs::Oscillator::Wave) std::clamp (wave, 0, 5); o.semitones = semitones; o.cents = cents;
        o.levelDb = levelDb; o.pulseWidth = pulseWidth; o.unison = unison; o.detuneCents = detuneCents; o.spread = spread; o.pan = pan;
        x->s.setSettings (set);
    }
    // type: 0 off, 1 low pass, 2 band pass, 3 high pass
    EMSCRIPTEN_KEEPALIVE void in_synth_filter (void* p, int type, int steep, float hz, float resonance, float keyTrack, float envSemitones, float velocity)
    {
        auto* x = as<SynthIn> (p);
        auto set = x->s.getSettings();
        set.filter = { (vs::Synth::FilterType) std::clamp (type, 0, 3), steep != 0, hz, resonance, keyTrack, envSemitones, velocity };
        x->s.setSettings (set);
    }
    // which: 0 the amp envelope, 1 the filter's
    EMSCRIPTEN_KEEPALIVE void in_synth_envelope (void* p, int which, float attack, float decay, float sustain, float release)
    {
        auto* x = as<SynthIn> (p);
        auto set = x->s.getSettings();
        (which == 0 ? set.ampEnv : set.filterEnv) = { attack, decay, sustain, release };
        x->s.setSettings (set);
    }
    // target: 0 none, 1 pitch, 2 filter, 3 level, 4 pulse width
    EMSCRIPTEN_KEEPALIVE void in_synth_lfo (void* p, int i, int wave, float hz, float depth, int target)
    {
        auto* x = as<SynthIn> (p);
        auto set = x->s.getSettings();
        set.lfo[(size_t) std::clamp (i, 0, vs::Synth::numLfos - 1)] = { (vs::Oscillator::Wave) std::clamp (wave, 0, 5), hz, depth, (vs::Synth::LfoTarget) std::clamp (target, 0, 4) };
        x->s.setSettings (set);
    }
    // mode: 0 poly, 1 mono, 2 legato
    EMSCRIPTEN_KEEPALIVE void in_synth_voices (void* p, int mode, int voices, float glideSeconds, float velocityToLevel, float levelDb)
    {
        auto* x = as<SynthIn> (p);
        auto set = x->s.getSettings();
        set.mode = (vs::Synth::VoiceMode) std::clamp (mode, 0, 2); set.voices = voices; set.glideSeconds = glideSeconds;
        set.velocityToLevel = velocityToLevel; set.levelDb = levelDb;
        x->s.setSettings (set);
    }
    EMSCRIPTEN_KEEPALIVE void in_synth_note_on (void* p, int note, float velocity) { as<SynthIn> (p)->s.noteOn (note, velocity); }
    EMSCRIPTEN_KEEPALIVE void in_synth_note_off (void* p, int note) { as<SynthIn> (p)->s.noteOff (note); }
    EMSCRIPTEN_KEEPALIVE void in_synth_all_off (void* p, int hard) { as<SynthIn> (p)->s.allNotesOff (hard != 0); }
    EMSCRIPTEN_KEEPALIVE void in_synth_bend (void* p, float semitones) { as<SynthIn> (p)->s.setPitchBend (semitones); }
    EMSCRIPTEN_KEEPALIVE int in_synth_sounding (void* p) { return as<SynthIn> (p)->s.isSounding() ? 1 : 0; }
    EMSCRIPTEN_KEEPALIVE void in_synth_render (void* p, int n)
    {
        auto* x = as<SynthIn> (p);
        std::fill (x->l.begin(), x->l.begin() + n, 0.0f);
        std::fill (x->r.begin(), x->r.begin() + n, 0.0f);
        x->s.render (x->l.data(), x->r.data(), n);
    }

    // ---- Sampler: load a sound by filling in_sampler_load_left (h, n) (and
    // _right for stereo), then in_sampler_add (h, ...)
    EMSCRIPTEN_KEEPALIVE void* in_sampler_create (double fs, int maxBlock)
    {
        auto* x = new SamplerIn;
        x->s.prepare (fs);
        x->l.assign ((size_t) maxBlock, 0.0f); x->r.assign ((size_t) maxBlock, 0.0f);
        return x;
    }
    EMSCRIPTEN_KEEPALIVE void in_sampler_destroy (void* p) { delete as<SamplerIn> (p); }
    EMSCRIPTEN_KEEPALIVE float* in_sampler_left (void* p)  { return as<SamplerIn> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* in_sampler_right (void* p) { return as<SamplerIn> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE float* in_sampler_load_left (void* p, int n)  { auto* x = as<SamplerIn> (p); x->loadL.assign ((size_t) n, 0.0f); return x->loadL.data(); }
    EMSCRIPTEN_KEEPALIVE float* in_sampler_load_right (void* p, int n) { auto* x = as<SamplerIn> (p); x->loadR.assign ((size_t) n, 0.0f); return x->loadR.data(); }
    EMSCRIPTEN_KEEPALIVE int in_sampler_add (void* p, int stereo, double sampleRate, int rootNote, int lowNote, int highNote, float gainDb, float pan,
                                             int oneShot, int choke, float releaseSeconds)
    {
        auto* x = as<SamplerIn> (p);
        vs::Sampler::Sound s;
        s.left = std::move (x->loadL);
        if (stereo) s.right = std::move (x->loadR);
        x->loadL.clear(); x->loadR.clear();
        s.sampleRate = sampleRate; s.rootNote = rootNote; s.lowNote = lowNote; s.highNote = highNote;
        s.gainDb = gainDb; s.pan = pan; s.oneShot = oneShot != 0; s.choke = choke; s.releaseSeconds = releaseSeconds;
        return x->s.addSound (std::move (s));
    }
    EMSCRIPTEN_KEEPALIVE void in_sampler_clear (void* p) { as<SamplerIn> (p)->s.clearSounds(); }
    EMSCRIPTEN_KEEPALIVE void in_sampler_note_on (void* p, int note, float velocity) { as<SamplerIn> (p)->s.noteOn (note, velocity); }
    EMSCRIPTEN_KEEPALIVE void in_sampler_note_off (void* p, int note) { as<SamplerIn> (p)->s.noteOff (note); }
    EMSCRIPTEN_KEEPALIVE void in_sampler_all_off (void* p, int hard) { as<SamplerIn> (p)->s.allNotesOff (hard != 0); }
    EMSCRIPTEN_KEEPALIVE int in_sampler_sounding (void* p) { return as<SamplerIn> (p)->s.isSounding() ? 1 : 0; }
    EMSCRIPTEN_KEEPALIVE void in_sampler_render (void* p, int n)
    {
        auto* x = as<SamplerIn> (p);
        std::fill (x->l.begin(), x->l.begin() + n, 0.0f);
        std::fill (x->r.begin(), x->r.begin() + n, 0.0f);
        x->s.render (x->l.data(), x->r.data(), n);
    }

    // ---- a pad (a drum pad's sound): the region played (start, end seconds; end at or before start:
    // to the recording's end), an end fade (seconds as heard), a tuning in semitones, its volume
    // (linear gain), and whether a hit cuts its own earlier notes; it plays on `note` alone
    EMSCRIPTEN_KEEPALIVE int in_sampler_add_pad (void* p, int stereo, double sampleRate, int note, float volume, float pan, int choke, int retrigger,
                                                 double startSeconds, double endSeconds, double fadeSeconds, double pitchSemitones, float releaseSeconds)
    {
        auto* x = as<SamplerIn> (p);
        vs::Sampler::Sound s;
        s.left = std::move (x->loadL);
        if (stereo) s.right = std::move (x->loadR);
        x->loadL.clear(); x->loadR.clear();
        s.sampleRate = sampleRate; s.rootNote = s.lowNote = s.highNote = note;
        s.gainDb = vs::ampToDb (std::max (0.0f, volume), 0.0f); s.pan = pan; s.oneShot = true; s.choke = choke; s.retrigger = retrigger != 0;
        s.startSeconds = startSeconds; s.endSeconds = endSeconds; s.fadeSeconds = fadeSeconds; s.pitchSemitones = pitchSemitones; s.releaseSeconds = releaseSeconds;
        return x->s.addSound (std::move (s));
    }
    // velocity (0..1) to gain: velocity to the power `curve`
    EMSCRIPTEN_KEEPALIVE void in_sampler_velocity_curve (void* p, float curve) { as<SamplerIn> (p)->s.setVelocityCurve (curve); }

    // ---- a loop through the sampler (vs::sampler::renderLoop): count hits (notes, beats, velocities);
    // returns the loop's length in frames, its left and right then at in_sampler_loop_left / _right
    EMSCRIPTEN_KEEPALIVE int in_sampler_render_loop (void* p, const int* notes, const double* beats, const float* velocities, int count,
                                                     double bpm, double loopBeats, double fs, double tailSeconds, float busGain)
    {
        auto* x = as<SamplerIn> (p);
        std::vector<vs::sampler::LoopHit> hits;
        for (int i = 0; i < count; ++i) hits.push_back ({ notes[i], beats[i], velocities[i] });
        auto [l, r] = vs::sampler::renderLoop (x->s, std::move (hits), bpm, loopBeats, fs, tailSeconds, busGain);
        x->loopL = std::move (l);
        x->loopR = std::move (r);
        return (int) x->loopL.size();
    }
    EMSCRIPTEN_KEEPALIVE const float* in_sampler_loop_left (void* p) { return as<SamplerIn> (p)->loopL.data(); }
    EMSCRIPTEN_KEEPALIVE const float* in_sampler_loop_right (void* p) { return as<SamplerIn> (p)->loopR.data(); }

    // ---- a score played by its instruments (vs::Ensemble)
    EMSCRIPTEN_KEEPALIVE void* in_ensemble_create (double fs)
    {
        auto* x = new EnsembleIn;
        x->e.prepare (fs);
        return x;
    }
    EMSCRIPTEN_KEEPALIVE void in_ensemble_destroy (void* p) { delete as<EnsembleIn> (p); }
    // a family's synth sound (Instruments.h); returns the instrument, or -1
    EMSCRIPTEN_KEEPALIVE int in_ensemble_add_synth (void* p, const char* family) { return as<EnsembleIn> (p)->e.addSynth (family); }
    // n samples of a recorded instrument's recording (by its manifest's sample index), to fill before in_ensemble_add_recorded
    EMSCRIPTEN_KEEPALIVE float* in_ensemble_recording (void* p, int sample, int n)
    {
        auto v = std::make_shared<std::vector<float>> ((size_t) std::max (0, n), 0.0f);
        as<EnsembleIn> (p)->staged[sample] = v;
        return v->data();
    }
    // a recorded instrument from its manifest (JSON text) and the recordings filled in so far, at trimDb, with the
    // family's synth sound for notes those can't play; returns the instrument, or -1
    EMSCRIPTEN_KEEPALIVE int in_ensemble_add_recorded (void* p, const char* manifest, double trimDb, const char* family)
    {
        auto* x = as<EnsembleIn> (p);
        auto have = std::make_shared<std::map<int, std::shared_ptr<std::vector<float>>>> (std::move (x->staged));
        x->staged.clear();
        return x->e.addRecorded (manifest, [have] (int i) -> std::shared_ptr<const std::vector<float>> {
            const auto it = have->find (i);
            return it == have->end() ? nullptr : it->second;
        }, trimDb, family);
    }
    // the parts: count volumes (0.8: as recorded) and pans (-1 .. 1)
    EMSCRIPTEN_KEEPALIVE void in_ensemble_parts (void* p, int count, const float* volumes, const float* pans)
    {
        std::vector<vs::Ensemble::Part> parts;
        for (int i = 0; i < count; ++i) parts.push_back ({ volumes[i], pans[i] });
        as<EnsembleIn> (p)->e.setParts (std::move (parts));
    }
    // count notes (times and durations in seconds, MIDI notes, velocities 1..127 and where a swell ends (0: none),
    // parts, instruments, flags: 1 slurred, 2 muted; techniques: 0 none, 1 pizz, 2 harmonic, 3 bright, 4 dark;
    // a glide's note (-1: none) and where it starts) rendered at once; normalize: to -1 dB sample peak; onlyCount
    // parts in only (0: every part). Returns the frames, then at in_ensemble_left / _right.
    EMSCRIPTEN_KEEPALIVE int in_ensemble_render (void* p, int count, const double* times, const double* durs, const int* midis, const float* vels,
                                                 const float* velEnds, const int* parts, const int* insts, const int* flags, const int* techs,
                                                 const int* bendTos, const float* bendAts, int normalize, int onlyCount, const int* only)
    {
        auto* x = as<EnsembleIn> (p);
        std::vector<vs::Ensemble::Note> notes ((size_t) std::max (0, count));
        for (int i = 0; i < count; ++i)
        {
            auto& n = notes[(size_t) i];
            n.time = times[i]; n.dur = durs[i]; n.midi = midis[i]; n.velocity = vels[i]; n.velocityEnd = velEnds[i];
            n.part = parts[i]; n.instrument = insts[i]; n.slurred = (flags[i] & 1) != 0; n.muted = (flags[i] & 2) != 0;
            n.technique = (vs::Ensemble::Technique) std::clamp (techs[i], 0, 4); n.bendTo = bendTos[i]; n.bendAt = bendAts[i];
        }
        auto [l, r] = x->e.render (std::move (notes), normalize != 0, std::vector<int> (only, only + std::max (0, onlyCount)));
        x->l = std::move (l);
        x->r = std::move (r);
        return (int) x->l.size();
    }
    // The recordings (manifest sample indexes, in order, at most cap into out) that count notes (MIDI notes,
    // velocities 1..127) play on a recorded instrument; returns how many (-1 for a manifest it can't read)
    EMSCRIPTEN_KEEPALIVE int in_recordings_for (const char* manifest, int count, const int* midis, const float* vels, int* out, int cap)
    {
        std::vector<std::pair<int, float>> notes;
        for (int i = 0; i < count; ++i) notes.push_back ({ midis[i], vels[i] });
        vs::Sampler probe;
        if (vs::instruments::loadRecorded (probe, manifest, nullptr) < 0) return -1;
        const auto need = vs::instruments::recordingsFor (manifest, notes);
        for (size_t i = 0; i < need.size() && (int) i < cap; ++i) out[i] = need[i];
        return (int) need.size();
    }
    EMSCRIPTEN_KEEPALIVE const float* in_ensemble_left (void* p) { return as<EnsembleIn> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE const float* in_ensemble_right (void* p) { return as<EnsembleIn> (p)->r.data(); }

    // ---- the score played live, for an AudioWorklet: the clock is the worklet's, in frames (a time in
    // seconds is frame / fs). Notes are scheduled ahead (or held: dur 0 until in_ensemble_release), what
    // hasn't started is forgotten from a time, and in_ensemble_live_render writes n frames at frame
    // into in_ensemble_left / _right, returning 1 while anything sounds or is still to come.
    EMSCRIPTEN_KEEPALIVE void in_ensemble_live_start (void* p) { as<EnsembleIn> (p)->e.startLive(); }
    // one note: as in_ensemble_render's, with its gain (1: as played); returns its id
    EMSCRIPTEN_KEEPALIVE int in_ensemble_schedule (void* p, double time, double dur, int midi, float vel, float velEnd, int part, int inst, int flags,
                                                   int tech, int bendTo, float bendAt, float gain)
    {
        vs::Ensemble::Note n;
        n.time = time; n.dur = dur; n.midi = midi; n.velocity = vel; n.velocityEnd = velEnd; n.part = part; n.instrument = inst;
        n.slurred = (flags & 1) != 0; n.muted = (flags & 2) != 0; n.technique = (vs::Ensemble::Technique) std::clamp (tech, 0, 4);
        n.bendTo = bendTo; n.bendAt = bendAt; n.gain = gain;
        return as<EnsembleIn> (p)->e.schedule (n);
    }
    EMSCRIPTEN_KEEPALIVE void in_ensemble_release (void* p, int id, double at) { as<EnsembleIn> (p)->e.release (id, at); }
    EMSCRIPTEN_KEEPALIVE void in_ensemble_cancel (void* p, double from) { as<EnsembleIn> (p)->e.cancel (from); }
    EMSCRIPTEN_KEEPALIVE void in_ensemble_all_off (void* p) { as<EnsembleIn> (p)->e.allOff(); }
    EMSCRIPTEN_KEEPALIVE void in_ensemble_click (void* p, double at, int accent, float level) { as<EnsembleIn> (p)->e.click (at, accent != 0, level); }
    EMSCRIPTEN_KEEPALIVE int in_ensemble_live_render (void* p, int n, double frame)
    {
        auto* x = as<EnsembleIn> (p);
        if ((int) x->l.size() < n) { x->l.resize ((size_t) n); x->r.resize ((size_t) n); }
        return x->e.renderLive (x->l.data(), x->r.data(), (std::int64_t) frame, n) ? 1 : 0;
    }
}
