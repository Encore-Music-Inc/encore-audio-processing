// The browser's sounds (sd_*): plain C functions around vs::drumsynth (every
// drum of every kit) and vs::metronome (every click sound, and the click
// engine an AudioWorklet runs). Built with the other wasm/api_*.cpp files
// into master.wasm by wasm/build.sh.
//
// A rendered sound comes back as memory from malloc (free it with free);
// its length is written to *outLength. Strings are owned by the engine.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <cstring>
#include <memory>
#include <string>
#include <emscripten/emscripten.h>
#include "dsp/DrumSynth.h"
#include "dsp/Metronome.h"

namespace
{
    float* copyOut (const std::vector<float>& x, int* outLength)
    {
        *outLength = (int) x.size();
        auto* p = (float*) std::malloc (std::max<size_t> (1, x.size()) * sizeof (float));
        if (p != nullptr && ! x.empty()) std::memcpy (p, x.data(), x.size() * sizeof (float));
        return p;
    }

    const std::vector<float>& role (const vs::metronome::Sounds& s, int r)
    {
        switch (r) { case 0: return s.accent; case 1: return s.beat; case 2: return s.sub; case 3: return s.poly; default: return s.alt; }
    }

    std::vector<float>& role (vs::metronome::Sounds& s, int r)
    {
        return const_cast<std::vector<float>&> (role (static_cast<const vs::metronome::Sounds&> (s), r));
    }

    const vs::drumsynth::Kit* kitAt (int i)
    {
        const auto& k = vs::drumsynth::kits();
        return i >= 0 && i < (int) k.size() ? &k[(size_t) i] : nullptr;
    }

    std::string text;   // the last name handed out
}

