#pragma once

#include <vector>

namespace vs
{
    // Linear prediction: the autocorrelation of a (windowed) stretch of audio
    // and the Levinson-Durbin recursion that turns it into a predictor
    // x[n] ~ sum a[k] x[n-k], k = 1..p. Used to fill gaps and repair clicks,
    // to find note onsets, and to encode FLAC.
    namespace lpc
    {
        // r[k] = sum x[i] x[i-k] for lags k = 0..p over x[0..n). The caller applies any window first.
        template <typename T>
        void autocorrelation (const T* x, int n, int p, double* r)
        {
            for (int k = 0; k <= p; ++k)
            {
                double s = 0.0;
                for (int i = k; i < n; ++i) s += (double) x[i] * (double) x[i - k];
                r[k] = s;
            }
        }

        // The autocorrelation of x[from..from+n) times the window w[0..n), lags
        // 0..p. Samples outside x[0..length), and samples that are not numbers, count as 0.
        template <typename T>
        void windowedAutocorrelation (const T* x, long length, long from, int n, const double* w, int p, double* r)
        {
            std::vector<double> seg ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                const long at = from + i;
                double v = at >= 0 && at < length ? (double) x[at] : 0.0;
                if (v != v) v = 0.0;
                seg[(size_t) i] = v * w[i];
            }
            autocorrelation (seg.data(), n, p, r);
        }

        // One Levinson-Durbin recursion. r[0] is first raised to
        // r[0] (1 + noiseRatio) + noiseFloor: a touch of white noise that keeps it
        // stable on silence. Then for each order i = 1..p: the reflection
        // coefficient, the predictor a[1..i] (a[0] = 0), and the error. After
        // each order it calls onOrder (i, a, err) unless the error has fallen to
        // 0 or below, where it stops: then `a` already holds order i and the
        // return value is i - 1, the last order with a positive error.
        // a must hold p + 1 values.
        template <typename OnOrder>
        int levinson (const double* r, int p, double noiseRatio, double noiseFloor, double* a, double& err, OnOrder&& onOrder)
        {
            std::vector<double> tmp ((size_t) p + 1, 0.0);
            for (int i = 0; i <= p; ++i) a[i] = 0.0;
            err = r[0] * (1 + noiseRatio) + noiseFloor;
            for (int i = 1; i <= p; ++i)
            {
                double acc = r[i];
                for (int j = 1; j < i; ++j) acc -= a[j] * r[i - j];
                const double k = acc / err;
                for (int j = 0; j <= p; ++j) tmp[(size_t) j] = a[j];
                a[i] = k;
                for (int j = 1; j < i; ++j) a[j] = tmp[(size_t) j] - k * tmp[(size_t) (i - j)];
                err *= 1 - k * k;
                if (! (err > 0)) return i - 1;
                onOrder (i, (const double*) a, err);
            }
            return p;
        }

        // The predictor of order p, a[1..p] (a[0] = 0), and its error, with a
        // 1e-9 white-noise bias (and 1e-20 floor). If the error reaches 0 the
        // recursion stops there and the error is 1e-20. a must hold p + 1 values.
        inline double predictor (const double* r, int p, double* a)
        {
            double err = 0.0;
            if (levinson (r, p, 1e-9, 1e-20, a, err, [] (int, const double*, double) {}) < p && ! (err > 0)) err = 1e-20;
            return err;
        }

        // The predictors of every order 1..maxOrder, each with its error, for an
        // encoder to choose from (1e-9 white-noise bias, no floor). Calls
        // onOrder (order, a, err) with a[1..order] the coefficients. None if r[0] <= 0.
        template <typename OnOrder>
        void predictorsUpTo (const double* r, int maxOrder, OnOrder&& onOrder)
        {
            if (r[0] <= 0) return;
            std::vector<double> a ((size_t) maxOrder + 1);
            double err = 0.0;
            levinson (r, maxOrder, 1e-9, 0.0, a.data(), err, onOrder);
        }

        // The prediction-error filter of order p: e[n] = sum a[k] x[n-k], k = 0..p,
        // a[0] = 1 (the predictor's coefficients negated), with a stronger 1e-4
        // white-noise bias (and 1e-12 floor) for short, noisy stretches. Stops early,
        // keeping the order it reached, if the error reaches 0. a must hold p + 1 values.
        inline void errorFilter (const double* r, int p, double* a)
        {
            double err = 0.0;
            levinson (r, p, 1e-4, 1e-12, a, err, [] (int, const double*, double) {});
            a[0] = 1.0;
            for (int k = 1; k <= p; ++k) a[k] = -a[k];
        }
    }
}
