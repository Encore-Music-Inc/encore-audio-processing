// The browser's effects (fx_*): the chorus, the phaser, the low/high-pass
// filter (one EQ band), the pitch shifter and time stretch, the compressor set
// by hand, the delay (Space's, the reverb off), the reverb, and the gain and
// send mixes and delay times they use. Plain C functions; audio passes
// through each effect's own left and right buffers (fx_<effect>_left/_right,
// up to maxBlock samples), processed in place.
// Built with the other wasm/api_*.cpp files into master.wasm by wasm/build.sh.
#include <emscripten/emscripten.h>
#include <cstdint>
#include <vector>
#include "dsp/Chorus.h"
#include "dsp/Compressor.h"
#include "dsp/EqBand.h"
#include "dsp/Mix.h"
#include "dsp/Phaser.h"
#include "dsp/ReverbEngine.h"
#include "dsp/Space.h"
#include "dsp/TimeStretch.h"

namespace
{
    struct Io
    {
        explicit Io (int maxBlock) : l ((size_t) maxBlock, 0.0f), r ((size_t) maxBlock, 0.0f) {}
        std::vector<float> l, r;
        float* ch[2] { nullptr, nullptr };
        float* const* chans() { ch[0] = l.data(); ch[1] = r.data(); return ch; }
    };
    struct ChorusFx : Io { using Io::Io; vs::Chorus fx; };
    struct PhaserFx : Io { using Io::Io; vs::Phaser fx; };
    struct FilterFx : Io { using Io::Io; vs::EqBand fx; };
    struct PitchFx : Io { using Io::Io; vs::PitchShift fx; };
    struct CompFx : Io { using Io::Io; vs::Compressor fx; };
    struct DelayFx : Io { using Io::Io; vs::Space fx; };
    struct ReverbFx : Io { using Io::Io; vs::ReverbEngine fx; std::vector<float> wl, wr; vs::Ramp<false> mix; bool mixSet = false; };
    struct StretchFx { vs::TimeStretch fx; std::vector<std::vector<float>> in, out; int channels; };

    template <typename T> T* as (void* p) { return static_cast<T*> (p); }
}

