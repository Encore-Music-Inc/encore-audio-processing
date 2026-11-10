// The browser's processing pieces (pr_*): sample rate conversion, fractional
// delays, decimation, float to integer with dither, channel maps, trims,
// fades and normalizing, the playback device filters, the vocal EQ and the
// loudness-matched "hear the original". Plain C functions; audio passes
// through float buffers in the module's memory. Where a function takes several
// channels at once (chans), it is an array of that many float pointers.
// Built with the other wasm/api_*.cpp files into master.wasm by wasm/build.sh.
#include <emscripten/emscripten.h>
#include <cstdint>
#include <vector>
#include "dsp/AbCompare.h"
#include "dsp/Channels.h"
#include "dsp/DeviceSim.h"
#include "dsp/Dither.h"
#include "dsp/FracDelay.h"
#include "dsp/Resampler.h"
#include "dsp/VocalEq.h"

namespace
{
    using Buffers = std::vector<std::vector<float>>;
    Buffers buffers (int channels, int n) { return Buffers ((size_t) channels, std::vector<float> ((size_t) n, 0.0f)); }

    struct Resample { vs::Resampler r; Buffers in; };
    struct Delay { vs::FracDelay d; Buffers in; };
    struct Decimate { vs::Decimator d; std::vector<float> in, out; };
    struct Quantize { vs::Quantizer q; int channels; Buffers in; std::vector<std::vector<std::int32_t>> ints; std::vector<std::uint8_t> bytes; };
    struct Device { vs::DeviceSim sim; Buffers io; };
    struct Vocal { vs::VocalEq eq; Buffers io; };
    struct Ab { vs::AbCompare ab; Buffers original, processed; };

    std::vector<const float*> pointers (const Buffers& b)
    {
        std::vector<const float*> p;
        for (auto& c : b) p.push_back (c.data());
        return p;
    }
}

