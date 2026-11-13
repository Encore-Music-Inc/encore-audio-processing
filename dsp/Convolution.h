#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include "Fft.h"
#include "Mix.h"
#include "Ramp.h"

namespace vs
{
    // A lock for handing things between the audio thread and one other
    // thread: the audio thread only ever tries it (and comes back next
    // block if it's taken); the other thread waits for it.
    class TryLock
    {
    public:
        bool tryLock() { return ! flag.test_and_set (std::memory_order_acquire); }
        void lock() { while (! tryLock()) std::this_thread::yield(); }
        void unlock() { flag.clear (std::memory_order_release); }

    private:
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
    };

    // Convolution with an impulse response (a long FIR filter) and no
    // latency: uniformly partitioned FFT convolution, where each block's
    // output is ready in the same block. The partitions follow the block
    // size: a power of two at least the largest block, with an FFT of twice
    // that (four times for blocks of 128 or less).
    //
    // A new impulse response is made ready (its partitions transformed, its
    // buffers allocated) on the thread that loads it, then handed to the
    // audio thread, which crossfades from the old one over 50 ms, so a change
    // doesn't click. The audio thread never allocates or frees: the response
    // it stops using is freed on the next load, prepare or destruction.
    //
    // Threads: prepare() and loadImpulseResponse() off the audio thread (one
    // thread loads at a time; a load may run while the audio thread
    // processes); process() and reset() on the audio thread. Until a response
    // is loaded the audio passes through unchanged. One or two channels are
    // convolved; any more are copies of the first.
    class Convolution
    {
    public:
        Convolution() = default;
        Convolution (const Convolution&) = delete;
        Convolution& operator= (const Convolution&) = delete;
        ~Convolution()
        {
            delete current;
            delete previous;
            delete retiring;
            delete pending.load();
            freeRetired();
        }

        // Allocates. Takes up the newest loaded response (with no crossfade)
        // and clears the history. maxBlockSize: the most samples process()
        // is given at once (it splits larger blocks).
        void prepare (double sampleRate, int maxBlockSize)
        {
            fs = sampleRate;
            maxBlock = std::max (1, maxBlockSize);
            preparedBlock.store (maxBlock);

            handOff.lock();
            Engine* taken = pending.exchange (nullptr);
            handOff.unlock();
            if (taken != nullptr)
            {
                delete current;
                current = taken;
            }
            delete previous;
            delete retiring;
            previous = retiring = nullptr;
            freeRetired();

            if (current != nullptr && (current->block != Engine::blockFor (maxBlock)))
            {
                auto* rebuilt = new Engine (current->ir, maxBlock);
                delete current;
                current = rebuilt;
            }
            if (current != nullptr) current->reset();

            fade.reset (fs, 0.05);
            fade.setCurrentAndTarget (1.0f);
            fadeGain.assign ((size_t) maxBlock, 0.0f);
            for (auto& m : mix) m.assign ((size_t) maxBlock, 0.0f);
        }

        // Allocates (off the audio thread). ir: one or two channels of equal
        // length; a channel past the last uses the last. Applied unscaled.
        void loadImpulseResponse (std::vector<std::vector<float>> ir)
        {
            if (ir.empty() || ir[0].empty()) ir = { { 1.0f } };
            auto* engine = new Engine (std::move (ir), preparedBlock.load());
            handOff.lock();
            Engine* replaced = pending.exchange (engine);
            handOff.unlock();
            delete replaced; // never seen by the audio thread
            freeRetired();
        }

        // Clears the history and ends a crossfade.
        void reset()
        {
            fade.setCurrentAndTarget (1.0f);
            if (current != nullptr) current->reset();
            retirePrevious();
        }

        int getLatency() const { return 0; }

        // In place
        void process (float* const* ch, int numCh, int numSamples)
        {
            for (int done = 0; done < numSamples;)
            {
                const int n = std::min (maxBlock, numSamples - done);
                float* part[maxChannels] {};
                const int convolved = std::min (numCh, maxChannels);
                for (int c = 0; c < convolved; ++c) part[c] = ch[c] + done;
                processBlock (part, convolved, n);
                for (int c = convolved; c < numCh; ++c)
                    std::copy (ch[0] + done, ch[0] + done + n, ch[c] + done);
                done += n;
            }
        }

    private:
        static constexpr int maxChannels = 2;

