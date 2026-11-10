#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace vs
{
    // The FFT for every header here, in double precision and without JUCE.
    //
    //   Fft      complex radix-2, in place: forward (e^-j) and inverse (scaled
    //            by 1/n). Its bit-reversal and twiddle tables are built once,
    //            when it is made, so a size that runs often costs only the
    //            butterflies.
    //   fftFor   one shared Fft per size (its transforms are const, so any
    //            number of callers can use it at once).
    //   RealFft  real input of size n: the n/2 + 1 bins from 0 to Nyquist, and
    //            back, through one complex transform of half the size.
    class Fft
    {
    public:
        Fft() = default;
        explicit Fft (int size) { setSize (size); }

        // size: a power of two (1 or more). Allocates; call it off the audio thread.
        void setSize (int size)
        {
            n = size;
            rev.assign ((size_t) n, 0u);
            for (int i = 1, j = 0; i < n; ++i)
            {
                int bit = n >> 1;
                for (; j & bit; bit >>= 1) j ^= bit;
                j ^= bit;
                rev[(size_t) i] = (std::uint32_t) j;
            }
            cosT.assign ((size_t) (n >> 1), 0.0);
            sinT.assign ((size_t) (n >> 1), 0.0);
            for (int i = 0; i < (n >> 1); ++i)
            {
                cosT[(size_t) i] = std::cos ((-2.0 * pi * i) / n);
                sinT[(size_t) i] = std::sin ((-2.0 * pi * i) / n);
            }
        }

        int size() const { return n; }

        static bool isPowerOfTwo (int v) { return v > 0 && (v & (v - 1)) == 0; }

        // In place, forward (e^-j), unscaled.
        void forward (double* re, double* im) const
        {
            for (int i = 1; i < n; ++i)
            {
                const int j = (int) rev[(size_t) i];
                if (i < j)
                {
                    double t = re[i]; re[i] = re[j]; re[j] = t;
                    t = im[i]; im[i] = im[j]; im[j] = t;
                }
            }
            const double* C = cosT.data();
            const double* S = sinT.data();
            for (int len = 2; len <= n; len <<= 1)
            {
                const int half = len >> 1, step = n / len;
                for (int i = 0; i < n; i += len)
                {
                    for (int k = 0, t = 0; k < half; ++k, t += step)
                    {
                        const int a = i + k, b = a + half;
                        const double wr = C[t], wi = S[t];
                        const double tr = re[b] * wr - im[b] * wi;
                        const double ti = re[b] * wi + im[b] * wr;
                        re[b] = re[a] - tr;
                        im[b] = im[a] - ti;
                        re[a] += tr;
                        im[a] += ti;
                    }
                }
            }
        }

        // In place, inverse (e^+j), scaled by 1/n: the forward transform with re and im swapped.
        void inverse (double* re, double* im) const
        {
            forward (im, re);
            const double s = 1.0 / n;
            for (int i = 0; i < n; ++i)
            {
                re[i] *= s;
                im[i] *= s;
            }
        }

    private:
        static constexpr double pi = 3.141592653589793;
        int n = 0;
        std::vector<std::uint32_t> rev;
        std::vector<double> cosT, sinT;
    };

    // One shared Fft per size, made the first time a size is asked for.
    inline const Fft& fftFor (int size)
    {
        static std::mutex lock;
        static std::map<int, std::unique_ptr<Fft>> cache;
        const std::lock_guard<std::mutex> hold (lock);
        auto& f = cache[size];
        if (! f) f = std::make_unique<Fft> (size);
        return *f;
    }

    class RealFft
    {
    public:
        RealFft() = default;
        explicit RealFft (int size) { setSize (size); }

        // size: a power of two, 2 or more. Allocates; call it off the audio thread.
        void setSize (int size)
        {
            n = size;
            h = n / 2;
            half.setSize (h);
            wr.assign ((size_t) h, 0.0);
            wi.assign ((size_t) h, 0.0);
            for (int k = 0; k < h; ++k)
            {
                wr[(size_t) k] = std::cos ((-2.0 * pi * k) / n);
                wi[(size_t) k] = std::sin ((-2.0 * pi * k) / n);
            }
            zr.assign ((size_t) h, 0.0);
            zi.assign ((size_t) h, 0.0);
        }

        int size() const { return n; }
        int bins() const { return h + 1; }

        // x: n real samples. re, im: the bins 0..n/2 (n/2 + 1 of each), unscaled.
        template <typename T>
        void forward (const T* x, double* re, double* im)
        {
            for (int k = 0; k < h; ++k)
            {
                zr[(size_t) k] = (double) x[2 * k];
                zi[(size_t) k] = (double) x[2 * k + 1];
            }
            half.forward (zr.data(), zi.data());
            re[0] = zr[0] + zi[0];
            im[0] = 0.0;
            re[h] = zr[0] - zi[0];
            im[h] = 0.0;
            for (int k = 1; k < h; ++k)
            {
                const double ar = zr[(size_t) k], ai = zi[(size_t) k];
                const double br = zr[(size_t) (h - k)], bi = -zi[(size_t) (h - k)]; // conj (Z[h-k])
                const double er = 0.5 * (ar + br), ei = 0.5 * (ai + bi);
                // odd part: (Z[k] - conj (Z[h-k])) / 2i
                const double or_ = 0.5 * (ai - bi), oi = -0.5 * (ar - br);
                const double cr = wr[(size_t) k], ci = wi[(size_t) k];
                re[k] = er + (cr * or_ - ci * oi);
                im[k] = ei + (cr * oi + ci * or_);
            }
        }

        // The power |X[k]|^2 of bins 0..n/2 into power[0..n/2].
        template <typename T>
        void power (const T* x, double* power, double* re, double* im)
        {
            forward (x, re, im);
            for (int k = 0; k <= h; ++k) power[k] = re[k] * re[k] + im[k] * im[k];
        }

        // re, im: bins 0..n/2 of a real signal (the imaginary parts at 0 and
        // Nyquist are taken as 0). x: the n samples, scaled by 1/n so that
        // inverse (forward (x)) == x.
        template <typename T>
        void inverse (const double* re, const double* im, T* x)
        {
            zr[0] = 0.5 * (re[0] + re[h]);
            zi[0] = 0.5 * (re[0] - re[h]);
            for (int k = 1; k < h; ++k)
            {
                const double ar = re[k], ai = im[k];
                const double br = re[h - k], bi = -im[h - k]; // conj (X[h-k])
                const double er = 0.5 * (ar + br), ei = 0.5 * (ai + bi);
                const double dr = 0.5 * (ar - br), di = 0.5 * (ai - bi);
                // odd part: (X[k] - conj (X[h-k])) / 2 times conj (W^k)
                const double cr = wr[(size_t) k], ci = -wi[(size_t) k];
                const double or_ = dr * cr - di * ci, oi = dr * ci + di * cr;
                // Z = E + i O
                zr[(size_t) k] = er - oi;
                zi[(size_t) k] = ei + or_;
            }
            half.inverse (zr.data(), zi.data());
            for (int k = 0; k < h; ++k)
            {
                x[2 * k] = (T) zr[(size_t) k];
                x[2 * k + 1] = (T) zi[(size_t) k];
            }
        }

    private:
        static constexpr double pi = 3.141592653589793;
        int n = 0, h = 0;
        Fft half;
        std::vector<double> wr, wi, zr, zi;
    };
}
