// The browser's instruments (in_*): the synth and the sampler. Plain C
// functions; notes go in as calls (in_synth_note_on (h, note, velocity)) at
// the block they start in, and in_<instrument>_render (h, n) writes n samples
// into its own left and right buffers (up to maxBlock).
// Built with the other wasm/api_*.cpp files into master.wasm by wasm/build.sh.
#include <emscripten/emscripten.h>
#include <algorithm>
#include <vector>
#include "dsp/Sampler.h"
#include "dsp/Synth.h"

namespace
{
    struct SynthIn { vs::Synth s; std::vector<float> l, r; };
    struct SamplerIn { vs::Sampler s; std::vector<float> l, r, loadL, loadR; };
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
}
