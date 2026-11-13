#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "Util.h"

namespace vs
{
    // How a level meter moves on screen, read a frame at a time (each
    // product's own frame rate): the numbers a meter shows, never the
    // measuring itself (that is the loudness, true-peak and peak code).
    namespace meters
    {
        // A meter that eases toward each new reading: by `rise` of the
        // difference when the reading is above it, `fall` when below
        // (vs::followEnvelope, once per frame)
        struct Ballistics { float rise, fall; };

        // ENCORE.'s meters, one setting each
        inline constexpr Ballistics level { 0.6f, 0.12f };          // input, output, wet and de-esser/compressor amount
        inline constexpr Ballistics gainReduction { 0.6f, 0.08f };  // a limiter's gain reduction
        inline constexpr Ballistics steady { 0.3f, 0.3f };          // a slow, even move (the vocal rider)

        inline void step (float& shown, float reading, const Ballistics& b) { followEnvelope (shown, reading, b.rise, b.fall); }

        // A peak meter in dB: jumps up to a reading, falls back at most
        // fallDbPerFrame, holds its peak for holdMs, and latches a clip
        // light at clipDb and above until reset
        struct PeakSettings { float fallDbPerFrame = 0.7f, holdMs = 1500.0f, clipDb = -0.1f, floorDb = -100.0f; };

        class PeakMeter
        {
        public:
            explicit PeakMeter (PeakSettings s = {}) : set (s) { reset(); }

            // One frame's reading at time nowMs; true when what the meter shows changed
            bool step (float readingDb, std::uint32_t nowMs)
            {
                bool changed = false;
                const float next = std::max (readingDb, levelDb - set.fallDbPerFrame);
                if (std::abs (next - levelDb) > 0.05f) { levelDb = next; changed = true; }
                if (readingDb > holdDb || nowMs - holdAt > (std::uint32_t) set.holdMs) { holdDb = readingDb; holdAt = nowMs; changed = true; }
                if (readingDb >= set.clipDb && ! clipped) { clipped = true; changed = true; }
                return changed;
            }

            // No signal: the meter drops to the floor; true when it moved
            bool silence()
            {
                if (levelDb <= set.floorDb) return false;
                levelDb = set.floorDb;
                return true;
            }

            void reset() { levelDb = holdDb = set.floorDb; holdAt = 0; clipped = false; }
            void clearClip() { clipped = false; }

            float level() const { return levelDb; }
            float hold() const { return holdDb; }
            bool isClipped() const { return clipped; }

        private:
            PeakSettings set;
            float levelDb = -100.0f, holdDb = -100.0f;
            std::uint32_t holdAt = 0;
            bool clipped = false;
        };

        // Where a level sits on a meter's scale, 0 (bottom) .. 1 (top), for a
        // scale from bottomDb to topDb with more room near the top
        inline float scalePosition (float db, float bottomDb = -60.0f, float topDb = 6.0f, float curve = 1.6f)
        {
            if (db <= bottomDb) return 0.0f;
            const float x = std::clamp ((db - bottomDb) / (topDb - bottomDb), 0.0f, 1.0f);
            return std::pow (x, curve);
        }

        // The volume fader: its position (0 at the bottom .. 1 at the top, +6 dB)
        // and the level it sets, 20 dB for every factor of e in position, and
        // silence (faderFloorDb) at the bottom. Songs store a fader's position,
        // so this law is how a saved level is read back.
        inline constexpr float faderTopDb = 6.0f, faderFloorDb = -100.0f;

        inline float faderPositionToDb (float position)
        {
            return position > 0.0f ? 20.0f * std::log (position) + faderTopDb : faderFloorDb;
        }

        inline float dbToFaderPosition (float db)
        {
            return db > faderFloorDb ? std::exp ((db - faderTopDb) * (1.0f / 20.0f)) : 0.0f;
        }

        // A level in dB as a meter reads it: silence (faderFloorDb) for no signal.
        inline float levelDb (float gain)
        {
            return gain > 0.0f ? ampToDb (gain, 0.0f) : faderFloorDb;
        }

        // A sample over this gain lights the overload light.
        inline constexpr float overloadGain = 0.999f;

        // The highest level since a meter last read it, with when it came (ms).
        // A reading replaces the held one when it is at least as loud (so a
        // later equal reading moves the time, and a NaN never replaces it);
        // taking it leaves silence at the same time, for the next block.
        struct Reading { std::uint32_t timeMs = 0; float db = faderFloorDb; };

        inline void keepHighest (Reading& held, Reading r)
        {
            if (r.db >= held.db) held = r;
        }

        inline Reading takeHighest (Reading& held)
        {
            const Reading r = held;
            held.db = faderFloorDb;
            return r;
        }

        inline float faderPositionToGain (float position)
        {
            return position > 0.0f ? std::pow (10.0f, (20.0f * std::log (position) + faderTopDb) * (1.0f / 20.0f)) : 0.0f;
        }

        inline float gainToFaderPosition (float gain)
        {
            return gain > 0.0f ? std::exp ((20.0f * std::log10 (gain) - faderTopDb) * (1.0f / 20.0f)) : 0.0f;
        }
    }
}