extern "C"
{
    // ---- Chorus
    EMSCRIPTEN_KEEPALIVE void* fx_chorus_create (double fs, int maxBlock) { auto* c = new ChorusFx (maxBlock); c->fx.prepare (fs); return c; }
    EMSCRIPTEN_KEEPALIVE void fx_chorus_destroy (void* p) { delete as<ChorusFx> (p); }
    EMSCRIPTEN_KEEPALIVE float* fx_chorus_left (void* p)  { return as<ChorusFx> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* fx_chorus_right (void* p) { return as<ChorusFx> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE void fx_chorus_settings (void* p, float depthMs, float speedHz, float width, float mix) { as<ChorusFx> (p)->fx.setSettings ({ depthMs, speedHz, width, mix }); }
    EMSCRIPTEN_KEEPALIVE void fx_chorus_reset (void* p) { as<ChorusFx> (p)->fx.reset(); }
    EMSCRIPTEN_KEEPALIVE void fx_chorus_process (void* p, int n, int stereo) { auto* c = as<ChorusFx> (p); c->fx.process (c->chans(), stereo ? 2 : 1, n); }

    // ---- Phaser
    EMSCRIPTEN_KEEPALIVE void* fx_phaser_create (double fs, int maxBlock) { auto* c = new PhaserFx (maxBlock); c->fx.prepare (fs); return c; }
    EMSCRIPTEN_KEEPALIVE void fx_phaser_destroy (void* p) { delete as<PhaserFx> (p); }
    EMSCRIPTEN_KEEPALIVE float* fx_phaser_left (void* p)  { return as<PhaserFx> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* fx_phaser_right (void* p) { return as<PhaserFx> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE void fx_phaser_settings (void* p, float depth, float rate, float feedback) { as<PhaserFx> (p)->fx.setSettings ({ depth, rate, feedback }); }
    EMSCRIPTEN_KEEPALIVE void fx_phaser_reset (void* p) { as<PhaserFx> (p)->fx.reset(); }
    EMSCRIPTEN_KEEPALIVE void fx_phaser_process (void* p, int n, int stereo) { auto* c = as<PhaserFx> (p); c->fx.process (c->chans(), stereo ? 2 : 1, n); }

    // ---- Filter: one EQ band (type: vs::EqBand::Type, 3 low cut = high-pass, 4 high cut = low-pass), gliding to new settings
    EMSCRIPTEN_KEEPALIVE void* fx_filter_create (double fs, int maxBlock) { auto* c = new FilterFx (maxBlock); c->fx.prepare (fs); return c; }
    EMSCRIPTEN_KEEPALIVE void fx_filter_destroy (void* p) { delete as<FilterFx> (p); }
    EMSCRIPTEN_KEEPALIVE float* fx_filter_left (void* p)  { return as<FilterFx> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* fx_filter_right (void* p) { return as<FilterFx> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE void fx_filter_settings (void* p, int type, float hz, float q, float gainDb, int slope)
    {
        vs::EqBand::Settings s; s.on = true; s.type = type; s.freq = hz; s.q = q; s.gainDb = gainDb; s.slope = slope;
        as<FilterFx> (p)->fx.setTarget (s);
    }
    EMSCRIPTEN_KEEPALIVE void fx_filter_process (void* p, int n, int stereo) { auto* c = as<FilterFx> (p); c->fx.process (c->chans(), stereo ? 2 : 1, 0, n); }

    // ---- Pitch shifter (real time; report fx_pitch_latency)
    EMSCRIPTEN_KEEPALIVE void* fx_pitch_create (double fs, int channels, int maxBlock) { auto* c = new PitchFx (maxBlock); c->fx.prepare (fs, channels, maxBlock); return c; }
    EMSCRIPTEN_KEEPALIVE void fx_pitch_destroy (void* p) { delete as<PitchFx> (p); }
    EMSCRIPTEN_KEEPALIVE float* fx_pitch_left (void* p)  { return as<PitchFx> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* fx_pitch_right (void* p) { return as<PitchFx> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE void fx_pitch_semitones (void* p, float semitones) { as<PitchFx> (p)->fx.setSemitones (semitones); }
    EMSCRIPTEN_KEEPALIVE int fx_pitch_latency (void* p) { return as<PitchFx> (p)->fx.latency(); }
    EMSCRIPTEN_KEEPALIVE void fx_pitch_reset (void* p) { as<PitchFx> (p)->fx.reset(); }
    EMSCRIPTEN_KEEPALIVE void fx_pitch_process (void* p, int n, int stereo) { auto* c = as<PitchFx> (p); c->fx.process (c->chans(), stereo ? 2 : 1, n); }

    // ---- Time stretch: fill fx_stretch_input (h, c) with inN samples, then
    // fx_stretch_process (h, inN, outN) and read outN from fx_stretch_output (h, c)
    EMSCRIPTEN_KEEPALIVE void* fx_stretch_create (double fs, int channels, int maxBlock)
    {
        auto* s = new StretchFx;
        s->channels = channels < 1 ? 1 : channels;
        s->fx.prepare (fs, s->channels, maxBlock);
        s->in.assign ((size_t) s->channels, std::vector<float> ((size_t) maxBlock, 0.0f));
        s->out.assign ((size_t) s->channels, std::vector<float> ((size_t) maxBlock, 0.0f));
        return s;
    }
    EMSCRIPTEN_KEEPALIVE void fx_stretch_destroy (void* p) { delete as<StretchFx> (p); }
    EMSCRIPTEN_KEEPALIVE float* fx_stretch_input (void* p, int c)  { return as<StretchFx> (p)->in[(size_t) c].data(); }
    EMSCRIPTEN_KEEPALIVE float* fx_stretch_output (void* p, int c) { return as<StretchFx> (p)->out[(size_t) c].data(); }
    EMSCRIPTEN_KEEPALIVE void fx_stretch_semitones (void* p, float semitones) { as<StretchFx> (p)->fx.setSemitones (semitones); }
    EMSCRIPTEN_KEEPALIVE void fx_stretch_reset (void* p) { as<StretchFx> (p)->fx.reset(); }
    EMSCRIPTEN_KEEPALIVE int fx_stretch_input_latency (void* p) { return as<StretchFx> (p)->fx.inputLatency(); }
    EMSCRIPTEN_KEEPALIVE int fx_stretch_output_latency (void* p) { return as<StretchFx> (p)->fx.outputLatency(); }
    EMSCRIPTEN_KEEPALIVE void fx_stretch_process (void* p, int inN, int outN)
    {
        auto* s = as<StretchFx> (p);
        std::vector<const float*> in; std::vector<float*> out;
        for (auto& c : s->in) in.push_back (c.data());
        for (auto& c : s->out) out.push_back (c.data());
        s->fx.process (in.data(), inN, out.data(), outN);
    }
    EMSCRIPTEN_KEEPALIVE void fx_stretch_flush (void* p, int outN)
    {
        auto* s = as<StretchFx> (p);
        std::vector<float*> out;
        for (auto& c : s->out) out.push_back (c.data());
        s->fx.flush (out.data(), outN);
    }

    // ---- Compressor: the vocal one by amount and style, or set by hand
    EMSCRIPTEN_KEEPALIVE void* fx_comp_create (double fs, int maxBlock) { auto* c = new CompFx (maxBlock); c->fx.prepare (fs); return c; }
    EMSCRIPTEN_KEEPALIVE void fx_comp_destroy (void* p) { delete as<CompFx> (p); }
    EMSCRIPTEN_KEEPALIVE float* fx_comp_left (void* p)  { return as<CompFx> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* fx_comp_right (void* p) { return as<CompFx> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE void fx_comp_amount (void* p, float amount, int style) { as<CompFx> (p)->fx.setParams (amount, style); }
    EMSCRIPTEN_KEEPALIVE void fx_comp_detail (void* p, float thresholdDb, float ratio, float attackMs, float releaseMs, float makeupDb, int rms)
    {
        as<CompFx> (p)->fx.setDetail ({ thresholdDb, ratio, attackMs, releaseMs, makeupDb, rms != 0 });
    }
    EMSCRIPTEN_KEEPALIVE float fx_comp_reduction (void* p) { return as<CompFx> (p)->fx.meterDb.load(); }
    EMSCRIPTEN_KEEPALIVE void fx_comp_process (void* p, int n, int stereo) { auto* c = as<CompFx> (p); c->fx.process (c->chans(), stereo ? 2 : 1, n); }

    // ---- Delay: Space's delay with its reverb off (stereo in place)
    EMSCRIPTEN_KEEPALIVE void* fx_delay_create (double fs, int maxBlock)
    {
        auto* c = new DelayFx (maxBlock);
        c->fx.prepare ({ fs, (std::uint32_t) maxBlock, 2u });
        return c;
    }
    EMSCRIPTEN_KEEPALIVE void fx_delay_destroy (void* p) { delete as<DelayFx> (p); }
    EMSCRIPTEN_KEEPALIVE float* fx_delay_left (void* p)  { return as<DelayFx> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* fx_delay_right (void* p) { return as<DelayFx> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE void fx_delay_settings (void* p, float amount, float seconds, float feedback)
    {
        as<DelayFx> (p)->fx.setParams (0.0f, 0.5f, amount, seconds, feedback);
    }
    EMSCRIPTEN_KEEPALIVE float fx_delay_seconds (int time, double bpm) { return vs::Space::delaySeconds (time, bpm); }
    EMSCRIPTEN_KEEPALIVE void fx_delay_process (void* p, int n)
    {
        auto* c = as<DelayFx> (p);
        c->fx.process (c->chans(), n, true);
    }

    // ---- Reverb (ENCORE.'s reverb engine; the wet signal mixed by the send law)
    EMSCRIPTEN_KEEPALIVE void* fx_reverb_create (double fs, int maxBlock)
    {
        auto* c = new ReverbFx (maxBlock);
        c->fx.prepare (fs, maxBlock);
        c->wl.assign ((size_t) maxBlock, 0.0f); c->wr.assign ((size_t) maxBlock, 0.0f);
        c->mix.reset (fs, 0.05);
        return c;
    }
    EMSCRIPTEN_KEEPALIVE void fx_reverb_destroy (void* p) { delete as<ReverbFx> (p); }
    EMSCRIPTEN_KEEPALIVE float* fx_reverb_left (void* p)  { return as<ReverbFx> (p)->l.data(); }
    EMSCRIPTEN_KEEPALIVE float* fx_reverb_right (void* p) { return as<ReverbFx> (p)->r.data(); }
    EMSCRIPTEN_KEEPALIVE void fx_reverb_settings (void* p, int mode, float predelayMs, float decaySec, float size, float toneHz, float lowCutHz,
                                                   float width, float mod, float duck, float shimmer, float mix)
    {
        auto* c = as<ReverbFx> (p);
        vs::ReverbEngine::Settings s;
        s.mode = mode; s.predelayMs = predelayMs; s.decaySec = decaySec; s.size = size; s.toneHz = toneHz; s.lowCutHz = lowCutHz;
        s.width = width; s.mod = mod; s.duck = duck; s.shimmer = shimmer;
        c->fx.setSettings (s);
        if (c->mixSet) c->mix.setTarget (mix); else c->mix.setCurrentAndTarget (mix);   // the first setting is where it starts
        c->mixSet = true;
    }
    EMSCRIPTEN_KEEPALIVE void fx_reverb_process (void* p, int n, int stereo)
    {
        auto* c = as<ReverbFx> (p);
        c->fx.process (c->l.data(), stereo ? c->r.data() : c->l.data(), c->wl.data(), c->wr.data(), n);
        vs::mix::sendMix (c->mix, c->chans(), stereo ? 2 : 1, c->wl.data(), c->wr.data(), n);
    }

    // ---- The send law (dry and wet gains for a mix amount, into out[0], out[1])
    EMSCRIPTEN_KEEPALIVE void fx_send_law (float amount, float* out) { const auto g = vs::mix::sendLaw (amount); out[0] = g.dry; out[1] = g.wet; }
}
