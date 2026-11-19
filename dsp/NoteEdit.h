#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace vs
{
    // Editing notes on a beat grid (a piano roll, a drum grid, a step
    // sequencer): the grid sizes, snapping and quantizing, transposing and
    // velocities. Beats are quarter notes; note numbers and velocities are
    // MIDI's (0..127, a played note's velocity 1..127).
    namespace noteedit
    {
        // A grid size: its id and how many beats one step is (the product names it)
        struct Grid { const char* id; double beats; };

        // Every grid size, in the order a list shows them
        inline constexpr std::array<Grid, 6> grids { {
            { "1/4", 1.0 }, { "1/8", 0.5 }, { "1/16", 0.25 }, { "1/32", 0.125 },
            { "1/8T", 1.0 / 3.0 }, { "1/16T", 1.0 / 6.0 },
        } };

        // A beat snapped to the nearest step of the grid
        inline double quantize (double beat, double grid) { return grid > 0 ? std::round (beat / grid) * grid : beat; }

        // A beat moved down to the step it is in
        inline double floorToGrid (double beat, double grid) { return grid > 0 ? std::floor (beat / grid) * grid : beat; }

        // A note number moved by semitones, kept to MIDI's range
        inline int transpose (int note, int semitones) { return std::clamp (note + semitones, 0, 127); }

        // A played note's velocity: set to `to` when it's 0 or more, else moved by delta; kept to 1..127
        inline int velocity (int current, int to, int delta = 0) { return std::clamp (to >= 0 ? to : current + delta, 1, 127); }

        // A drum hit's velocities, loudest first
        inline constexpr int accentVelocity = 127, normalVelocity = 100, softVelocity = 70, ghostVelocity = 40;

        // A velocity that counts as an accent (a click on it goes back to normal)
        inline constexpr int accentFrom = 120;

        // A step grid over a clip: how many steps, where each starts, how long a hit lasts
        struct StepGrid
        {
            double grid = 0.25;     // beats per step
            double offset = 0.0;    // the beat (in the clip's own counting) where step 0 starts

            // Steps in a clip of `beats` beats (at least one)
            int steps (double beats) const { return std::max (1, (int) std::ceil (beats / grid - 1.0e-6)); }
            double stepBeat (int s) const { return s * grid + offset; }
            // A hit is a little shorter than its step, so hits on neighbouring steps never touch
            double hitLength() const { return grid * 0.9; }
        };

        // A loop being played and recorded over and over (a pad player's loop
        // recorder): positions are kept in beats, so changing the tempo keeps the
        // groove; times are seconds on the player's clock, `start` being when
        // beat 0 of the first pass is.
        namespace loop
        {
            inline double wrap (double beat, double loopBeats) { return std::fmod (std::fmod (beat, loopBeats) + loopBeats, loopBeats); }

            // Where a time falls in the loop, 0 to loopBeats
            inline double beatAt (double time, double start, double bpm, double loopBeats) { return wrap ((time - start) * bpm / 60.0, loopBeats); }

            // A beat snapped to the nearest 1/steps of a beat (0: left as it is), wrapping at the loop's end
            inline double snap (double beat, int steps, double loopBeats) { return steps > 0 ? wrap (quantize (beat, 1.0 / steps), loopBeats) : beat; }

            // A recorded hit's time in [from, to): which of the hits, and when, for every pass of the loop in that window
            struct Due { double time; int hit; };

            // beats: each hit's place; notBefore (may be null): a hit doesn't sound before this time (0: always)
            inline std::vector<Due> hitsBetween (const double* beats, const double* notBefore, int count, double from, double to,
                                                 double start, double bpm, double loopBeats)
            {
                std::vector<Due> out;
                const double spb = 60.0 / bpm, len = loopBeats * spb;
                if (to <= start) return out;
                const double k0 = std::max (0.0, std::floor ((from - start) / len)), k1 = std::floor ((to - start) / len);
                for (double k = k0; k <= k1; ++k)
                    for (int h = 0; h < count; ++h)
                    {
                        const double t = start + k * len + beats[h] * spb;
                        const double nb = notBefore != nullptr ? notBefore[h] : 0.0;
                        if (t >= from && t < to && t >= start && ! (nb != 0.0 && ! std::isnan (nb) && t < nb)) out.push_back ({ t, h });
                    }
                std::stable_sort (out.begin(), out.end(), [] (const Due& a, const Due& b) { return a.time < b.time; });
                return out;
            }

            // The beats' clicks in [from, to): when, which beat (counted from start), and whether it begins a bar
            struct Click { double time; int beat; bool down; };

            inline std::vector<Click> clicksBetween (double from, double to, double start, double bpm, int beatsPerBar = 4)
            {
                std::vector<Click> out;
                const double spb = 60.0 / bpm;
                for (auto b = (std::int64_t) std::ceil ((from - start) / spb - 1e-9); start + (double) b * spb < to; ++b)
                    out.push_back ({ start + (double) b * spb, (int) b, ((b % beatsPerBar) + beatsPerBar) % beatsPerBar == 0 });
                return out;
            }

            // The next time on a grid of `interval` seconds counted from `origin`, at or after t
            inline double nextGridTime (double t, double origin, double interval) { return origin + std::ceil ((t - origin) / interval - 1e-9) * interval; }
        }
    }
}