        // One impulse response, partitioned and transformed for one block
        // size, with each channel's history.
        struct Engine
        {
            static int blockFor (int maxBlockSize)
            {
                int b = 1;
                while (b < maxBlockSize) b <<= 1;
                return b;
            }

            Engine (std::vector<std::vector<float>> response, int maxBlockSize)
                : ir (std::move (response))
            {
                block = blockFor (maxBlockSize);
                fftSize = block > 128 ? 2 * block : 4 * block;
                segLen = fftSize - block;
                bins = fftSize / 2 + 1;
                const int len = (int) ir[0].size();
                numSegs = std::max (1, (len + segLen - 1) / segLen);
                stride = segLen / block; // input partitions per response partition
                numInSegs = numSegs * stride;
                fft.setSize (fftSize);

                // the response's partitions, transformed
                const int numIr = (int) ir.size();
                irRe.assign ((size_t) (numIr * numSegs * bins), 0.0);
                irIm.assign (irRe.size(), 0.0);
                std::vector<float> seg ((size_t) fftSize);
                for (int r = 0; r < numIr; ++r)
                    for (int s = 0; s < numSegs; ++s)
                    {
                        std::fill (seg.begin(), seg.end(), 0.0f);
                        const int from = s * segLen, count = std::min (segLen, len - from);
                        std::copy (ir[(size_t) r].begin() + from, ir[(size_t) r].begin() + from + count, seg.begin());
                        const size_t at = (size_t) ((r * numSegs + s) * bins);
                        fft.forward (seg.data(), irRe.data() + at, irIm.data() + at);
                    }

                for (int c = 0; c < maxChannels; ++c)
                {
                    auto& h = history[(size_t) c];
                    h.input.assign ((size_t) fftSize, 0.0f);
                    h.overlap.assign ((size_t) segLen, 0.0f);
                    h.segRe.assign ((size_t) (numInSegs * bins), 0.0);
                    h.segIm.assign (h.segRe.size(), 0.0);
                    h.accRe.assign ((size_t) bins, 0.0);
                    h.accIm.assign ((size_t) bins, 0.0);
                }
                sumRe.assign ((size_t) bins, 0.0);
                sumIm.assign ((size_t) bins, 0.0);
                out.assign ((size_t) fftSize, 0.0f);
            }

            void reset()
            {
                for (int c = 0; c < maxChannels; ++c)
                {
                    auto& h = history[(size_t) c];
                    std::fill (h.input.begin(), h.input.end(), 0.0f);
                    std::fill (h.overlap.begin(), h.overlap.end(), 0.0f);
                    std::fill (h.segRe.begin(), h.segRe.end(), 0.0);
                    std::fill (h.segIm.begin(), h.segIm.end(), 0.0);
                    h.current = h.pos = 0;
                }
            }

            // x: the input, y: the output (may be the same)
            void process (int c, const float* x, float* y, int numSamples)
            {
                auto& h = history[(size_t) c];
                const int r = std::min (c, (int) ir.size() - 1);
                const double* rRe = irRe.data() + (size_t) (r * numSegs * bins);
                const double* rIm = irIm.data() + (size_t) (r * numSegs * bins);

                for (int done = 0; done < numSamples;)
                {
                    const bool newBlock = h.pos == 0;
                    const int n = std::min (numSamples - done, block - h.pos);
                    std::copy (x + done, x + done + n, h.input.begin() + h.pos);

                    // this block's input so far, transformed
                    double* sRe = h.segRe.data() + (size_t) (h.current * bins);
                    double* sIm = h.segIm.data() + (size_t) (h.current * bins);
                    fft.forward (h.input.data(), sRe, sIm);

                    // the older input against the rest of the response: once per block
                    if (newBlock)
                    {
                        std::fill (h.accRe.begin(), h.accRe.end(), 0.0);
                        std::fill (h.accIm.begin(), h.accIm.end(), 0.0);
                        int index = h.current;
                        for (int s = 1; s < numSegs; ++s)
                        {
                            index += stride;
                            if (index >= numInSegs) index -= numInSegs;
                            multiplyAdd (h.segRe.data() + (size_t) (index * bins), h.segIm.data() + (size_t) (index * bins),
                                         rRe + (size_t) (s * bins), rIm + (size_t) (s * bins), h.accRe.data(), h.accIm.data());
                        }
                    }

                    std::copy (h.accRe.begin(), h.accRe.end(), sumRe.begin());
                    std::copy (h.accIm.begin(), h.accIm.end(), sumIm.begin());
                    multiplyAdd (sRe, sIm, rRe, rIm, sumRe.data(), sumIm.data());
                    fft.inverse (sumRe.data(), sumIm.data(), out.data());

                    for (int i = 0; i < n; ++i)
                        y[done + i] = out[(size_t) (h.pos + i)] + h.overlap[(size_t) (h.pos + i)];

                    h.pos += n;
                    if (h.pos == block)
                    {
                        std::fill (h.input.begin(), h.input.end(), 0.0f);
                        h.pos = 0;
                        // what reaches past this block, for the next ones
                        for (int i = 0; i < segLen; ++i)
                            h.overlap[(size_t) i] = out[(size_t) (block + i)] + (i + block < segLen ? h.overlap[(size_t) (i + block)] : 0.0f);
                        h.current = h.current > 0 ? h.current - 1 : numInSegs - 1;
                    }
                    done += n;
                }
            }

