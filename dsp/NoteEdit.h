#pragma once

#include <algorithm>
#include <array>
#include <cmath>

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
    }
}
