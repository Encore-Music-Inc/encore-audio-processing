#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace vs
{
    // Small statistics on arrays of numbers: median, percentile, mean,
    // standard deviation and a least-squares line. Each works in double
    // precision on a pointer and a count. An empty array gives 0.
    namespace stats
    {
        // The middle value (the mean of the two middle values for an even count).
        template <typename T>
        double median (const T* x, int n)
        {
            if (n <= 0) return 0.0;
            std::vector<double> s (x, x + n);
            std::sort (s.begin(), s.end());
            const size_t m = (size_t) n >> 1;
            return (n & 1) ? s[m] : 0.5 * (s[m - 1] + s[m]);
        }

        // The value at fraction p (0..1) of the sorted array: element floor (p n),
        // kept inside the array.
        template <typename T>
        double percentile (const T* x, int n, double p)
        {
            if (n <= 0) return 0.0;
            std::vector<double> s (x, x + n);
            std::sort (s.begin(), s.end());
            const double at = std::floor (p * n);
            const int i = at < 0.0 ? 0 : at > (double) (n - 1) ? n - 1 : (int) at;
            return s[(size_t) i];
        }

        // The median of x[0..n) (element n / 2 once sorted; for an even count the
        // upper of the two middle values), found by quickselect. Reorders x; for
        // hot loops on small arrays.
        template <typename T>
        T medianInPlace (T* a, int n)
        {
            int lo = 0, hi = n - 1;
            const int k = n >> 1;
            while (lo < hi)
            {
                const T pivot = a[(lo + hi) >> 1];
                int i = lo, j = hi;
                while (i <= j)
                {
                    while (a[i] < pivot) ++i;
                    while (a[j] > pivot) --j;
                    if (i <= j)
                    {
                        const T t = a[i]; a[i] = a[j]; a[j] = t;
                        ++i; --j;
                    }
                }
                if (j < k) lo = i;
                if (k < i) hi = j;
                if (j < k && k < i) break;
            }
            return a[k];
        }

        template <typename T>
        double mean (const T* x, int n)
        {
            double s = 0.0;
            for (int i = 0; i < n; ++i) s += (double) x[i];
            return s / (n > 0 ? n : 1);
        }

        // Population standard deviation (divides by n).
        template <typename T>
        double standardDeviation (const T* x, int n)
        {
            const double m = mean (x, n);
            double s = 0.0;
            for (int i = 0; i < n; ++i) s += ((double) x[i] - m) * ((double) x[i] - m);
            return std::sqrt (s / (n > 0 ? n : 1));
        }

        // The least-squares line through the points (x[i], y[i]), as its slope
        // and the means it passes through: at (x) = meanY + slope (x - meanX).
        // All x the same: slope 0.
        struct Line
        {
            double slope = 0.0, meanX = 0.0, meanY = 0.0;
            double at (double x) const { return meanY + slope * (x - meanX); }
        };

        template <typename T>
        Line fitLine (const T* x, const T* y, int n)
        {
            Line l;
            l.meanX = mean (x, n);
            l.meanY = mean (y, n);
            double num = 0.0, den = 0.0;
            for (int i = 0; i < n; ++i)
            {
                num += ((double) x[i] - l.meanX) * ((double) y[i] - l.meanY);
                den += ((double) x[i] - l.meanX) * ((double) x[i] - l.meanX);
            }
            l.slope = den != 0.0 ? num / den : 0.0;
            return l;
        }
    }
}
