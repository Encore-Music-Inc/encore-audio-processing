#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>
#include "Util.h"

namespace vs
{
    // Automatic fader rider. Measures the vocal's RMS level and moves a gain
    // toward the target, limited to +/- range. When the singer stops (level far
    // below target) it holds its last position instead of cranking up the
    // room noise, so breaths and bleed don't get pulled up between phrases.
    class Rider
    {
    public:
        void prepare (double sampleRate)
        {
            fs = sampleRate;
            detCoef = onePole (30.0f, fs);
            reset();
        }

        void reset()
        {
            env = 1.0e-6f;
            gainDb = desiredDb = 0.0f;
        }

        void setParams (float newTargetDb, float newRangeDb, float speed01)
        {
            targetDb = newTargetDb;
            rangeDb = newRangeDb;
            // speed 0 = slow, gentle rides (600 ms), 1 = fast, word-by-word (60 ms)
            moveCoef = onePole (600.0f * std::pow (0.1f, speed01), fs);
        }

        // `follow` channels (e.g. harmony voices) get the same gain moves but
        // don't affect the measurement.
        void process (float* const* ch, int numCh, int numSamples, float* const* follow = nullptr, int numFollow = 0)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                float sq = 0.0f;
                for (int c = 0; c < numCh; ++c)
                    sq = std::max (sq, ch[c][i] * ch[c][i]);

                env += detCoef * (sq - env);
                const float levelDb = powerToDb (env);

                if (levelDb > targetDb - 30.0f)
                    desiredDb = std::clamp (targetDb - levelDb, -rangeDb, rangeDb);

                gainDb += moveCoef * (desiredDb - gainDb);
                const float g = dbToGain (gainDb);

                for (int c = 0; c < numCh; ++c)
                    ch[c][i] *= g;
                for (int c = 0; c < numFollow; ++c)
                    follow[c][i] *= g;
            }

            meterDb.store (gainDb);
        }

        std::atomic<float> meterDb { 0.0f };

    private:
        double fs = 44100.0;
        float detCoef = 0.0f, moveCoef = 0.0f;
        float env = 0.0f, gainDb = 0.0f, desiredDb = 0.0f;
        float targetDb = -18.0f, rangeDb = 6.0f;
    };

    // The rider's slow form, for a whole recording (a live set): it plans a gain
    // track, one value every hopSec, instead of changing the audio, so the moves
    // can be changed or left off later. It aims the level of the mid, averaged
    // over about 3 s, at the target, moving at most 1 dB a second within +/-
    // range. Like the vocal rider it holds still when nothing is playing (well
    // under the target, or 12 dB under its own average), and silence doesn't
    // drag the average down.
    class LevelRider
    {
    public:
        static constexpr double hopSec = 0.1;

        explicit LevelRider (double sampleRate, double targetDb = -18.0, double rangeDb = 12.0)
            : target (targetDb), range (rangeDb), hopN ((int) std::floor (sampleRate * hopSec + 0.5)), envCoef (1.0 - std::exp (-hopSec / 3.0))
        {
        }

        // The next block (r null: one channel)
        void push (const float* l, const float* r, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                const double m = midOf ((double) l[i], (double) (r != nullptr ? r[i] : l[i]));
                acc += m * m;
                if (++inHop == hopN)
                {
                    const double power = acc / inHop;
                    const double now = db10 (power, 1e-12);
                    const bool playing = now > target - 30.0 && now > db10 (env, 1e-12) - 12.0;
                    if (playing) env += envCoef * (power - env);
                    const double level = db10 (env, 1e-12);
                    if (playing)
                    {
                        const double want = std::max (-range, std::min (range, target - level));
                        const double step = 1.0 * hopSec;   // dB per hop
                        gainDb += std::max (-step, std::min (step, want - gainDb));
                    }
                    moves.push_back ((float) gainDb);
                    acc = 0.0;
                    inHop = 0;
                }
            }
        }

        // The gain (dB) for each hop so far
        const std::vector<float>& track() const { return moves; }
        // The level the ride is giving now (dB): the average and the gain
        double levelDb() const { return db10 (env, 1e-12) + gainDb; }

    private:
        double target, range;
        int hopN;
        double envCoef;
        double acc = 0.0, env = 1.0e-6, gainDb = 0.0;
        int inHop = 0;
        std::vector<float> moves;
    };
}
