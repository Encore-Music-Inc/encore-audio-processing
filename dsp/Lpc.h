#pragma once

#include <algorithm>
#include <cmath>
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

        // The predictors a lossless encoder chooses from for one block: x[0..n)
        // times the window w (a FLAC encoder's Tukey (0.5), windows::tukey), its
        // autocorrelation to lag maxOrder, then predictorsUpTo. Calls
        // onOrder (order, a, err) for each order with a positive error.
        template <typename T, typename OnOrder>
        void blockPredictors (const T* x, int n, const double* w, int maxOrder, OnOrder&& onOrder)
        {
            if (n <= 0 || maxOrder <= 0) return;
            std::vector<double> y ((size_t) n), r ((size_t) maxOrder + 1);
            for (int i = 0; i < n; ++i) y[(size_t) i] = (double) x[i] * w[i];
            autocorrelation (y.data(), n, maxOrder, r.data());
            predictorsUpTo (r.data(), maxOrder, onOrder);
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

        // Rebuilds the samples of x listed in `missing` (sorted indices into
        // x[0..length)) from the audio around them: the values that make the
        // prediction error of the predictor a (a[1..p], a[0] unused) over the
        // surrounding stretch as small as possible (least-squares
        // autoregressive interpolation). The normal equations are banded
        // Toeplitz in the error filter's autocorrelation, solved by a banded
        // Cholesky factorisation. x is changed in place; samples before 0 or
        // from `length` on count as absent.
        template <typename T>
        void gapFill (T* x, long length, const std::vector<long>& missing, const double* a, int p)
        {
            const int m = (int) missing.size();
            if (m == 0) return;
            std::vector<double> b ((size_t) p + 1), rb ((size_t) p + 1);
            b[0] = 1.0;
            for (int k = 1; k <= p; ++k) b[(size_t) k] = -a[k];
            for (int k = 0; k <= p; ++k)
            {
                double s = 0.0;
                for (int l = 0; l + k <= p; ++l) s += b[(size_t) l] * b[(size_t) (l + k)];
                rb[(size_t) k] = s;
            }
            for (long t : missing) x[t] = (T) 0;
            // the prediction errors that touch a missing sample, with those at zero
            const long t0 = missing.front(), t1 = missing.back() + p;
            std::vector<double> e0 ((size_t) (t1 - t0 + 1));
            for (long n = t0; n <= t1; ++n)
            {
                double s = 0.0;
                for (int k = 0; k <= p; ++k)
                {
                    const long at = n - k;
                    if (at >= 0 && at < length) s += b[(size_t) k] * (double) x[at];
                }
                e0[(size_t) (n - t0)] = s;
            }
            std::vector<double> rhs ((size_t) m);
            for (int i = 0; i < m; ++i)
            {
                double s = 0.0;
                for (int k = 0; k <= p; ++k) s += b[(size_t) k] * e0[(size_t) (missing[(size_t) i] + k - t0)];
                rhs[(size_t) i] = -s;
            }
            // R[i][j] = rb[|ti - tj|]: banded, at most p either side in index space
            const int bw = std::min (p, m - 1), w = bw + 1;
            std::vector<double> L ((size_t) m * (size_t) w, 0.0);
            auto at = [&] (int i, int j) -> double& { return L[(size_t) i * (size_t) w + (size_t) (i - j)]; };
            for (int i = 0; i < m; ++i)
            {
                for (int j = std::max (0, i - bw); j <= i; ++j)
                {
                    const long d = missing[(size_t) i] - missing[(size_t) j];
                    double s = d <= p ? rb[(size_t) d] : 0.0;
                    if (i == j) s *= 1 + 1e-9;
                    for (int k = std::max (0, i - bw); k < j; ++k) s -= at (i, k) * at (j, k);
                    if (i == j) at (i, i) = std::sqrt (std::max (s, 1e-24));
                    else at (i, j) = s / at (j, j);
                }
            }
            std::vector<double> y ((size_t) m);
            for (int i = 0; i < m; ++i)
            {
                double s = rhs[(size_t) i];
                for (int k = std::max (0, i - bw); k < i; ++k) s -= at (i, k) * y[(size_t) k];
                y[(size_t) i] = s / at (i, i);
            }
            for (int i = m - 1; i >= 0; --i)
            {
                double s = y[(size_t) i];
                for (int k = i + 1; k <= std::min (m - 1, i + bw); ++k) s -= at (k, i) * y[(size_t) k];
                y[(size_t) i] = s / at (i, i);
            }
            for (int i = 0; i < m; ++i) x[missing[(size_t) i]] = (T) y[(size_t) i];
        }
    }
}