            void multiplyAdd (const double* aRe, const double* aIm, const double* bRe, const double* bIm, double* re, double* im) const
            {
                for (int k = 0; k < bins; ++k)
                {
                    re[k] += aRe[k] * bRe[k] - aIm[k] * bIm[k];
                    im[k] += aRe[k] * bIm[k] + aIm[k] * bRe[k];
                }
            }

            struct History
            {
                std::vector<float> input, overlap;
                std::vector<double> segRe, segIm, accRe, accIm;
                int current = 0, pos = 0;
            };

            std::vector<std::vector<float>> ir;
            int block = 128, fftSize = 512, segLen = 384, bins = 257, numSegs = 1, stride = 1, numInSegs = 1;
            RealFft fft;
            std::vector<double> irRe, irIm, sumRe, sumIm;
            std::vector<float> out;
            History history[maxChannels];
            Engine* nextRetired = nullptr;
        };

        void processBlock (float* const* ch, int numCh, int n)
        {
            if (previous == nullptr && retiring == nullptr && handOff.tryLock())
            {
                Engine* incoming = pending.exchange (nullptr);
                handOff.unlock();
                if (incoming != nullptr)
                {
                    previous = current;
                    current = incoming;
                    fade.setCurrentAndTarget (1.0f);
                    fade.setTarget (0.0f);
                }
            }

            if (fade.isRamping())
            {
                // previous * g + current * (1 - g), g falling from 1 to 0
                for (int i = 0; i < n; ++i) fadeGain[(size_t) i] = fade.next();
                for (int c = 0; c < numCh; ++c)
                {
                    float* m = mix[(size_t) c].data();
                    if (previous != nullptr) previous->process (c, ch[c], m, n);
                    else std::copy (ch[c], ch[c] + n, m);
                    run (c, ch[c], n);
                    mix::blend (ch[c], m, fadeGain.data(), n);
                }
                if (! fade.isRamping()) retirePrevious();
            }
            else
            {
                for (int c = 0; c < numCh; ++c) run (c, ch[c], n);
            }
            if (retiring != nullptr) tryRetire();
        }

        void run (int c, float* x, int n)
        {
            if (current != nullptr) current->process (c, x, x, n);
        }

        // Audio thread: the old response goes on the list to be freed off the audio thread
        void retirePrevious()
        {
            if (previous == nullptr) return;
            retiring = previous;
            previous = nullptr;
            tryRetire();
        }

        void tryRetire()
        {
            if (! handOff.tryLock()) return;
            retiring->nextRetired = retired;
            retired = retiring;
            handOff.unlock();
            retiring = nullptr;
        }

        // Off the audio thread
        void freeRetired()
        {
            handOff.lock();
            Engine* list = retired;
            retired = nullptr;
            handOff.unlock();
            while (list != nullptr)
            {
                Engine* next = list->nextRetired;
                delete list;
                list = next;
            }
        }

        double fs = 48000.0;
        int maxBlock = 512;
        std::atomic<int> preparedBlock { 128 };
        Engine* current = nullptr;   // audio thread (and prepare)
        Engine* previous = nullptr;  // audio thread: fading out
        Engine* retiring = nullptr;  // audio thread: waiting for the lock to go on the list
        Engine* retired = nullptr;   // the list, under handOff
        std::atomic<Engine*> pending { nullptr }; // under handOff
        TryLock handOff;
        Ramp<false> fade;
        std::vector<float> fadeGain;
        std::vector<float> mix[maxChannels];
    };
}
