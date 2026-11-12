#pragma once

// Time stretching and pitch shifting: Signalsmith Stretch (MIT, vendored in
// dsp/third_party/signalsmith, see THIRD-PARTY-NOTICES.md), a phase-vocoder
// stretcher, with every transform it makes running on this repository's one
// FFT (vs::Fft): its complex FFT is replaced below with one on vs::Fft, at
// power-of-two sizes only, so none of its own FFT code runs.
//
//   TimeStretch   streaming: n samples in, m samples out (m / n is the speed),
//                 the pitch moved by any number of semitones
//   PitchShift    a real-time effect: the same number of samples out as in,
//                 the pitch moved, delayed by latency()

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>
#include "Fft.h"
#include "third_party/signalsmith/signalsmith-stretch.h"

namespace signalsmith { namespace linear {

    // The complex FFT the stretcher's real FFT sits on, on vs::Fft: power-of-two
    // sizes, unscaled both ways (as the library's own)
    template <typename Sample, bool splitComputation>
    struct EncoreSplitFFT
    {
        using Complex = std::complex<Sample>;
        static constexpr bool prefersSplit = true;

        static size_t fastSizeAbove (size_t size)
        {
            size_t p = 1;
            while (p < size) p *= 2;
            return p;
        }

        EncoreSplitFFT (size_t size = 0) { resize (size); }

        void resize (size_t size)
        {
            n = size;
            if (n > 0) fft_.setSize ((int) n);
            re.assign (n, 0.0);
            im.assign (n, 0.0);
        }

        size_t size() const { return n; }
        size_t steps() const { return 1; }

        void fft (const Complex* time, Complex* freq) { load (time); fft_.forward (re.data(), im.data()); store (freq, 1.0); }
        void fft (size_t, const Complex* time, Complex* freq) { fft (time, freq); }
        void fft (const Sample* inR, const Sample* inI, Sample* outR, Sample* outI) { load (inR, inI); fft_.forward (re.data(), im.data()); store (outR, outI, 1.0); }
        void fft (size_t, const Sample* inR, const Sample* inI, Sample* outR, Sample* outI) { fft (inR, inI, outR, outI); }

        void ifft (const Complex* freq, Complex* time) { load (freq); fft_.inverse (re.data(), im.data()); store (time, (double) n); }
        void ifft (size_t, const Complex* freq, Complex* time) { ifft (freq, time); }
        void ifft (const Sample* inR, const Sample* inI, Sample* outR, Sample* outI) { load (inR, inI); fft_.inverse (re.data(), im.data()); store (outR, outI, (double) n); }
        void ifft (size_t, const Sample* inR, const Sample* inI, Sample* outR, Sample* outI) { ifft (inR, inI, outR, outI); }

    private:
        void load (const Complex* x) { for (size_t i = 0; i < n; ++i) { re[i] = (double) x[i].real(); im[i] = (double) x[i].imag(); } }
        void load (const Sample* r, const Sample* i_) { for (size_t i = 0; i < n; ++i) { re[i] = (double) r[i]; im[i] = (double) i_[i]; } }
        void store (Complex* x, double scale) { for (size_t i = 0; i < n; ++i) x[i] = Complex ((Sample) (re[i] * scale), (Sample) (im[i] * scale)); }
        void store (Sample* r, Sample* i_, double scale) { for (size_t i = 0; i < n; ++i) { r[i] = (Sample) (re[i] * scale); i_[i] = (Sample) (im[i] * scale); } }

        size_t n = 0;
        vs::Fft fft_;
        std::vector<double> re, im;
    };

    template <> struct SplitFFT<float, false>  : EncoreSplitFFT<float, false>  { using EncoreSplitFFT::EncoreSplitFFT; };
    template <> struct SplitFFT<double, false> : EncoreSplitFFT<double, false> { using EncoreSplitFFT::EncoreSplitFFT; };
}}

namespace vs
{
    class TimeStretch
    {
    public:
        // Allocates; call it off the audio thread. maxBlock: the most samples
        // in or out of one process() call.
        void prepare (double sampleRate, int numChannels, int maxBlock)
        {
            channels = std::clamp (numChannels, 1, 8);
            stretch.presetDefault (channels, (float) sampleRate);
            in.assign ((size_t) channels, nullptr);
            out.assign ((size_t) channels, nullptr);
            scratch.assign ((size_t) channels, std::vector<float> ((size_t) std::max (1, maxBlock), 0.0f));
            reset();
        }

        void reset() { stretch.reset(); }

        // The pitch, in semitones up (negative down); formants move with it
        void setSemitones (float semitones) { stretch.setTransposeSemitones (semitones); }

        // inN samples of each channel in, outN samples of each channel out:
        // outN / inN is the speed (2: half as long). in may be null for silence.
        void process (const float* const* inCh, int inN, float* const* outCh, int outN)
        {
            for (int c = 0; c < channels; ++c)
            {
                in[(size_t) c] = inCh != nullptr ? inCh[c] : silence (c, inN);
                out[(size_t) c] = outCh[c];
            }
            stretch.process (in.data(), inN, out.data(), outN);
        }

        // At the end of a stream: the rest of what's been put in, outN samples
        void flush (float* const* outCh, int outN)
        {
            for (int c = 0; c < channels; ++c) out[(size_t) c] = outCh[c];
            stretch.flush (out.data(), outN);
        }

        // How far behind the input the output is, in input and output samples
        int inputLatency() const { return stretch.inputLatency(); }
        int outputLatency() const { return stretch.outputLatency(); }

    private:
        const float* silence (int c, int n)
        {
            auto& s = scratch[(size_t) c];
            if ((int) s.size() < n) s.resize ((size_t) n);
            std::fill (s.begin(), s.begin() + n, 0.0f);
            return s.data();
        }

        int channels = 1;
        signalsmith::stretch::SignalsmithStretch<float> stretch { 0x454e43L };   // a fixed seed: the same sound every run (it only draws numbers past 2x stretch)
        std::vector<const float*> in;
        std::vector<float*> out;
        std::vector<std::vector<float>> scratch;
    };

    // A real-time pitch shifter: as many samples out as in, the pitch moved
    // by semitones, the sound delayed by latency() samples (report it to the host)
    class PitchShift
    {
    public:
        void prepare (double sampleRate, int numChannels, int maxBlock)
        {
            channels = std::clamp (numChannels, 1, 2);
            core.prepare (sampleRate, channels, maxBlock);
            buf.assign ((size_t) channels, std::vector<float> ((size_t) std::max (1, maxBlock), 0.0f));
        }
        void reset() { core.reset(); }
        void setSemitones (float semitones) { core.setSemitones (semitones); }
        int latency() const { return core.inputLatency() + core.outputLatency(); }

        // numCh channels of n samples, in place
        void process (float* const* ch, int numCh, int n)
        {
            const int nc = std::min (numCh, channels);
            float* outs[2] {};
            for (int c = 0; c < channels; ++c)
            {
                auto& b = buf[(size_t) c];
                if ((int) b.size() < n) b.resize ((size_t) n);
                outs[c] = b.data();
            }
            const float* ins[2] { ch[0], nc > 1 ? ch[1] : ch[0] };
            core.process (ins, n, outs, n);
            for (int c = 0; c < nc; ++c) std::copy (outs[c], outs[c] + n, ch[c]);
        }

    private:
        int channels = 2;
        TimeStretch core;
        std::vector<std::vector<float>> buf;
    };
}
