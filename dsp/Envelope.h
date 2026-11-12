#pragma once

#include <algorithm>
#include <cmath>

namespace vs
{
    // An ADSR envelope, one value (0..1) per sample: attack rises in a
    // straight line, decay and release fall exponentially (to within -80 dB
    // over their times), sustain holds. A new note-on starts the attack from
    // where it is (no click on a retrigger).
    class Envelope
    {
    public:
        struct Settings { float attack = 0.005f, decay = 0.2f, sustain = 0.7f, release = 0.3f; }; // seconds; sustain 0..1

        void prepare (double sampleRate) { fs = sampleRate; setSettings (set); }

        void setSettings (const Settings& s)
        {
            set = s;
            attackStep = 1.0 / std::max (1.0, set.attack * fs);
            auto coef = [this] (float seconds) { return std::exp (std::log (1.0e-4) / std::max (1.0, seconds * fs)); };
            decayCoef = coef (set.decay);
            releaseCoef = coef (set.release);
        }

        void noteOn() { stage = Stage::attack; }
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
                    value += attackStep;
                    if (value >= 1.0) { value = 1.0; stage = Stage::decay; }
                    break;
                case Stage::decay:
                    value = set.sustain + (value - set.sustain) * decayCoef;
                    if (value - set.sustain < 1.0e-5) { value = set.sustain; stage = Stage::sustain; }
                    break;
                case Stage::sustain:
                    value = set.sustain;
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
        double fs = 48000.0, value = 0.0, attackStep = 0.0, decayCoef = 0.0, releaseCoef = 0.0;
        Settings set;
        Stage stage = Stage::idle;
    };
}
