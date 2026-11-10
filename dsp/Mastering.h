#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include "MasterChain.h"

namespace vs
{
    // ENCORE.'s one way to master a whole song with vs::MasterChain.
    //
    // 1. Energies: the song's power per second (the sum over both channels of
    //    each one's mean square), to find its loudest 30 s.
    // 2. Learning: that stretch, looped, through a chain that is learning
    //    (level, tone, then the drive that hits the loudness target) until it
    //    stops learning, at most 90 s of audio, then it finishes with what it
    //    heard.
    // 3. Render: the whole song through a new chain with what was learned
    //    locked, the chain's latency taken back out (the master lines up with
    //    the original sample for sample), its integrated loudness measured as
    //    it goes. While that is 0.1 LU or more off the target the drive is
    //    corrected and the song rendered again: the first correction moves the
    //    drive by the difference, later ones by the difference over the slope
    //    measured between the last two renders (LU per dB, kept to 1/3 .. 2).
    //    It stops when on target, when the drive is held at -6 or +24 dB (the
    //    next render would be the same), or when a render comes out no closer
    //    than the best so far (the best is rendered once more, as the master),
    //    and after maxRenders renders at most.
    //
    // Each pass is fed block by block, so a song never has to fit in memory
    // at once: only the 30 s learning stretch is kept. A mono song is fed with
    // r null (it is mastered as both channels of a stereo pair).
    //
    //     Mastering m;
    //     m.begin (fs, settings, length);
    //     feed the song to feedEnergy();           (pass 1)
    //     auto w = m.learningWindow();
    //     feed samples [w.from, w.from + w.length) to feedWindow(); m.learn();
    //     do { m.beginRender(); feed the song to render(); m.finishRender(); }
    //     while (m.nextPass());                     (pass 3, keep the last output)
    class Mastering
    {
    public:
        static constexpr int block = 1024;
        static constexpr double windowSeconds = 30.0, maxLearnSeconds = 90.0;
        static constexpr float toleranceLu = 0.1f, minDriveDb = -6.0f, maxDriveDb = 24.0f;
        // LU of loudness per dB of drive that a correction assumes, at least and at most
        static constexpr double minSlope = 1.0 / 3.0, maxSlope = 2.0;
        // Every test song converges in 5 renders or fewer; 8 leaves room.
        static constexpr int maxRenders = 8;

        void begin (double sampleRate, const MasterChain::Settings& s, long long songLength)
        {
            fs = sampleRate;
            settings = s;
            len = songLength;
            sec = (int) fs;
            power.clear();
            secAcc[0] = secAcc[1] = 0.0;
            secCount = 0;
            windowL.clear(); windowR.clear();
            learnedValues = {};
            renders = 0;
            lastLufs = -100.0f;
            finalRender = false;
            energyDone = false;
        }

        // ---- pass 1: the whole song, in order
        void feedEnergy (const float* l, const float* r, int n)
        {
            const float* ch[2] { l, r != nullptr ? r : l };
            for (int i = 0; i < n; ++i)
            {
                for (int c = 0; c < 2; ++c) { const float x = ch[c][i]; secAcc[c] += x * x; }
                if (++secCount == sec) closeSecond();
            }
        }

        struct Window { long long from = 0, length = 0; };

        // After pass 1: where the loudest 30 s (or the whole song, if shorter) starts.
        Window learningWindow()
        {
            if (! energyDone) { if (secCount > 0) closeSecond(); energyDone = true; }
            const long long window = std::min (len, (long long) (int) (fs * windowSeconds));
            const int secs = (int) power.size();
            const int wSecs = (int) std::max (1LL, window / std::max (1, sec));
            int best = 0;
            double bestSum = -1, run = 0;
            for (int i = 0; i < secs; ++i)
            {
                run += power[(size_t) i];
                if (i >= wSecs) run -= power[(size_t) (i - wSecs)];
                if (i >= wSecs - 1 && run > bestSum) { bestSum = run; best = i - wSecs + 1; }
            }
            Window w;
            w.from = std::clamp ((long long) best * sec, 0LL, std::max (0LL, len - window));
            w.length = window;
            return w;
        }

        // ---- pass 2: the learning stretch's samples, in order
        void feedWindow (const float* l, const float* r, int n)
        {
            windowL.insert (windowL.end(), l, l + n);
            windowR.insert (windowR.end(), r != nullptr ? r : l, (r != nullptr ? r : l) + n);
        }

        // Runs the learning on the stretch fed (looped) and keeps what it learned.
        void learn()
        {
            const int window = (int) windowL.size();
            auto learner = std::make_unique<MasterChain>();
            learner->prepare (fs, block);
            learner->setSettings (settings);
            learner->startLearning();
            std::vector<float> tl ((size_t) block), tr ((size_t) block);
            const int loop = std::max (block, window - block);
            for (int done = 0; learner->isLearning() && done < (int) (fs * maxLearnSeconds); done += block)
            {
                const int at = done % loop;
                const int n = std::min (block, window - at);
                if (n <= 0) continue;
                std::copy (windowL.begin() + at, windowL.begin() + at + n, tl.begin());
                std::copy (windowR.begin() + at, windowR.begin() + at + n, tr.begin());
                learner->process (tl.data(), tr.data(), n);
            }
            learner->finishLearningNow();
            learnedValues = learner->getLearned();
            learnedValues.valid = true; // rendering never learns again, whatever the stretch held
            drive = learnedValues.driveDb;
            std::vector<float>().swap (windowL);
            std::vector<float>().swap (windowR);
        }

        const MasterChain::Learned& learned() const { return learnedValues; }

