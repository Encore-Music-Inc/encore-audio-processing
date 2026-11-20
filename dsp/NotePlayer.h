// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// Notes played to the sample for a theory tool: a chord strummed or struck
// together, a melody, a progression in its rhythm. Each note is a pluck
// (plucked strings, or a piano's struck string) at its pitch, scheduled at a
// frame with a gain and cut off after its length; the mix goes through a
// master volume and a gentle compressor, so a strum and a single note sit at
// about the same level. Plain C++ with no JUCE, so the browser (master.wasm)
// plays the same sound.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include "Dynamics.h"
#include "Metronome.h"
#include "Music.h"
#include "Oscillator.h"
#include "Util.h"

namespace vs
{
    class NotePlayer
    {
    public:
        enum class Sound { strings, piano };

        // The volume slider (0 to 100), squared, at most 0.9
        static double levelOf (double slider) { const double v = std::clamp (slider, 0.0, 100.0) / 100; return v * v * 0.9; }

        // The compressor: the browser's DynamicsCompressor as the tools set it (threshold
        // -14 dB, ratio 4; its 30 dB knee, 3 ms attack and 250 ms release), with its
        // makeup gain (the full-scale level's cut, to the power 0.6, given back)
        static constexpr float thresholdDb = -14.0f, ratio = 4.0f, kneeDb = 30.0f, attackSec = 0.003f, releaseSec = 0.25f;
        static float makeupDb() { return 0.6f * dynamics::cutDb (0.0f - thresholdDb, 1.0f - 1.0f / ratio, kneeDb); }

        void prepare (double sampleRate)
        {
            sr = sampleRate;
            voices.clear();
            sounds.clear();
            cut = 0;
            gain = target = (float) levelOf (70);
            back = -1;
        }

        void setVolume (double slider)
        {
            target = (float) levelOf (slider);
            if (back < 0) gain = target;
        }

        // Notes (MIDI) from frame `at`: spread seconds apart (0 together; up plays the last
        // first), each note `seconds` long less its place in the strum, the chord's level
        // shared between its notes (half, over the square root of how many)
        void play (const std::vector<int>& notes, std::int64_t at, Sound sound, double spread, double level, double seconds, bool up = false)
        {
            if (notes.empty()) return;
            const double each = level * 0.5 / std::sqrt ((double) std::max<size_t> (1, notes.size()));
            const auto n = notes.size();
            for (size_t i = 0; i < n; ++i)
            {
                const int midi = up ? notes[n - 1 - i] : notes[i];
                const auto start = at + (std::int64_t) std::llround ((double) i * spread * sr);
                const auto release = at + (std::int64_t) std::llround (seconds * sr);
                auto buf = soundOf (sound, midi);
                voices.add (buf, buf.get(), start, each, std::max (start, release), 0.05 * sr, std::max (start, release) + (std::int64_t) std::llround (0.4 * sr));
            }
        }

        // Everything sounding fades out from frame `at` (over 30 ms) and is dropped; the
        // volume comes back for what's played next
        void silence (std::int64_t at)
        {
            fadeAt = at;
            back = at + (std::int64_t) std::llround (0.25 * sr);
        }

        // Writes frames [frame0, frame0 + n) into out; returns whether anything is sounding
        bool process (float* out, int n, std::int64_t frame0)
        {
            std::fill (out, out + n, 0.0f);
            voices.mix (out, n, frame0);
            const float fade = std::exp (-1.0f / (float) (0.03 * sr));
            const float makeup = dbToGain (makeupDb());
            for (int i = 0; i < n; ++i)
            {
                const std::int64_t f = frame0 + i;
                if (back >= 0 && f >= fadeAt && f < back) gain *= fade;
                else if (back >= 0 && f >= back)
                {
                    voices.clear();
                    std::fill (out + i, out + n, 0.0f);
                    back = -1;
                    gain = target;
                    break;
                }
                const float x = out[i] * gain;
                // the compressor follows the level of each sample
                const float over = ampToDb (std::abs (x)) - thresholdDb;
                dynamics::glideCut (cut, dynamics::cutDb (over, 1.0f - 1.0f / ratio, kneeDb), (float) (1.0 / sr), attackSec, releaseSec);
                out[i] = x * dbToGain (-cut) * makeup;
            }
            return ! voices.empty();
        }

    private:
        double sr = 48000;
        metronome::ClickVoices voices;
        std::map<std::pair<int, int>, std::shared_ptr<const std::vector<float>>> sounds;
        float cut = 0, gain = 0, target = 0;
        std::int64_t fadeAt = 0, back = -1;

        // One note's sound, made once: three seconds, each overtone dying away faster than the last
        std::shared_ptr<const std::vector<float>> soundOf (Sound s, int midi)
        {
            const auto key = std::make_pair ((int) s, midi);
            auto it = sounds.find (key);
            if (it != sounds.end()) return it->second;
            const bool piano = s == Sound::piano;
            auto buf = std::make_shared<const std::vector<float>> (renderPluck (
                music::midiToHz (midi), sr, 3.0,
                piano ? std::vector<double> { 1, 0.5, 0.32, 0.2, 0.12, 0.08, 0.05, 0.03 } : std::vector<double> { 1, 0.62, 0.42, 0.3, 0.2, 0.14, 0.09, 0.06 },
                piano ? 0.8 : 1.1, piano ? 1.4 : 0.9, 0.003, 0.3));
            sounds.emplace (key, buf);
            return buf;
        }
    };
}