extern "C"
{
    // ---- drums

    // How many drum sounds there are, and each one's id and list name
    EMSCRIPTEN_KEEPALIVE int sd_drum_count() { return (int) vs::drumsynth::sounds().size(); }

    EMSCRIPTEN_KEEPALIVE const char* sd_drum_id (int i)
    {
        const auto& s = vs::drumsynth::sounds();
        return i >= 0 && i < (int) s.size() ? s[(size_t) i].id : "";
    }

    EMSCRIPTEN_KEEPALIVE const char* sd_drum_name (int i)
    {
        text = vs::drumsynth::soundName (sd_drum_id (i));
        return text.c_str();
    }

    // One drum sound (by id) at sampleRate; length 0 for an id it doesn't know
    EMSCRIPTEN_KEEPALIVE float* sd_drum_render (const char* id, double sampleRate, int* outLength)
    {
        return copyOut (vs::drumsynth::renderSound (id, sampleRate), outLength);
    }

    // The kits: how many, each one's id, name and description
    EMSCRIPTEN_KEEPALIVE int sd_kit_count() { return (int) vs::drumsynth::kits().size(); }
    EMSCRIPTEN_KEEPALIVE const char* sd_kit_id (int kit) { auto* k = kitAt (kit); return k ? k->id : ""; }
    EMSCRIPTEN_KEEPALIVE const char* sd_kit_name (int kit) { auto* k = kitAt (kit); return k ? k->name : ""; }
    EMSCRIPTEN_KEEPALIVE const char* sd_kit_about (int kit) { auto* k = kitAt (kit); return k ? k->about : ""; }

    // A kit's drums: how many, and each one's sound id; out: 3 ints, the pad
    // slot (0 for none), the General MIDI note and the choke group
    EMSCRIPTEN_KEEPALIVE int sd_kit_pad_count (int kit) { auto* k = kitAt (kit); return k ? (int) k->pads.size() : 0; }

    EMSCRIPTEN_KEEPALIVE const char* sd_kit_pad (int kit, int pad, int* out)
    {
        auto* k = kitAt (kit);
        if (k == nullptr || pad < 0 || pad >= (int) k->pads.size()) return "";
        const auto& p = k->pads[(size_t) pad];
        out[0] = p.slot; out[1] = p.note; out[2] = p.choke;
        return p.sound;
    }

    // The sound id a kit plays for a General MIDI note ("" for none)
    EMSCRIPTEN_KEEPALIVE const char* sd_kit_sound_for_note (int kit, int note)
    {
        auto* k = kitAt (kit);
        const char* s = k ? vs::drumsynth::kitSound (*k, note) : nullptr;
        return s ? s : "";
    }

    // ---- click sounds

    // The sound sets: how many, each one's id and name
    EMSCRIPTEN_KEEPALIVE int sd_click_set_count() { return (int) std::size (vs::metronome::soundSets); }

    EMSCRIPTEN_KEEPALIVE const char* sd_click_set_id (int i)
    {
        return i >= 0 && i < sd_click_set_count() ? vs::metronome::soundSets[i].id : "";
    }

    EMSCRIPTEN_KEEPALIVE const char* sd_click_set_name (int i)
    {
        return i >= 0 && i < sd_click_set_count() ? vs::metronome::soundSets[i].name : "";
    }

    // One sound of a set at sampleRate. role: 0 accent, 1 beat, 2 sub, 3 poly,
    // 4 alt (length 0 for a set without one)
    EMSCRIPTEN_KEEPALIVE float* sd_click_render (const char* setId, int soundRole, double sampleRate, int* outLength)
    {
        return copyOut (role (vs::metronome::makeSounds (setId, sampleRate), soundRole), outLength);
    }

    // ---- the click engine, for an AudioWorklet

    struct ClickEngineHandle
    {
        vs::metronome::ClickEngine engine;
        vs::metronome::Sounds sounds;
    };

    EMSCRIPTEN_KEEPALIVE ClickEngineHandle* sd_engine_create (double sampleRate)
    {
        return new ClickEngineHandle { vs::metronome::ClickEngine (sampleRate), {} };
    }

    EMSCRIPTEN_KEEPALIVE void sd_engine_destroy (ClickEngineHandle* h) { delete h; }

    // Plays a sound set, made at the engine's sample rate
    EMSCRIPTEN_KEEPALIVE void sd_engine_set_sounds (ClickEngineHandle* h, const char* setId)
    {
        h->sounds = vs::metronome::makeSounds (setId, h->engine.sr);
        h->engine.setSounds (h->sounds);
    }

    // Replaces one sound (role as sd_click_render) with x[0..n): any buffer,
    // such as another set's accent
    EMSCRIPTEN_KEEPALIVE void sd_engine_set_sound (ClickEngineHandle* h, int soundRole, const float* x, int n)
    {
        role (h->sounds, soundRole).assign (x, x + std::max (0, n));
        h->engine.setSounds (h->sounds);
    }

    // The sequencer's settings and the volume sliders. s: 22 numbers, in
    // order: bpm, top, bottom, units, swing, randomMute, poly on, poly n,
    // gap on, gap play, gap mute, trainer mode (0 off, 1 steps, 2 ramp),
    // trainer from, step, every, to, loop, minutes, then the accent, beat,
    // sub and poly volumes (0 to 100). groups and accents: nGroups and
    // nAccents ints (0 to leave them out, as a settings object without them:
    // the default 4/4 values, which cleanSettings replaces when they don't
    // fit the meter). sub: the subdivision id.
    EMSCRIPTEN_KEEPALIVE void sd_engine_set (ClickEngineHandle* h, const double* s, const int* groups, int nGroups, const int* accents, int nAccents, const char* sub)
    {
        using namespace vs::metronome;
        auto i = [] (double v) { return std::isnan (v) ? 0 : (int) roundHalfUp (v); };
        MetronomeSettings m;
        m.bpm = s[0]; m.top = i (s[1]); m.bottom = i (s[2]); m.units = i (s[3]);
        m.swing = s[4]; m.randomMute = s[5];
        m.poly = { s[6] != 0, i (s[7]) };
        m.gap = { s[8] != 0, i (s[9]), i (s[10]) };
        m.trainer.mode = s[11] == 1 ? TrainerMode::steps : s[11] == 2 ? TrainerMode::ramp : TrainerMode::off;
        m.trainer.from = s[12]; m.trainer.step = s[13]; m.trainer.every = s[14]; m.trainer.to = s[15];
        m.trainer.loop = s[16] != 0; m.trainer.minutes = s[17];
        if (nGroups > 0) m.groups.assign (groups, groups + nGroups);
        if (nAccents > 0) m.accents.assign (accents, accents + nAccents);
        m.sub = sub;
        h->engine.set (m, { s[18], s[19], s[20], s[21] });
    }

    // Starts at frame (the first count there, after a count-in bar when
    // countIn); seed: the random muting's
    EMSCRIPTEN_KEEPALIVE void sd_engine_start (ClickEngineHandle* h, double frame, int countIn, unsigned seed)
    {
        h->engine.start (frame, { countIn != 0, seed });
    }

    EMSCRIPTEN_KEEPALIVE void sd_engine_stop (ClickEngineHandle* h) { h->engine.stop(); }

    // Mixes frames [frame0, frame0 + n) into out (from malloc); returns how
    // many counts start in this block (read them with sd_engine_event)
    EMSCRIPTEN_KEEPALIVE int sd_engine_process (ClickEngineHandle* h, float* out, int n, double frame0)
    {
        return (int) h->engine.render (out, n, (std::int64_t) frame0).size();
    }

    // Count i of the last block. out: 16 numbers, in order: frame, len, bar,
    // count, top, group, groupStart, level, countIn, gap, muted, bpm, units,
    // swing, polyN, the number of clicks inside the count (subs)
    EMSCRIPTEN_KEEPALIVE void sd_engine_event (ClickEngineHandle* h, int i, double* out)
    {
        const auto& e = h->engine.lastEvents()[(size_t) i];
        const double v[] { e.frame, e.len, (double) e.bar, (double) e.count, (double) e.top, (double) e.group, (double) e.groupStart, (double) e.level,
                           (double) e.countIn, (double) e.gap, (double) e.muted, e.bpm, (double) e.units, e.swing, (double) e.polyN, (double) e.subs.size() };
        std::memcpy (out, v, sizeof (v));
    }
}