extern "C"
{
    // ---- Resampler: fsIn to fsOut, streaming. Fill pr_resampler_input (h, c)
    // for each channel, pr_resampler_push (h, n), read pr_resampler_output (h, c)
    // (as many samples as it returned); pr_resampler_flush after the last block.
    EMSCRIPTEN_KEEPALIVE void* pr_resampler_create (double fsIn, double fsOut, int channels, int maxBlock)
    {
        return new Resample { vs::Resampler (fsIn / fsOut, channels), buffers (channels, maxBlock) };
    }
    EMSCRIPTEN_KEEPALIVE void pr_resampler_destroy (void* p) { delete static_cast<Resample*> (p); }
    EMSCRIPTEN_KEEPALIVE float* pr_resampler_input (void* p, int c) { return static_cast<Resample*> (p)->in[(size_t) c].data(); }
    EMSCRIPTEN_KEEPALIVE int pr_resampler_push (void* p, int n)
    {
        auto* r = static_cast<Resample*> (p);
        const auto in = pointers (r->in);
        return r->r.push (in.data(), n);
    }
    EMSCRIPTEN_KEEPALIVE int pr_resampler_flush (void* p) { return static_cast<Resample*> (p)->r.flush(); }
    EMSCRIPTEN_KEEPALIVE const float* pr_resampler_output (void* p, int c) { return static_cast<Resample*> (p)->r.output (c); }

    // Samples a whole recording of n samples becomes at the new rate
    EMSCRIPTEN_KEEPALIVE int pr_resampled_length (int n, double fsIn, double fsOut) { return vs::resampledLength (n, fsIn, fsOut); }

    // ---- Fractional delay per channel (delays: that many doubles, in samples)
    EMSCRIPTEN_KEEPALIVE void* pr_fracdelay_create (const double* delays, int channels, int maxBlock)
    {
        return new Delay { vs::FracDelay (std::vector<double> (delays, delays + channels)), buffers (channels, maxBlock) };
    }
    EMSCRIPTEN_KEEPALIVE void pr_fracdelay_destroy (void* p) { delete static_cast<Delay*> (p); }
    EMSCRIPTEN_KEEPALIVE int pr_fracdelay_latency (void* p) { return static_cast<Delay*> (p)->d.latency(); }
    EMSCRIPTEN_KEEPALIVE float* pr_fracdelay_buffer (void* p, int c) { return static_cast<Delay*> (p)->in[(size_t) c].data(); }
    // Delays n samples of channel c's buffer in place
    EMSCRIPTEN_KEEPALIVE void pr_fracdelay_process (void* p, int c, int n)
    {
        auto* d = static_cast<Delay*> (p);
        d->d.process (c, d->in[(size_t) c].data(), n);
    }

    // ---- Decimator (8th-order Butterworth at 0.42 fs / factor), one channel
    EMSCRIPTEN_KEEPALIVE void* pr_decimator_create (double fs, int factor, int maxBlock)
    {
        auto* d = new Decimate { vs::Decimator (fs, factor), std::vector<float> ((size_t) maxBlock), {} };
        d->out.resize ((size_t) d->d.maxOutput (maxBlock));
        return d;
    }
    EMSCRIPTEN_KEEPALIVE void pr_decimator_destroy (void* p) { delete static_cast<Decimate*> (p); }
    EMSCRIPTEN_KEEPALIVE float* pr_decimator_input (void* p) { return static_cast<Decimate*> (p)->in.data(); }
    EMSCRIPTEN_KEEPALIVE const float* pr_decimator_output (void* p) { return static_cast<Decimate*> (p)->out.data(); }
    EMSCRIPTEN_KEEPALIVE int pr_decimator_push (void* p, int n)
    {
        auto* d = static_cast<Decimate*> (p);
        return d->d.push (d->in.data(), n, d->out.data());
    }

    // The low-band analysis copy (two 1 kHz low-passes, every d-th sample): out holds n / d + 1
    EMSCRIPTEN_KEEPALIVE int pr_decimate_low_band (const float* x, int n, double fs, int d, float* out)
    {
        return vs::decimateLowBand (x, n, fs, d, out);
    }

    // ---- Float to integers. mode: 0 dither only off the integer grid, 1 always, 2 never.
    // Fill pr_quantizer_input (h, c), then pr_quantizer_process (h, n) for
    // pr_quantizer_ints (h, c), or pr_quantizer_pcm (h, n) for interleaved
    // little-endian bytes at pr_quantizer_bytes (h).
    EMSCRIPTEN_KEEPALIVE void* pr_quantizer_create (int bits, int mode, int seed, int channels, int maxBlock)
    {
        const auto m = mode == 1 ? vs::Quantizer::Dither::always : mode == 2 ? vs::Quantizer::Dither::never : vs::Quantizer::Dither::automatic;
        auto* q = new Quantize { vs::Quantizer (bits, m, (std::uint32_t) seed), channels, buffers (channels, maxBlock), {}, {} };
        q->ints.assign ((size_t) channels, std::vector<std::int32_t> ((size_t) maxBlock, 0));
        q->bytes.assign ((size_t) maxBlock * (size_t) channels * (size_t) (bits / 8), 0);
        return q;
    }
    EMSCRIPTEN_KEEPALIVE void pr_quantizer_destroy (void* p) { delete static_cast<Quantize*> (p); }
    EMSCRIPTEN_KEEPALIVE float* pr_quantizer_input (void* p, int c) { return static_cast<Quantize*> (p)->in[(size_t) c].data(); }
    EMSCRIPTEN_KEEPALIVE const std::int32_t* pr_quantizer_ints (void* p, int c) { return static_cast<Quantize*> (p)->ints[(size_t) c].data(); }
    EMSCRIPTEN_KEEPALIVE const std::uint8_t* pr_quantizer_bytes (void* p) { return static_cast<Quantize*> (p)->bytes.data(); }
    EMSCRIPTEN_KEEPALIVE void pr_quantizer_process (void* p, int n)
    {
        auto* q = static_cast<Quantize*> (p);
        const auto in = pointers (q->in);
        std::vector<std::int32_t*> out;
        for (auto& c : q->ints) out.push_back (c.data());
        q->q.process (in.data(), q->channels, n, out.data());
    }
    EMSCRIPTEN_KEEPALIVE void pr_quantizer_pcm (void* p, int n)
    {
        auto* q = static_cast<Quantize*> (p);
        const auto in = pointers (q->in);
        q->q.processToPcm (in.data(), q->channels, n, q->bytes.data());
    }
    EMSCRIPTEN_KEEPALIVE int pr_quantizer_dithering (void* p) { return static_cast<Quantize*> (p)->q.isDithering() ? 1 : 0; }

    EMSCRIPTEN_KEEPALIVE int pr_on_grid (const float* const* chans, int numCh, int n, int bits)
    {
        return vs::Quantizer::onGrid (chans, numCh, n, bits) ? 1 : 0;
    }

    // ---- Channels. map: 0 keep, 1 left, 2 right, 3 mono, 4 stereo
    EMSCRIPTEN_KEEPALIVE int pr_mapped_channels (int map, int numIn) { return vs::channels::mappedChannels ((vs::channels::Map) map, numIn); }
    EMSCRIPTEN_KEEPALIVE void pr_map_channels (int map, const float* const* in, int numIn, int n, float* const* out)
    {
        vs::channels::map ((vs::channels::Map) map, in, numIn, n, out);
    }
    // out: two ints, the first sample kept and the one after the last
    EMSCRIPTEN_KEEPALIVE void pr_trim_range (int n, double fs, double startSec, double endSec, int* out)
    {
        const auto r = vs::channels::trimRange (n, fs, startSec, endSec);
        out[0] = r.first; out[1] = r.last;
    }
    EMSCRIPTEN_KEEPALIVE void pr_fades (float* const* chans, int numCh, int n, double fs, double fadeInSec, double fadeOutSec)
    {
        vs::channels::fades (chans, numCh, n, fs, fadeInSec, fadeOutSec);
    }
    EMSCRIPTEN_KEEPALIVE float pr_peak (const float* const* chans, int numCh, int n) { return vs::channels::peakOf (chans, numCh, n); }
    // mode: 0 off, 1 peak (-1 dBFS), 2 LUFS (-14, capped at -1 dBFS peak). out: four doubles,
    // gain dB, outcome (0 unchanged, 1 applied, 2 silent, 3 too quiet, 4 stopped at the peak), measured LUFS, LUFS reached when stopped
    EMSCRIPTEN_KEEPALIVE void pr_normalize (float* const* chans, int numCh, int n, double fs, int mode, double* out)
    {
        const auto r = vs::channels::normalize (chans, numCh, n, fs, (vs::channels::Normalize) mode);
        out[0] = r.gainDb; out[1] = (double) r.outcome; out[2] = r.lufs; out[3] = r.reachedLufs;
    }

    // ---- Playback devices ("listen on"): 0 studio monitors, 1 phone, 2 laptop, 3 earbuds, 4 car
    EMSCRIPTEN_KEEPALIVE int pr_device_count() { return (int) vs::device::devices().size(); }
    EMSCRIPTEN_KEEPALIVE const char* pr_device_id (int i) { return vs::device::devices()[(size_t) i].id; }
    EMSCRIPTEN_KEEPALIVE const char* pr_device_name (int i) { return vs::device::devices()[(size_t) i].name; }
    EMSCRIPTEN_KEEPALIVE void* pr_device_create (double fs, int device, int maxBlock)
    {
        auto* d = new Device { {}, buffers (2, maxBlock) };
        d->sim.prepare (fs);
        d->sim.setDevice (device);
        return d;
    }
    EMSCRIPTEN_KEEPALIVE void pr_device_destroy (void* p) { delete static_cast<Device*> (p); }
    EMSCRIPTEN_KEEPALIVE void pr_device_set (void* p, int device) { static_cast<Device*> (p)->sim.setDevice (device); }
    EMSCRIPTEN_KEEPALIVE float* pr_device_left (void* p)  { return static_cast<Device*> (p)->io[0].data(); }
    EMSCRIPTEN_KEEPALIVE float* pr_device_right (void* p) { return static_cast<Device*> (p)->io[1].data(); }
    // stereo 0: only the left buffer (one channel)
    EMSCRIPTEN_KEEPALIVE void pr_device_process (void* p, int n, int stereo)
    {
        auto* d = static_cast<Device*> (p);
        d->sim.process (d->io[0].data(), stereo != 0 ? d->io[1].data() : nullptr, n);
    }
    EMSCRIPTEN_KEEPALIVE double pr_device_magnitude (void* p, double freq) { return static_cast<Device*> (p)->sim.magnitudeDb (freq); }

    // ---- Vocal EQ: low cut and four tone bands
    EMSCRIPTEN_KEEPALIVE void* pr_vocal_eq_create (double fs, int maxBlock)
    {
        auto* v = new Vocal { {}, buffers (2, maxBlock) };
        v->eq.prepare (fs);
        return v;
    }
    EMSCRIPTEN_KEEPALIVE void pr_vocal_eq_destroy (void* p) { delete static_cast<Vocal*> (p); }
    EMSCRIPTEN_KEEPALIVE void pr_vocal_eq_settings (void* p, float lowCutHz, float lowDb, float bodyDb, float presenceDb, float airDb)
    {
        vs::VocalEq::Settings s;
        s.lowCutHz = lowCutHz; s.lowDb = lowDb; s.bodyDb = bodyDb; s.presenceDb = presenceDb; s.airDb = airDb;
        static_cast<Vocal*> (p)->eq.setSettings (s);
    }
    EMSCRIPTEN_KEEPALIVE float* pr_vocal_eq_left (void* p)  { return static_cast<Vocal*> (p)->io[0].data(); }
    EMSCRIPTEN_KEEPALIVE float* pr_vocal_eq_right (void* p) { return static_cast<Vocal*> (p)->io[1].data(); }
    EMSCRIPTEN_KEEPALIVE void pr_vocal_eq_process (void* p, int n, int stereo)
    {
        auto* v = static_cast<Vocal*> (p);
        float* ch[2] { v->io[0].data(), v->io[1].data() };
        v->eq.process (ch, stereo != 0 ? 2 : 1, n);
    }
    EMSCRIPTEN_KEEPALIVE double pr_vocal_eq_magnitude (void* p, double freq) { return static_cast<Vocal*> (p)->eq.magnitudeDb (freq); }

    // ---- Loudness-matched "hear the original". Per block: the input in
    // pr_ab_original_left/right, pr_ab_take_original (h, n); process it; the
    // processed audio in pr_ab_left/right, pr_ab_mix (h, n): what to play is
    // then in pr_ab_left/right.
    EMSCRIPTEN_KEEPALIVE void* pr_ab_create (double fs, int maxBlock, int latency)
    {
        auto* a = new Ab { {}, buffers (2, maxBlock), buffers (2, maxBlock) };
        a->ab.prepare (fs, maxBlock, latency);
        return a;
    }
    EMSCRIPTEN_KEEPALIVE void pr_ab_destroy (void* p) { delete static_cast<Ab*> (p); }
    EMSCRIPTEN_KEEPALIVE float* pr_ab_original_left (void* p)  { return static_cast<Ab*> (p)->original[0].data(); }
    EMSCRIPTEN_KEEPALIVE float* pr_ab_original_right (void* p) { return static_cast<Ab*> (p)->original[1].data(); }
    EMSCRIPTEN_KEEPALIVE float* pr_ab_left (void* p)  { return static_cast<Ab*> (p)->processed[0].data(); }
    EMSCRIPTEN_KEEPALIVE float* pr_ab_right (void* p) { return static_cast<Ab*> (p)->processed[1].data(); }
    EMSCRIPTEN_KEEPALIVE void pr_ab_hear_original (void* p, int on) { static_cast<Ab*> (p)->ab.setHearOriginal (on != 0); }
    EMSCRIPTEN_KEEPALIVE void pr_ab_take_original (void* p, int n)
    {
        auto* a = static_cast<Ab*> (p);
        a->ab.takeOriginal (a->original[0].data(), a->original[1].data(), n);
    }
    EMSCRIPTEN_KEEPALIVE void pr_ab_mix (void* p, int n)
    {
        auto* a = static_cast<Ab*> (p);
        a->ab.mix (a->processed[0].data(), a->processed[1].data(), n);
    }
    EMSCRIPTEN_KEEPALIVE float pr_ab_match_db (void* p) { return static_cast<Ab*> (p)->ab.matchGainDb(); }
}
