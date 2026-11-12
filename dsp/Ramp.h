#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace vs
{
    // A value that ramps to a new target over a fixed number of samples,
    // linearly (equal steps) or multiplicatively (equal ratios, for gains),
    // number for number as JUCE's SmoothedValue does (a multiplicative ramp
    // starts at 1). A new target closer than float precision to the old one
    // is ignored. The browser build's juce::SmoothedValue (lite/JuceLite.h)
    // is this ramp.
    template <bool multiplicative>
    class Ramp
    {
    public:
        // (a time or rate of 0 or less leaves everything as it is)
        void reset (double sampleRate, double seconds)
        {
            if (! (sampleRate > 0 && seconds > 0)) return;
            stepsToTarget = (int) std::floor (seconds * sampleRate);
            setCurrentAndTarget (target);
        }

        void setCurrentAndTarget (float v) { target = current = v; countdown = 0; }

        void setTarget (float v)
        {
            if (nearlyEqual (v, target)) return;
            if (stepsToTarget <= 0) { setCurrentAndTarget (v); return; }
            target = v;
            countdown = stepsToTarget;
            if constexpr (multiplicative)
                step = std::exp ((std::log (std::abs (target)) - std::log (std::abs (current))) / (float) countdown);
            else
                step = (target - current) / (float) countdown;
        }

        float next()
        {
            if (countdown <= 0) return target;
            --countdown;
            if (countdown > 0)
            {
                if constexpr (multiplicative) current *= step; else current += step;
            }
            else
            {
                current = target;
            }
            return current;
        }

        float skip (int n)
        {
            if (n >= countdown) { setCurrentAndTarget (target); return target; }
            if constexpr (multiplicative) current *= (float) std::pow (step, n); else current += step * (float) n;
            countdown -= n;
            return current;
        }

        bool isRamping() const { return countdown > 0; }
        float getTarget() const { return target; }
        float getCurrent() const { return current; }

    private:
        static bool nearlyEqual (float a, float b)
        {
            if (! (std::isfinite (a) && std::isfinite (b))) return a == b;
            const float d = std::abs (a - b);
            return d <= std::numeric_limits<float>::min() || d <= std::numeric_limits<float>::epsilon() * std::max (std::abs (a), std::abs (b));
        }

        float current = multiplicative ? 1.0f : 0.0f, target = current, step = 0.0f;
        int countdown = 0, stepsToTarget = 0;
    };

    // How a value moves between two points drawn over time (automation, a
    // tempo ramp): each segment from (x1, y1) to (x2, y2) has a curve amount c
    // from -1 to 1 set on its first point. 0 is a straight line. Up to 0.5
    // either way bends it as a quadratic Bezier through one control point;
    // beyond that the value holds flat at one end for part of the way and then
    // bends. Number for number as Tracktion Engine reads automation and tempo
    // curves, so songs saved with them play the same.
    namespace curve
    {
        struct Point { double x, y; };

        // The Bezier control point of a segment with curve amount c (-1 .. 1)
        inline Point bezierPoint (double x1, double y1, double x2, double y2, double c)
        {
            const double run = x2 - x1;
            if (y2 > y1)
            {
                const double rise = y2 - y1;
                return { x1 + run / 2 - run / 2 * -c, y1 + rise / 2 + rise / 2 * -c };
            }
            const double rise = y1 - y2;
            return { x1 + run / 2 - run / 2 * -c, y2 + rise / 2 - rise / 2 * -c };
        }

        // Where the bend starts and ends for a curve amount past 0.5 either way
        struct Ends { double x1, y1, x2, y2; };
        inline Ends bezierEnds (double x1, double y1, double x2, double y2, double c)
        {
            const double minic = (std::abs (c) - 0.5f) * 2.0f;
            const double run = minic * (x2 - x1);
            const double rise = minic * ((y2 > y1) ? (y2 - y1) : (y1 - y2));
            if (c > 0)
                return { x1 + run, (float) y1, x2, (float) (y1 < y2 ? (y2 - rise) : (y2 + rise)) };
            return { x1, (float) (y1 < y2 ? (y1 + rise) : (y1 - rise)), x2 - run, (float) y2 };
        }

        // y at x on the quadratic Bezier from (x1, y1) through control (xb, yb) to (x2, y2)
        inline double bezierYFromX (double x, double x1, double y1, double xb, double yb, double x2, double y2)
        {
            if (x1 == x2 || y1 == y2) return y1;
            if (x <= x1) return y1;
            if (x >= x2) return y2;
            const double a = x1 - 2 * xb + x2, b = -2 * x1 + 2 * xb, c = x1 - x;
            double t;
            if (a == 0)
                t = -c / b;
            else
            {
                t = (-b + std::sqrt (b * b - 4 * a * c)) / (2 * a);
                if (t < 0.0f || t > 1.0f) t = (-b - std::sqrt (b * b - 4 * a * c)) / (2 * a);
            }
            return (std::pow (1 - t, 2) * y1) + 2 * t * (1 - t) * yb + std::pow (t, 2) * y2;
        }

        // x at parameter t (0 .. 1) along the same Bezier
        inline double bezierXFromT (double t, double x1, double xb, double x2)
        {
            if (x1 == x2) return (x1 + x2) / 2.0 * t + x1;
            return (std::pow (1.0 - t, 2.0) * x1) + 2 * t * (1 - t) * xb + std::pow (t, 2.0) * x2;
        }

        // The value at x inside the segment from (x1, y1), curve amount c, to (x2, y2)
        inline float valueAt (double x, double x1, float y1, float c, double x2, float y2)
        {
            if (c == 0.0f)
            {
                const auto alpha = (float) ((x - x1) / (x2 - x1));
                return y1 + alpha * (y2 - y1);
            }
            const double amount = std::clamp (c * 2.0f, -1.0f, 1.0f);
            const auto bp = bezierPoint (x1, y1, x2, y2, amount);
            if (c >= -0.5f && c <= 0.5f)
                return (float) bezierYFromX (x, x1, y1, bp.x, (float) bp.y, x2, y2);
            const auto e = bezierEnds (x1, y1, x2, y2, c);
            if (x >= x1 && x <= e.x1) return y1;
            if (x >= e.x2 && x <= x2) return y2;
            return (float) bezierYFromX (x, e.x1, (float) e.y1, bp.x, (float) bp.y, e.x2, (float) e.y2);
        }
    }
}

