#pragma once

// DSD (the one-bit audio of SACDs and hi-res stores: .dsf and .dff files) to
// PCM. One bit at 2.8224 MHz (DSD64) and up becomes 88.2 kHz PCM (176.4 kHz
// from DSD256 up) through a long linear-phase low-pass: a windowed sinc
// (four-term Blackman-Harris, windows::blackmanHarrisAt) cut off at 30 kHz,
// which keeps the music and leaves out DSD's ultrasonic noise. It runs a
// byte at a time from lookup tables: one table per byte of taps, holding
// what each of the 256 possible bytes adds.
//
// A DSD bit is +1 or -1, so a full-scale (0 dB SACD) signal comes out near
// -6 dBFS, the usual way, with room for the hottest SACD masters.
//
// The decoder takes each channel's bytes in time order, most significant
// bit first (the earliest), as a DSDIFF file stores them; a DSF file stores
// one-bit samples least significant bit first, so its bytes are reversed
// first (reverseBits). Output sample i is the filter over bytes
// [i * step, i * step + bytes) of the stream, so a stream of n bytes gives
// floor ((n - bytes) / step) + 1 samples (none until there are `bytes`),
// however it is pushed. Plain C++20, no JUCE.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "Windows.h"

namespace vs::dsd
{
    inline constexpr double cutoffHz = 30000.0;

    // The PCM rate a DSD rate becomes: 176.4 kHz from DSD256 (11.2896 MHz) up, else 88.2 kHz
    inline double outputRate (double dsdRate) { return dsdRate >= 11289600.0 ? 176400.0 : 88200.0; }

    // Bytes of DSD per PCM sample, or 0 for a rate that isn't a whole number
    // of bytes per sample of outputRate (one the decoder can't read)
    inline int bytesPerSample (double dsdRate)
    {
        const double step = dsdRate / outputRate (dsdRate) / 8.0;
        return step >= 1.0 && step == std::floor (step) && step < 1.0e6 ? (int) step : 0;
    }

    // A byte with its bits in the opposite order (a DSF file's bytes to time order)
    inline std::uint8_t reverseBits (std::uint8_t v)
    {
        std::uint8_t r = 0;
        for (int b = 0; b < 8; ++b) r = (std::uint8_t) (r | (((v >> b) & 1) << (7 - b)));
        return r;
    }

    // The low-pass as tables: bytes tables of 256, entry [k * 256 + v] what
    // byte v adds at the filter's k-th byte (bit b of the byte, from the
    // most significant, is tap k * 8 + b, +1 for a 1 and -1 for a 0). The
    // filter has 32 taps per PCM sample's worth of DSD bits, rounded up to a
    // whole byte, and its taps sum to 1.
    struct Filter
    {
        int bytes = 0;
        std::vector<float> table;
    };

    inline Filter makeFilter (double dsdRate)
    {
        const double decim = dsdRate / outputRate (dsdRate);
        const int taps = (int) std::ceil ((decim * 32) / 8) * 8;
        std::vector<double> h ((size_t) taps);
        const double fc = cutoffHz / dsdRate, mid = (taps - 1) / 2.0;
        double sum = 0.0;
        for (int i = 0; i < taps; ++i)
        {
            const double x = i - mid;
            const double sinc = x == 0 ? 2 * fc : std::sin (2 * windows::pi * fc * x) / (windows::pi * x);
            h[(size_t) i] = sinc * windows::blackmanHarrisAt (i, taps);
            sum += h[(size_t) i];
        }
        Filter f;
        f.bytes = taps / 8;
        f.table.resize ((size_t) f.bytes * 256);
        for (int k = 0; k < f.bytes; ++k)
            for (int v = 0; v < 256; ++v)
            {
                double acc = 0.0;
                for (int b = 0; b < 8; ++b) acc += (h[(size_t) (k * 8 + b)] / sum) * (((v >> (7 - b)) & 1) ? 1 : -1);
                f.table[(size_t) (k * 256 + v)] = (float) acc;
            }
        return f;
    }

    // Streaming DSD to PCM for one or more channels. push() any number of
    // bytes per channel at a time (the same number for every channel); the
    // PCM it makes is in output (c) until the next push.
    class Decoder
    {
    public:
        Decoder (double dsdRate, int channels)
            : step (bytesPerSample (dsdRate)), fsOut (outputRate (dsdRate)),
              pending ((size_t) std::max (0, channels)), out ((size_t) std::max (0, channels))
        {
            if (step > 0) filter = makeFilter (dsdRate);
        }

        // false for a DSD rate it can't read (bytesPerSample is 0)
        bool ok() const { return step > 0; }
        double outputSampleRate() const { return fsOut; }
        int channels() const { return (int) pending.size(); }
        // Bytes per output sample, and the filter's length in bytes
        int bytesPerOutput() const { return step; }
        int filterBytes() const { return filter.bytes; }

        // in: one pointer per channel to n bytes each. Returns how many PCM
        // samples each channel made.
        int push (const std::uint8_t* const* in, int n)
        {
            int made = 0;
            for (size_t c = 0; c < pending.size(); ++c)
            {
                auto& p = pending[c];
                auto& o = out[c];
                o.clear();
                if (! ok()) continue;
                p.insert (p.end(), in[c], in[c] + std::max (0, n));
                size_t pos = 0;
                for (; pos + (size_t) filter.bytes <= p.size(); pos += (size_t) step)
                {
                    double acc = 0.0;
                    for (int k = 0; k < filter.bytes; ++k) acc += filter.table[((size_t) k << 8) | p[pos + (size_t) k]];
                    o.push_back ((float) acc);
                }
                p.erase (p.begin(), p.begin() + (std::ptrdiff_t) std::min (pos, p.size()));
                made = (int) o.size();
            }
            return made;
        }

        const float* output (int c) const { return out[(size_t) c].data(); }

        // Forget everything pushed so far (a new stream)
        void reset()
        {
            for (auto& p : pending) p.clear();
            for (auto& o : out) o.clear();
        }

    private:
        int step;
        double fsOut;
        Filter filter;
        std::vector<std::vector<std::uint8_t>> pending;
        std::vector<std::vector<float>> out;
    };

    // A whole channel at once: bits, n bytes in time order (most significant
    // bit first); empty for a rate the decoder can't read
    inline std::vector<float> toPcm (const std::uint8_t* bits, int n, double dsdRate)
    {
        Decoder d (dsdRate, 1);
        const std::uint8_t* in[1] { bits };
        const int made = d.push (in, n);
        return std::vector<float> (d.output (0), d.output (0) + made);
    }
}