        // Masters with values learned elsewhere instead of passes 1 and 2.
        void setLearned (const MasterChain::Learned& l) { learnedValues = l; learnedValues.valid = true; drive = l.driveDb; }

        // ---- pass 3: the whole song, in order, after beginRender()
        void beginRender()
        {
            chain = std::make_unique<MasterChain>();
            chain->prepare (fs, block);
            chain->setSettings (settings);
            chain->lockLearned (learnedValues);
            latency = chain->getLatency();
            inL.assign ((size_t) block, 0.0f); inR.assign ((size_t) block, 0.0f);
            filled = 0;
            fed = 0;
            written = -latency;
            meter.prepare (fs);
            ++renders;
        }

        int getLatency() const { return chain != nullptr ? chain->getLatency() : 0; }

        // The most samples render() writes for n samples in, and finishRender() writes in all
        static int maxRenderOutput (int n) { return n + block; }
        int maxFinishOutput() const { return latency + 2 * block; }

        // Takes n samples of the song and writes the master's next samples to
        // outL / outR (in line with the song); returns how many.
        int render (const float* l, const float* r, int n, float* outL, float* outR)
        {
            const float* rr = r != nullptr ? r : l;
            int out = 0;
            for (int i = 0; i < n && fed < len; ++i, ++fed)
            {
                inL[(size_t) filled] = l[i];
                inR[(size_t) filled] = rr[i];
                if (++filled == block) out += processBlock (outL + out, outR + out);
            }
            return out;
        }

        // After the last render(): the rest of the master (silence fed past the
        // end until every sample of the song is out). Returns how many.
        int finishRender (float* outL, float* outR)
        {
            int out = 0;
            while (written < len)
            {
                std::fill (inL.begin() + filled, inL.end(), 0.0f);
                std::fill (inR.begin() + filled, inR.end(), 0.0f);
                filled = block;
                out += processBlock (outL + out, outR + out);
            }
            lastLufs = meter.integrated();
            chain = nullptr;
            return out;
        }

        // The integrated loudness of the last render, LUFS
        float renderedLufs() const { return lastLufs; }
        int rendersDone() const { return renders; }

        // After a render: true when the song must be rendered again (the
        // drive already moved for it), false when the last render is the master.
        bool nextPass()
        {
            const double err = (double) settings.targetLufs - (double) lastLufs;
            if (finalRender) return false;
            const bool closer = renders == 1 || std::abs (err) < std::abs (bestErr);
            if (closer) { bestErr = err; bestDrive = drive; }
            if (std::abs (err) < toleranceLu || renders >= maxRenders) return false;
            if (! closer)
            {
                // this render got no closer: render the best one again and stop there
                drive = bestDrive;
                learnedValues.driveDb = (float) drive;
                finalRender = true;
                return true;
            }
            double step = err;
            if (renders > 1 && drive != prevDrive)
            {
                const double slope = ((double) lastLufs - prevLufs) / (drive - prevDrive);
                step = err / std::clamp (slope, minSlope, maxSlope);
            }
            const double moved = std::max ((double) minDriveDb, std::min ((double) maxDriveDb, drive + step));
            if (moved == drive) return false; // held at a limit: another render would be the same
            prevDrive = drive;
            prevLufs = lastLufs;
            drive = moved;
            learnedValues.driveDb = (float) drive;
            return true;
        }

        // Every pass on a song held in memory (l, r of len samples, r may be
        // null), written to outL / outR. Returns the master's loudness.
        float masterWhole (const float* l, const float* r, float* outL, float* outR, int feedBlock = 65536)
        {
            for (long long s = 0; s < len; s += feedBlock)
                feedEnergy (l + s, r != nullptr ? r + s : nullptr, (int) std::min ((long long) feedBlock, len - s));
            const auto w = learningWindow();
            feedWindow (l + w.from, r != nullptr ? r + w.from : nullptr, (int) w.length);
            learn();
            do
            {
                beginRender();
                long long o = 0;
                for (long long s = 0; s < len; s += feedBlock)
                    o += render (l + s, r != nullptr ? r + s : nullptr, (int) std::min ((long long) feedBlock, len - s), outL + o, outR + o);
                finishRender (outL + o, outR + o);
            }
            while (nextPass());
            return lastLufs;
        }

    private:
        void closeSecond()
        {
            double p = 0.0;
            for (int c = 0; c < 2; ++c)
            {
                const float rms = (float) std::sqrt (secAcc[c] / secCount);
                p += (double) rms * rms;
                secAcc[c] = 0.0;
            }
            power.push_back (p);
            secCount = 0;
        }

        int processBlock (float* outL, float* outR)
        {
            chain->process (inL.data(), inR.data(), block);
            int out = 0;
            for (int i = 0; i < block; ++i, ++written)
                if (written >= 0 && written < len)
                {
                    outL[out] = inL[(size_t) i];
                    outR[out] = inR[(size_t) i];
                    ++out;
                }
            if (out > 0) meter.process (outL, outR, out);
            filled = 0;
            return out;
        }

        double fs = 48000.0;
        MasterChain::Settings settings;
        long long len = 0;
        int sec = 48000;
        std::vector<double> power;
        double secAcc[2] {};
        int secCount = 0;
        bool energyDone = false;
        std::vector<float> windowL, windowR, inL, inR;
        MasterChain::Learned learnedValues;
        std::unique_ptr<MasterChain> chain;
        LoudnessMeter meter;
        int latency = 0, filled = 0, renders = 0;
        long long fed = 0, written = 0;
        float lastLufs = -100.0f;
        double drive = 6.0; // the drive between renders, kept in double as it moves
        double prevDrive = 0.0, prevLufs = 0.0, bestDrive = 0.0, bestErr = 0.0;
        bool finalRender = false;
    };
}
