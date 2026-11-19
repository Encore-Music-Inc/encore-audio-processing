#pragma once

#include <algorithm>
#include <cmath>

namespace vs
{
    // An ADSR envelope, one value (0..1) per sample: attack rises in a
    // straight line, decay and release fall exponentially (to within -80 dB
    // over their times), sustain holds. A new note-on starts the attack from
    // where it is (no click on a retrigger). A second, slower decay (decay2,
    // to sustain2) can take over once the first has settled for three of its
    // time constants: a piano's quick drop and long fade.
    class Envelope
    {
    public:
        struct Settings
        {
            float attack = 0.005f, decay = 0.2f, sustain = 0.7f, release = 0.3f;   // seconds; sustain 0..1
            float decay2 = 0.0f, sustain2 = 0.0f;                                  // 0: no second decay
        };

        // The length (seconds) to within -80 dB of an exponential with time
        // constant tau: an envelope time for a curve given by its time constant
        static constexpr double timeForTimeConstant (double tau) { return tau * 9.210340371976184; }

        void prepare (double sampleRate) { fs = sampleRate; setSettings (set); }

        void setSettings (const Settings& s)
        {
            set = s;
            attackStep = 1.0 / std::max (1.0, set.attack * fs);
            auto coef = [this] (float seconds) { return std::exp (std::log (1.0e-4) / std::max (1.0, seconds * fs)); };
            decayCoef = coef (set.decay);
            decay2Coef = coef (set.decay2);
            releaseCoef = coef (set.release);
            secondAfter = set.decay2 > 0.0f ? (long) std::lround (3.0 * set.decay / 9.210340371976184 * fs) : -1;
        }

        void noteOn() { stage = Stage::attack; attackStepNow = attackStep; decayed = 0; }
        // A note-on whose attack starts at `from` (0..1) and takes `attackSeconds`
        // to the top: a slurred note sliding in without a new attack
        void noteOnFrom (double from, double attackSeconds)
        {
            value = std::clamp (from, 0.0, 1.0);
            stage = Stage::attack;
            attackStepNow = (1.0 - value) / std::max (1.0, attackSeconds * fs);
            decayed = 0;
        }
        void noteOff() { if (stage != Stage::idle) stage = Stage::release; }
        void reset() { stage = Stage::idle; value = 0.0; }

        bool isActive() const { return stage != Stage::idle; }
        bool isReleasing() const { return stage == Stage::release; }
        float current() const { return (float) value; }

        float next()
        {
            switch (stage)
            {
                case Stage::idle: return 0.0f;
                case Stage::attack:
                    value += attackStepNow;
                    if (value >= 1.0) { value = 1.0; stage = Stage::decay; }
                    break;
                case Stage::decay:
                    if (secondAfter >= 0 && decayed >= secondAfter)
                    {
                        value = set.sustain2 + (value - set.sustain2) * decay2Coef;
                        if (std::abs (value - set.sustain2) < 1.0e-5) { value = set.sustain2; stage = set.sustain2 > 0.0f ? Stage::sustain : Stage::idle; }
                        break;
                    }
                    ++decayed;
                    value = set.sustain + (value - set.sustain) * decayCoef;
                    if (secondAfter < 0 && value - set.sustain < 1.0e-5) { value = set.sustain; stage = Stage::sustain; }
                    break;
                case Stage::sustain:
                    value = secondAfter >= 0 ? set.sustain2 : set.sustain;
                    break;
                case Stage::release:
                    value *= releaseCoef;
                    if (value < 1.0e-5) { value = 0.0; stage = Stage::idle; }
                    break;
            }
            return (float) value;
        }

    private:
        enum class Stage { idle, attack, decay, sustain, release };
        double fs = 48000.0, value = 0.0, attackStep = 0.0, attackStepNow = 0.0, decayCoef = 0.0, decay2Coef = 0.0, releaseCoef = 0.0;
        long secondAfter = -1, decayed = 0;
        Settings set;
        Stage stage = Stage::idle;
    };
}
