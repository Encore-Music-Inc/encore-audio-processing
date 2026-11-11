#pragma once

#include <algorithm>
#include <cmath>

namespace vs
{
    // A phaser: four first-order allpass stages whose corner sweeps up and
    // down between a low and a high frequency (equal ratios each sample, so
    // the sweep sounds even), with feedback from the last stage into the
    // first, added to the dry signal (the notches are where they cancel).
    // Both channels share one sweep. Its settings are Tracktion Engine's
    // phaser (GPLv3), rewritten.
    class Phaser
    {
    public:
        struct Settings
        {
            float depth = 5.0f;     // octaves the sweep covers above its bottom
            float rate = 0.4f;      // how fast it sweeps (octaves per second, roughly)
            float feedback = 0.7f;  // 0 .. 0.95
        };

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            reset();
        }

        void reset()
        {
            for (auto& ch : state) std::fill (std::begin (ch), std::end (ch), 0.0);
            sweep = minSweep();
            rising = true;
        }

        void setSettings (const Settings& s)
        {
            set = s;
            set.depth = std::clamp (s.depth, 0.0f, 10.0f);
            set.feedback = std::clamp (s.feedback, 0.0f, 0.95f);
        }
        const Settings& getSettings() const { return set; }

        // numCh channels (1 or 2) of n samples, in place
        void process (float* const* ch, int numCh, int n)
        {
            const double range = std::pow (2.0, (double) set.depth);
            const double up = std::pow (range, set.rate / (fs / 2.0)), down = 1.0 / up;
            const double lo = minSweep(), hi = lo * range;
            const int nc = std::clamp (numCh, 0, 2);
            for (int i = 0; i < n; ++i)
            {
                const double coef = (1.0 - sweep) / (1.0 + sweep);
                for (int c = 0; c < nc; ++c)
                {
                    double* v = state[(size_t) c];
                    double t = ch[c][i] + set.feedback * v[7];
                    v[1] = coef * (v[1] + t) - v[0];     v[0] = t;
                    v[3] = coef * (v[3] + v[1]) - v[2];  v[2] = v[1];
                    v[5] = coef * (v[5] + v[3]) - v[4];  v[4] = v[3];
                    v[7] = coef * (v[7] + v[5]) - v[6];  v[6] = v[5];
                    for (int k = 0; k < 8; ++k) if (std::abs (v[k]) < 1.0e-20) v[k] = 0.0;
                    ch[c][i] = (float) (ch[c][i] + v[7]);
                }
                sweep *= rising ? up : down;
                if (sweep > hi) rising = false;
                else if (sweep < lo) rising = true;
            }
        }

    private:
        // where the sweep turns at the bottom (the allpass corner low in the audio range)
        double minSweep() const { return 3.141592653589793 * 100.0 / fs; }

        double fs = 48000.0, sweep = 0.0;
        bool rising = true;
        Settings set;
        double state[2][8] {};
    };
}
