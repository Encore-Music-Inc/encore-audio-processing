// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// Following a singing voice and measuring what it did, in numbers:
//
//   VoiceTracker   reads a voice frame by frame (vs::Yin with Yin::voice()),
//                  gates out silence and breath, holds back a sudden octave jump
//                  until it lasts, and smooths with a median of three.
//   voice::...     what a pitch track (frames of time and MIDI note) shows: its
//                  runs of singing, range and tessitura, a held note's drift
//                  and vibrato, the notes sung, and the scores of the singing
//                  exercises (matching a note, a pattern, a siren, a tune sung
//                  back).
//   NoteFinder     turns a stream of nearest-note readings from an instrument
//                  into played notes; timingReport measures them against a click.
//
// Only numbers come out; the words shown to people are the product's. Plain
// C++ with no JUCE, so the browser (master.wasm) measures the same way.
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "Music.h"
#include "Stats.h"
#include "Yin.h"
#include "Util.h"

namespace vs
{
    namespace voice
    {
        // Seconds of sound per pitch reading.
        inline constexpr double trackWindow = 0.045;

        using vs::roundHalfUp;

        inline double clampTo (double v, double lo, double hi) { return std::max (lo, std::min (hi, v)); }

        // The middle value (the mean of the two middle ones for an even count); NaN when empty.
        inline double median (const std::vector<double>& a)
        {
            return a.empty() ? std::numeric_limits<double>::quiet_NaN() : stats::median (a.data(), (int) a.size());
        }

        inline double mean (const std::vector<double>& a) { return stats::mean (a.data(), (int) a.size()); }
        inline double sd (const std::vector<double>& a) { return stats::standardDeviation (a.data(), (int) a.size()); }

        // A pitch reading at a moment: midi is NaN when there's no voice.
        struct PitchFrame
        {
            double t = 0.0;
            double midi = std::numeric_limits<double>::quiet_NaN();
            bool voiced() const { return ! std::isnan (midi); }
        };

        using Run = std::vector<PitchFrame>;

        // Runs of voice: a new one starts after a gap longer than maxGap seconds or
        // a jump bigger than maxStep semitones.
        inline std::vector<Run> voicedRuns (const std::vector<PitchFrame>& frames, double maxGap = 0.12, double maxStep = 1.5)
        {
            std::vector<Run> runs;
            for (const auto& f : frames)
            {
                if (! f.voiced()) continue;
                if (runs.empty() || f.t - runs.back().back().t > maxGap || std::abs (f.midi - runs.back().back().midi) > maxStep)
                    runs.emplace_back();
                runs.back().push_back (f);
            }
            return runs;
        }

        inline double runSeconds (const Run& run) { return run.empty() ? 0.0 : run.back().t - run.front().t; }

        // Each frame's pitch replaced by the median of the five around it.
        inline Run median5 (const Run& run)
        {
            Run out;
            out.reserve (run.size());
            for (size_t i = 0; i < run.size(); ++i)
            {
                std::vector<double> m;
                for (size_t j = i >= 2 ? i - 2 : 0; j < std::min (run.size(), i + 3); ++j) m.push_back (run[j].midi);
                out.push_back ({ run[i].t, median (m) });
            }
            return out;
        }

        // A pitch and how long it stands for, in seconds.
        struct WeightedPitch { double midi = 0.0, w = 0.0; };

        // Frames that hold a pitch, each with how long it stands for: the 0.2 s
        // before and the 0.2 s after sit within 30 cents of each other (a slide
        // fails that, a vibrato around one note doesn't).
        inline std::vector<WeightedPitch> steadyFrames (const std::vector<PitchFrame>& frames)
        {
            std::vector<WeightedPitch> out;
            for (const auto& run : voicedRuns (frames))
            {
                if (runSeconds (run) < 0.25) continue;
                for (size_t i = 0; i < run.size(); ++i)
                {
                    const auto& f = run[i];
                    std::vector<double> before, after;
                    for (long j = (long) i; j >= 0 && f.t - run[(size_t) j].t <= 0.2; --j) before.push_back (run[(size_t) j].midi);
                    for (size_t j = i + 1; j < run.size() && run[j].t - f.t <= 0.2; ++j) after.push_back (run[j].midi);
                    const double dt = i + 1 < run.size() ? std::min (0.05, run[i + 1].t - f.t) : 0.0;
                    if (before.size() >= 3 && after.size() >= 3 && std::abs (mean (before) - mean (after)) < 0.3)
                    {
                        auto both = before;
                        both.insert (both.end(), after.begin(), after.end());
                        if (sd (both) < 0.6) out.push_back ({ f.midi, dt });
                    }
                }
            }
            return out;
        }

        // The pitch below which a share p (0..1) of the time is spent; NaN when empty.
        inline double weightedPercentile (std::vector<WeightedPitch> items, double p)
        {
            std::stable_sort (items.begin(), items.end(), [] (const WeightedPitch& a, const WeightedPitch& b) { return a.midi < b.midi; });
            double total = 0.0;
            for (const auto& i : items) total += i.w;
            double acc = 0.0;
            for (const auto& i : items)
            {
                acc += i.w;
                if (acc >= p * total) return i.midi;
            }
            return items.empty() ? std::numeric_limits<double>::quiet_NaN() : items.back().midi;
        }

        // Where a voice sits comfortably, in whole notes, and how many seconds of steady singing showed it.
        struct Tessitura { double lo = 0.0, hi = 0.0, seconds = 0.0; };

        // The middle 70% of the time spent on steady notes; nothing with under 3 seconds of them.
        inline std::optional<Tessitura> tessitura (const std::vector<PitchFrame>& frames)
        {
            const auto steady = steadyFrames (frames);
            double seconds = 0.0;
            for (const auto& s : steady) seconds += s.w;
            if (seconds < 3) return std::nullopt;
            return Tessitura { roundHalfUp (weightedPercentile (steady, 0.15)), roundHalfUp (weightedPercentile (steady, 0.85)), seconds };
        }

        // A range test's result (MIDI notes).
        struct RangeResult
        {
            double low = 0.0, high = 0.0;           // the lowest and highest notes reached, whole notes
            double lowExact = 0.0, highExact = 0.0;
            double tessLo = 0.0, tessHi = 0.0;      // the comfortable middle
            bool tessMeasured = false;              // from comfortable singing (else a rule of thumb)
        };

        // The lowest and highest notes reached (slides and sirens count; a blip under a
        // quarter of a second doesn't), and the comfortable middle: from tessFrames
        // (singing done comfortably) when there's enough of it, else from about a
        // third above the bottom to a fourth below the top. Nothing with under ten
        // frames in runs of a quarter second or more.
        inline std::optional<RangeResult> analyzeRange (const std::vector<PitchFrame>& frames, const std::vector<PitchFrame>* tessFrames = nullptr)
        {
            Run kept;
            for (const auto& r : voicedRuns (frames))
                if (runSeconds (r) >= 0.25)
                    for (const auto& f : median5 (r)) kept.push_back (f);
            if (kept.size() < 10) return std::nullopt;
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            for (const auto& f : kept) { lo = std::min (lo, f.midi); hi = std::max (hi, f.midi); }
            RangeResult r;
            r.low = roundHalfUp (lo);
            r.high = roundHalfUp (hi);
            r.lowExact = lo;
            r.highExact = hi;
            const auto sung = tessFrames ? tessitura (*tessFrames) : std::nullopt;
            r.tessMeasured = sung.has_value();
            if (sung)
            {
                r.tessLo = clampTo (sung->lo, r.low, r.high);
                r.tessHi = clampTo (sung->hi, r.low, r.high);
            }
            else
            {
                const double a = r.low + 3, b = r.high - 4;
                if (a <= b) { r.tessLo = a; r.tessHi = b; }
                else { r.tessLo = std::floor ((r.low + r.high) / 2); r.tessHi = std::ceil ((r.low + r.high) / 2); }
            }
            return r;
        }

        // Cents folded to the nearest octave (a note an octave or two out still
        // counts as the note): what's left, and how many octaves.
        struct Folded { double cents = 0.0, octaves = 0.0; };
        inline Folded foldOctave (double cents)
        {
            const double octaves = roundHalfUp (cents / 1200);
            return { cents - octaves * 1200, octaves };
        }

        // Moving average over n points, centred (shorter at the ends).
        inline std::vector<double> movingAverage (const std::vector<double>& a, int n)
        {
            if (n <= 1) return a;
            std::vector<double> out (a.size());
            const long h = n >> 1, len = (long) a.size();
            for (long i = 0; i < len; ++i)
            {
                double s = 0.0;
                int c = 0;
                for (long j = std::max (0L, i - h); j <= std::min (len - 1, i + h); ++j) { s += a[(size_t) j]; ++c; }
                out[(size_t) i] = s / c;
            }
            return out;
        }

        // A run of voiced frames at an even `rate` per second, interpolated in a straight line between frames.
        inline std::vector<double> resampleRun (const Run& run, double rate = 100)
        {
            const double t0 = run.front().t, t1 = run.back().t;
            const size_t n = (size_t) std::floor ((t1 - t0) * rate) + 1;
            std::vector<double> values (n);
            size_t j = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const double t = t0 + (double) i / rate;
                while (j + 1 < run.size() - 1 && run[j + 1].t <= t) ++j;
                const auto& a = run[j];
                const auto& b = run[std::min (j + 1, run.size() - 1)];
                values[i] = b.t > a.t ? a.midi + (b.midi - a.midi) * clampTo ((t - a.t) / (b.t - a.t), 0, 1) : a.midi;
            }
            return values;
        }

        inline std::vector<PitchFrame> voicedOnly (const std::vector<PitchFrame>& frames)
        {
            std::vector<PitchFrame> v;
            for (const auto& f : frames) if (f.voiced()) v.push_back (f);
            return v;
        }

        // One try at matching a note.
        struct MatchResult
        {
            bool heard = false;
            double cents = 0.0;         // how far off once settled (median), octave folded
            std::optional<double> settle; // seconds from the first sound until it stayed within 50 cents for a quarter second
            double steadiness = 0.0;    // the spread of the held pitch in cents, a vibrato smoothed out first
            double octaves = 0.0;
            double sungMidi = 0.0;
            double score = 0.0;         // 0 to 100
        };

        // frames: from when the singing window opened. Not heard with under 8
        // voiced frames or a quarter of a second of them.
        inline MatchResult scoreMatch (const std::vector<PitchFrame>& frames, double target)
        {
            MatchResult r;
            const auto voiced = voicedOnly (frames);
            if (voiced.size() < 8 || voiced.back().t - voiced.front().t < 0.25) return r;
            std::vector<double> m;
            for (const auto& f : voiced) m.push_back (f.midi);
            const double octaves = foldOctave ((median (m) - target) * 100).octaves;
            const double goal = target + 12 * octaves;
            const double onset = voiced.front().t;
            std::optional<double> settleAt;
            for (size_t i = 0; i < voiced.size() && ! settleAt; ++i)
            {
                const double t0 = voiced[i].t;
                size_t j = i;
                bool ok = true;
                for (; j < voiced.size() && voiced[j].t - t0 <= 0.25; ++j)
                    if (std::abs (voiced[j].midi - goal) > 0.5) { ok = false; break; }
                if (ok && j < voiced.size() && voiced[j - 1].t - t0 >= 0.2) settleAt = t0;
            }
            const double from = settleAt ? *settleAt : onset + 0.3;
            Run held;
            for (const auto& f : voiced) if (f.t >= from) held.push_back (f);
            if (held.size() < 5) held = voiced;
            std::vector<double> c;
            for (const auto& f : held) c.push_back ((f.midi - goal) * 100);
            const double heldSeconds = runSeconds (held);
            const double perSecond = (double) held.size() / (heldSeconds != 0.0 ? heldSeconds : 1.0);
            r.heard = true;
            r.cents = median (c);
            r.steadiness = sd (movingAverage (c, (int) std::max (3.0, roundHalfUp (perSecond * 0.25))));
            if (settleAt) r.settle = *settleAt - onset;
            r.octaves = octaves;
            r.sungMidi = goal + r.cents / 100;
            const double acc = clampTo (1 - (std::abs (r.cents) - 10) / 60, 0, 1);
            const double steady = clampTo (1 - (r.steadiness - 8) / 40, 0, 1);
            const double speed = r.settle ? clampTo (1 - (*r.settle - 0.25) / 1.5, 0, 1) : 0.0;
            r.score = roundHalfUp (100 * (0.6 * acc + 0.2 * steady + 0.2 * speed));
            return r;
        }

        struct Vibrato { double rate = 0.0, width = 0.0, regularity = 0.0; }; // Hz, cents peak to peak, 0 to 1

        // What a held note did.
        struct SustainResult
        {
            double seconds = 0.0, meanMidi = 0.0;
            double drift = 0.0;                 // cents, start to end
            double wobble = 0.0;                // cents, slow unsteadiness once any vibrato is set aside
            std::optional<Vibrato> vibrato;
            std::optional<double> cents;        // vs the target, octave folded
            double octaves = 0.0;
        };

        // The longest run of a held note, its first 0.35 s (the onset) and last
        // 0.15 s left out, on an even 100 readings a second: its drift (a straight
        // line through it), its vibrato (a regular wave 3.5 to 9 times a second,
        // found by autocorrelation once the slow wander is taken out; its width put
        // back to what was sung, since the reading window and taking out the wander
        // shrink it), and the slow wobble left. Nothing under 1.5 s or 10 frames.
        inline std::optional<SustainResult> analyzeSustain (const std::vector<PitchFrame>& frames, std::optional<double> target = std::nullopt)
        {
            auto runs = voicedRuns (frames, 0.12, 2);
            std::stable_sort (runs.begin(), runs.end(), [] (const Run& a, const Run& b) { return runSeconds (a) > runSeconds (b); });
            if (runs.empty()) return std::nullopt;
            Run run;
            const double from = runs[0].front().t + 0.35, to = runs[0].back().t - 0.15;
            for (const auto& f : runs[0]) if (f.t >= from && f.t <= to) run.push_back (f);
            if (run.size() < 10 || runSeconds (run) < 1.5) return std::nullopt;
            const double rate = 100;
            const auto values = resampleRun (run, rate);
            const size_t n = values.size();
            const double meanMidi = mean (values);
            std::vector<double> c (n);
            for (size_t i = 0; i < n; ++i) c[i] = (values[i] - meanMidi) * 100;
            std::vector<double> xs (n);
            for (size_t i = 0; i < n; ++i) xs[i] = (double) i;
            const auto line = stats::fitLine (xs.data(), c.data(), (int) n);
            const double xm = line.meanX, slope = line.slope;
            const double drift = slope * (double) (n - 1);
            std::vector<double> r (n);
            for (size_t i = 0; i < n; ++i) r[i] = c[i] - slope * ((double) i - xm);

            const auto slow = movingAverage (r, (int) roundHalfUp (0.4 * rate));
            std::vector<double> hp (n);
            for (size_t i = 0; i < n; ++i) hp[i] = r[i] - slow[i];
            std::optional<Vibrato> vibrato;
            double energy = 0.0;
            for (double v : hp) energy += v * v;
            if (energy > 0)
            {
                auto ac = [&] (int lag) {
                    double s = 0.0;
                    for (size_t i = 0; i + (size_t) lag < n; ++i) s += hp[i] * hp[i + (size_t) lag];
                    return ((s / energy) * (double) n) / (double) ((long) n - lag);
                };
                const int lo = (int) std::floor (rate / 9), hi = (int) std::ceil (rate / 3.5);
                std::vector<double> acs ((size_t) hi + 2);
                for (int lag = lo - 1; lag <= hi + 1; ++lag) acs[(size_t) lag] = ac (lag);
                int best = -1;
                double bestV = -std::numeric_limits<double>::infinity();
                for (int lag = lo; lag <= hi; ++lag)
                    if (acs[(size_t) lag] > bestV && acs[(size_t) lag] >= acs[(size_t) lag - 1] && acs[(size_t) lag] >= acs[(size_t) lag + 1])
                    {
                        bestV = acs[(size_t) lag];
                        best = lag;
                    }
                const double width = 2 * std::sqrt (2.0) * sd (hp);
                if (best > 0 && bestV > 0.3 && width >= 15)
                {
                    const double a = acs[(size_t) best - 1], b = acs[(size_t) best], cc = acs[(size_t) best + 1];
                    const double lag = best + stats::parabolicOffset (a, b, cc, 1e-9);
                    const double hz = rate / lag;
                    const double N = 2 * ((int) roundHalfUp (0.4 * rate) >> 1) + 1;
                    const double pi = 3.141592653589793;
                    const double x = (pi * hz) / rate;
                    const double y = pi * hz * trackWindow;
                    const double kept = (std::sin (y) / y) * (1 - std::sin (N * x) / (N * std::sin (x)));
                    vibrato = Vibrato { hz, width / clampTo (kept, 0.5, 1.2), clampTo (bestV, 0, 1) };
                }
            }
            SustainResult res;
            res.seconds = (double) n / rate;
            res.meanMidi = meanMidi;
            res.drift = drift;
            res.wobble = sd (movingAverage (r, vibrato ? (int) std::max (3.0, roundHalfUp (rate / vibrato->rate)) : (int) roundHalfUp (0.1 * rate)));
            res.vibrato = vibrato;
            if (target)
            {
                const auto f = foldOctave ((meanMidi - *target) * 100);
                res.cents = f.cents;
                res.octaves = f.octaves;
            }
            return res;
        }

        // One sung note: when it started and ended (seconds), and its pitch.
        struct NoteSegment { double start = 0.0, end = 0.0, midi = 0.0; };

        // Splits singing into notes: steady stretches (spread under 30 cents across
        // 0.12 s) joined when they're the same note (within 0.6 semitone, 0.25 s
        // apart), slides between notes left out, notes shorter than minDur dropped.
        inline std::vector<NoteSegment> segmentNotes (const std::vector<PitchFrame>& frames, double minDur = 0.15)
        {
            struct Group { double start, end; std::vector<double> m; };
            std::vector<Group> groups;
            for (const auto& run : voicedRuns (frames, 0.12, 3))
            {
                long g = -1;
                for (size_t i = 0; i < run.size(); ++i)
                {
                    const auto& f = run[i];
                    std::vector<double> near;
                    for (long j = (long) i; j >= 0 && f.t - run[(size_t) j].t <= 0.06; --j) near.push_back (run[(size_t) j].midi);
                    for (size_t j = i + 1; j < run.size() && run[j].t - f.t <= 0.06; ++j) near.push_back (run[j].midi);
                    const bool steady = near.size() >= 3 && sd (near) < 0.3;
                    if (! steady) { g = -1; continue; }
                    if (g >= 0 && std::abs (f.midi - median (groups[(size_t) g].m)) < 0.6 && f.t - groups[(size_t) g].end < 0.15)
                    {
                        groups[(size_t) g].m.push_back (f.midi);
                        groups[(size_t) g].end = f.t;
                    }
                    else
                    {
                        groups.push_back ({ f.t, f.t, { f.midi } });
                        g = (long) groups.size() - 1;
                    }
                }
            }
            std::vector<Group> joined;
            for (const auto& g : groups)
            {
                if (! joined.empty() && std::abs (median (joined.back().m) - median (g.m)) < 0.6 && g.start - joined.back().end < 0.25)
                {
                    joined.back().m.insert (joined.back().m.end(), g.m.begin(), g.m.end());
                    joined.back().end = g.end;
                }
                else joined.push_back (g);
            }
            std::vector<NoteSegment> out;
            for (const auto& g : joined)
                if (g.end - g.start >= minDur) out.push_back ({ g.start, g.end, median (g.m) });
            return out;
        }

        // A tune's note against what was sung, or a jump between two notes: what
        // was asked for, what was sung, and the cents off (sung and cents empty: not heard).
        struct SingBackItem { double target = 0.0; std::optional<double> sung, cents; };

        struct SingBackResult
        {
            std::vector<SingBackItem> notes, jumps; // jumps: semitones between neighbouring notes
            double octaves = 0.0;
            double score = 0.0;                     // 0 to 100
        };

        // Singing back a short tune: each note's cents, and each jump between
        // notes (an interval sung right in another octave or key still counts for
        // the jump). Extra sung notes are joined to a near-equal neighbour, or the
        // shortest is dropped, until there are as many as asked for.
        inline SingBackResult scoreSingBack (const std::vector<PitchFrame>& frames, const std::vector<double>& targets)
        {
            auto segs = segmentNotes (frames);
            while (segs.size() > targets.size())
            {
                long bi = -1;
                double bd = std::numeric_limits<double>::infinity();
                for (size_t i = 0; i + 1 < segs.size(); ++i)
                {
                    const double dd = std::abs (segs[i].midi - segs[i + 1].midi);
                    if (dd < bd) { bd = dd; bi = (long) i; }
                }
                if (bd < 0.8)
                {
                    const auto a = segs[(size_t) bi], b = segs[(size_t) bi + 1];
                    const double wa = a.end - a.start, wb = b.end - b.start;
                    const double wsum = wa + wb;
                    segs.erase (segs.begin() + bi, segs.begin() + bi + 2);
                    segs.insert (segs.begin() + bi, { a.start, b.end, (a.midi * wa + b.midi * wb) / (wsum != 0.0 ? wsum : 1.0) });
                }
                else
                {
                    size_t si = 0;
                    for (size_t i = 0; i < segs.size(); ++i)
                        if (segs[i].end - segs[i].start < segs[si].end - segs[si].start) si = i;
                    segs.erase (segs.begin() + (long) si);
                }
            }
            SingBackResult res;
            res.octaves = segs.empty() ? 0.0 : foldOctave ((segs[0].midi - targets[0]) * 100).octaves;
            for (size_t i = 0; i < targets.size(); ++i)
            {
                SingBackItem n { targets[i], std::nullopt, std::nullopt };
                if (i < segs.size()) { n.sung = segs[i].midi; n.cents = (segs[i].midi - targets[i] - 12 * res.octaves) * 100; }
                res.notes.push_back (n);
            }
            for (size_t i = 1; i < targets.size(); ++i)
            {
                const auto& a = res.notes[i - 1];
                const auto& b = res.notes[i];
                SingBackItem j { targets[i] - targets[i - 1], std::nullopt, std::nullopt };
                if (a.sung && b.sung)
                {
                    j.sung = *b.sung - *a.sung;
                    j.cents = (*b.sung - *a.sung - (targets[i] - targets[i - 1])) * 100;
                }
                res.jumps.push_back (j);
            }
            auto points = [] (const std::vector<SingBackItem>& items) {
                std::vector<double> p;
                for (const auto& i : items) p.push_back (i.cents ? clampTo (1 - (std::abs (*i.cents) - 15) / 85, 0, 1) : 0.0);
                return p;
            };
            const auto pts = points (res.notes), jpts = points (res.jumps);
            res.score = roundHalfUp (100 * (0.5 * mean (pts) + 0.5 * (jpts.empty() ? mean (pts) : mean (jpts))));
            return res;
        }

        // A note of an exercise pattern: MIDI note, start and length in seconds.
        struct PatternNote { double midi = 0.0, at = 0.0, dur = 0.0; };

        // A pattern note against what was sung (sung and cents empty: not heard).
        struct ScoredNote { double midi = 0.0; std::optional<double> sung, cents; double octaves = 0.0; };

        // Each note of a pattern against what was sung: the median over the middle
        // of each note's time (the first quarter, at most 0.15 s, is the move onto
        // it; the last 30 ms is left out), octave folded. frames: times from the
        // pattern's start. Not heard with under 3 frames.
        inline std::vector<ScoredNote> scoreNotes (const std::vector<PitchFrame>& frames, const std::vector<PatternNote>& notes)
        {
            std::vector<ScoredNote> out;
            for (const auto& n : notes)
            {
                const double from = n.at + std::min (0.15, n.dur * 0.25), to = n.at + n.dur - 0.03;
                std::vector<double> got;
                for (const auto& f : frames) if (f.voiced() && f.t >= from && f.t <= to) got.push_back (f.midi);
                ScoredNote s { n.midi, std::nullopt, std::nullopt, 0.0 };
                if (got.size() >= 3)
                {
                    const double sung = median (got);
                    const auto fo = foldOctave ((sung - n.midi) * 100);
                    s.sung = sung;
                    s.cents = fo.cents;
                    s.octaves = fo.octaves;
                }
                out.push_back (s);
            }
            return out;
        }

        struct PatternScore { std::vector<ScoredNote> notes; double lag = 0.0; };

        // scoreNotes, forgiving a singer who comes in up to 0.1 s early or 0.3 s
        // late: of the timings tried every 25 ms, the one that hears the most notes,
        // then whose frames sit closest to the notes, then the smallest shift.
        inline PatternScore scorePattern (const std::vector<PitchFrame>& frames, const std::vector<PatternNote>& notes)
        {
            PatternScore best;
            double bestRank = 0.0;
            bool have = false;
            std::vector<PitchFrame> shifted (frames.size());
            for (double lag = -0.1; lag <= 0.3001; lag += 0.025)
            {
                for (size_t i = 0; i < frames.size(); ++i) shifted[i] = { frames[i].t - lag, frames[i].midi };
                auto s = scoreNotes (shifted, notes);
                double err = 0.0;
                for (const auto& n : notes)
                    for (const auto& f : shifted)
                        if (f.voiced() && f.t >= n.at && f.t <= n.at + n.dur)
                            err += std::min (100.0, std::abs (foldOctave ((f.midi - n.midi) * 100).cents));
                double heard = 0;
                for (const auto& x : s) if (x.cents) ++heard;
                const double rank = heard * 100000 - err - std::abs (lag) * 200;
                if (! have || rank > bestRank) { have = true; bestRank = rank; best = { std::move (s), lag }; }
            }
            return best;
        }

        // A siren's glide: from and to (MIDI), seconds up and seconds back down.
        struct Glide { double from = 0.0, to = 0.0, up = 0.0, down = 0.0; };

        struct SirenScore
        {
            double coverage = 0.0;                 // the share of the slide's notes reached
            std::optional<double> lowest, highest; // the notes reached
            double jump = 0.0;                     // the biggest sudden jump (a flip or crack), semitones
            std::optional<double> jumpAt;          // where it happened, when 2.5 semitones or more
        };

        inline SirenScore scoreSiren (const std::vector<PitchFrame>& frames, const Glide& g)
        {
            const double lo = std::min (g.from, g.to), hi = std::max (g.from, g.to);
            std::set<double> covered;
            SirenScore s;
            double jumpAt = 0.0;
            const PitchFrame* prev = nullptr;
            for (const auto& f : frames)
            {
                if (! f.voiced())
                {
                    if (prev && f.t - prev->t > 0.15) prev = nullptr;
                    continue;
                }
                if (f.midi >= lo - 0.5 && f.midi <= hi + 0.5) covered.insert (roundHalfUp (f.midi));
                if (prev && f.t - prev->t < 0.1)
                {
                    const double d = std::abs (f.midi - prev->midi);
                    if (d > s.jump) { s.jump = d; jumpAt = (f.midi + prev->midi) / 2; }
                }
                prev = &f;
            }
            s.coverage = std::min (1.0, (double) covered.size() / (hi - lo + 1));
            if (! covered.empty()) { s.lowest = *covered.begin(); s.highest = *covered.rbegin(); }
            if (s.jump >= 2.5) s.jumpAt = roundHalfUp (jumpAt);
            return s;
        }

        // The numbers behind an exercise's feedback, from its scored notes.
        struct NotesSummary
        {
            int heard = 0, total = 0;
            int good = 0;          // within 25 cents
            double topAvg = 0.0;   // mean cents of the highest quarter of the notes heard (at least one)
            double lowAvg = 0.0;   // ... and of the lowest quarter
            double all = 0.0;      // mean cents of every note heard
        };

        inline NotesSummary notesSummary (const std::vector<ScoredNote>& scored)
        {
            NotesSummary s;
            s.total = (int) scored.size();
            std::vector<ScoredNote> heard;
            for (const auto& x : scored) if (x.cents) heard.push_back (x);
            s.heard = (int) heard.size();
            if (heard.empty()) return s;
            std::vector<double> all;
            for (const auto& x : heard) { all.push_back (*x.cents); if (std::abs (*x.cents) <= 25) ++s.good; }
            auto sorted = heard;
            std::stable_sort (sorted.begin(), sorted.end(), [] (const ScoredNote& a, const ScoredNote& b) { return a.midi < b.midi; });
            const size_t q = std::max<size_t> (1, sorted.size() / 4);
            std::vector<double> top, low;
            for (size_t i = sorted.size() - std::min (q, sorted.size()); i < sorted.size(); ++i) top.push_back (*sorted[i].cents);
            for (size_t i = 0; i < std::min (q, sorted.size()); ++i) low.push_back (*sorted[i].cents);
            s.topAvg = mean (top);
            s.lowAvg = mean (low);
            s.all = mean (all);
            return s;
        }

        // ---- exercises for a range: notes and times only (their names and
        // instructions are the product's). Notes are MIDI numbers.

        // A saved range: the lowest and highest notes and the comfortable middle
        struct VocalRange { double low = 0, high = 0, tessLo = 0, tessHi = 0; };

        // A random number from 0 to 1 (the product's own source, so it can be repeated)
        using Rand = std::function<double()>;

        // The notes a range allows: the saved range less a semitone of cushion at each end
        struct NoteSpan { double lo = 0, hi = 0; };
        inline NoteSpan safeRange (const VocalRange& r) { return { r.low + 1, r.high - 1 }; }

        // One key shift for a song whose melody runs low..high: where the melody
        // lands, whether it fits the range, how far outside it goes, how far it
        // strays out of the comfortable middle, and its rank (lower is better).
        // keyPc: the new key's pitch class (-1 when the original isn't known).
        struct KeyShift { int shift = 0; double low = 0, high = 0; bool fits = false, comfortable = false; double outside = 0, strain = 0; int keyPc = -1; double rank = 0; };

        // Every shift from an octave down to an octave up, best first: the melody
        // in or near the comfortable middle, a smaller move winning a tie.
        inline std::vector<KeyShift> keyShiftsForRange (double songLow, double songHigh, const VocalRange& r, int originalPc = -1)
        {
            const double lo = std::min (songLow, songHigh), hi = std::max (songLow, songHigh);
            std::vector<KeyShift> out;
            for (int s = -12; s <= 12; ++s)
            {
                KeyShift k;
                k.shift = s;
                k.low = lo + s;
                k.high = hi + s;
                k.fits = k.low >= r.low && k.high <= r.high;
                k.outside = std::max (0.0, r.low - k.low) + std::max (0.0, k.high - r.high);
                k.strain = std::max (0.0, k.high - r.tessHi) + std::max (0.0, r.tessLo - k.low);
                const double centre = std::abs ((k.low + k.high) / 2 - (r.tessLo + r.tessHi) / 2);
                k.comfortable = k.fits && k.strain == 0;
                k.keyPc = originalPc < 0 ? -1 : ((originalPc + s) % 12 + 12) % 12;
                k.rank = (k.fits ? 0 : 1000 + k.outside * 10) + k.strain * 3 + centre * 0.5 + std::abs (s) * 0.15;
                out.push_back (k);
            }
            std::stable_sort (out.begin(), out.end(), [] (const KeyShift& a, const KeyShift& b) { return a.rank < b.rank; });
            return out;
        }

        // The next note to match. Level 1 stays in the comfortable middle with
        // small steps; each level widens the area and the jumps. Never outside
        // the safe range. prev: the last note, if any.
        inline double nextMatchNote (int level, const VocalRange& r, std::optional<double> prev, const Rand& rand)
        {
            const auto safe = safeRange (r);
            const NoteSpan area = level <= 2 ? NoteSpan { r.tessLo, r.tessHi }
                                : level == 3 ? NoteSpan { std::max (safe.lo, r.tessLo - 2), std::min (safe.hi, r.tessHi + 2) }
                                : safe;
            static constexpr int maxJumps[] { 0, 4, 7, 9, 12, 24 };
            const int maxJump = maxJumps[std::clamp (level, 1, 5)];
            double lo = area.lo, hi = std::max (area.lo, area.hi);
            if (prev)
            {
                lo = std::max (lo, *prev - maxJump);
                hi = std::min (hi, *prev + maxJump);
            }
            if (lo > hi) { lo = area.lo; hi = std::max (area.lo, area.hi); }
            std::vector<double> choices;
            for (double m = lo; m <= hi; m++) if (! prev || m != *prev) choices.push_back (m);
            if (choices.empty()) choices.push_back (lo);
            const double n = (double) choices.size();
            return choices[(size_t) std::fmod (std::floor (rand() * n), n)];
        }

        // A short tune to sing back: 2 notes (levels 1 and 2) or 3 (from level 3),
        // jumps growing with the level, every note in the safe range
        inline std::vector<double> buildSingBack (int level, const VocalRange& r, const Rand& rand)
        {
            const auto safe = safeRange (r);
            const int count = level >= 3 ? 3 : 2;
            static const std::vector<int> stepTables[] { { 1, 2, 3, 4 }, { 2, 3, 4, 5, 7 }, { 2, 3, 4, 5, 7 }, { 3, 4, 5, 7, 8, 9 }, { 4, 5, 7, 9, 10, 12 } };
            const auto& steps = stepTables[std::clamp (level, 1, 5) - 1];
            const double n = (double) steps.size();
            for (int tries = 0; tries < 50; ++tries)
            {
                std::vector<double> notes { nextMatchNote (std::min (level, 2), r, std::nullopt, rand) };
                for (int i = 1; i < count; ++i)
                {
                    const int pick = steps[(size_t) std::fmod (std::floor (rand() * n), n)];
                    const double step = pick * (rand() < 0.5 ? -1.0 : 1.0);
                    notes.push_back (notes.back() + step);
                }
                if (std::all_of (notes.begin(), notes.end(), [&] (double m) { return m >= safe.lo && m <= safe.hi; })) return notes;
            }
            return { r.tessLo, std::min (safe.hi, r.tessLo + 2) };
        }

        // ---- warm-ups

        // One pattern: a run of notes (PatternNote), or a siren (Glide)
        struct WarmupPattern { std::vector<PatternNote> notes; std::optional<Glide> glide; };
        // An exercise: its id (hum, trill, siren, arpeggio, vowels, sustain,
        // cooldown) and its patterns
        struct WarmupExercise { std::string id; std::vector<WarmupPattern> patterns; };

        inline std::vector<PatternNote> warmupPattern (const std::vector<int>& intervals, double root, double dur)
        {
            std::vector<PatternNote> out;
            for (size_t i = 0; i < intervals.size(); ++i) out.push_back ({ root + intervals[i], (double) i * dur, dur });
            return out;
        }

        // Roots for a pattern climbing (dir 1) or falling (dir -1) by half steps,
        // staying within lo..hi, at most count of them
        inline std::vector<double> warmupRoots (const std::vector<int>& intervals, double start, double lo, double hi, int count, int dir = 1)
        {
            const double top = *std::max_element (intervals.begin(), intervals.end());
            const double bottom = *std::min_element (intervals.begin(), intervals.end());
            std::vector<double> out;
            double r = start;
            while (r + bottom < lo) r++;
            while (r + top > hi && r + bottom > lo) r--;
            for (; (int) out.size() < count && r + bottom >= lo && r + top <= hi; r += dir) out.push_back (r);
            return out;
        }

        // A warm-up built for a range: hums, lip trills, sirens, arpeggios, a
        // vowel scale (full only), long notes and a cool-down. Every note stays
        // inside the safe range, and the climbing patterns stop a few semitones
        // above the comfortable middle. An exercise with no patterns is left out.
        inline std::vector<WarmupExercise> buildWarmup (const VocalRange& r, bool full)
        {
            const auto safe = safeRange (r);
            const double top = std::min (safe.hi, r.tessHi + 3);
            const double mid = roundHalfUp ((r.tessLo + r.tessHi) / 2);
            std::vector<WarmupExercise> ex;
            auto add = [&] (const char* id, std::vector<WarmupPattern> list) { if (! list.empty()) ex.push_back ({ id, std::move (list) }); };
            auto runs = [] (const std::vector<double>& roots, const std::vector<int>& iv, double dur)
            {
                std::vector<WarmupPattern> l;
                for (double root : roots) l.push_back ({ warmupPattern (iv, root, dur), std::nullopt });
                return l;
            };
            const std::vector<int> five { 7, 5, 4, 2, 0 };
            add ("hum", runs (warmupRoots (five, std::max (safe.lo, r.tessLo - 2), safe.lo, top, full ? 6 : 4), five, 0.6));
            const std::vector<int> trill { 0, 2, 4, 5, 7, 5, 4, 2, 0 };
            auto up = warmupRoots (trill, std::max (safe.lo, r.tessLo - 2), safe.lo, top, full ? 11 : 8);
            if (full && ! up.empty())
            {
                std::vector<double> down (up.begin(), up.end() - 1);
                std::reverse (down.begin(), down.end());
                if (down.size() > 4) down.resize (4);
                up.insert (up.end(), down.begin(), down.end());
            }
            add ("trill", runs (up, trill, 0.4));
            const double sLo = std::max (safe.lo, r.tessLo - 3), sHi = std::min (safe.hi, r.tessHi + 4);
            if (sHi - sLo >= 5)
                add ("siren", std::vector<WarmupPattern> ((size_t) (full ? 4 : 3), WarmupPattern { {}, Glide { sLo, sHi, 2.5, 2.5 } }));
            const std::vector<int> arp = std::min (top, safe.hi) - r.tessLo < 12 ? std::vector<int> { 0, 4, 7, 4, 0 } : std::vector<int> { 0, 4, 7, 12, 7, 4, 0 };
            add ("arpeggio", runs (warmupRoots (arp, std::max (safe.lo, r.tessLo - 1), safe.lo, top, full ? 8 : 5), arp, 0.45));
            if (full) add ("vowels", runs (warmupRoots (trill, r.tessLo, safe.lo, top, 6), trill, 0.4));
            std::vector<WarmupPattern> holds;
            for (double m : { mid - 2, mid, mid + 2 })
                if (m >= safe.lo && m <= safe.hi) holds.push_back ({ { { m, 0.0, full ? 5.0 : 4.0 } }, std::nullopt });
            add ("sustain", holds);
            add ("cooldown", runs (warmupRoots (five, std::min (mid + 1, top - 7), safe.lo, top, full ? 5 : 3, -1), five, 0.6));
            return ex;
        }

        // How long a pattern lasts, in seconds
        inline double patternSeconds (const WarmupPattern& p)
        {
            if (p.glide) return p.glide->up + p.glide->down;
            double end = -std::numeric_limits<double>::infinity();
            for (const auto& n : p.notes) end = std::max (end, n.at + n.dur);
            return end;
        }

        // A siren's target pitch at t seconds from its start
        inline double glideAt (const Glide& g, double t)
        {
            return t <= g.up ? g.from + ((g.to - g.from) * t) / g.up : g.to - (g.to - g.from) * std::min (1.0, (t - g.up) / g.down);
        }
    }

    // Follows a voice frame by frame: gates out silence and breath, catches
    // octave slips (a sudden jump over 7 semitones is only believed once it has
    // lasted 0.1 s, unless the old note is still one of the candidate periods),
    // and smooths lightly with a median of three (enough to steady the line
    // without shrinking a vibrato).
    class VoiceTracker
    {
    public:
        struct Options
        {
            double minHz = 65.0, maxHz = 1100.0;
            double gate = 0.0025;       // the quietest level counted as voice (RMS)
            double minClarity = 0.72;
            double window = voice::trackWindow; // seconds of sound per reading
            double a4 = 440.0;
        };

        struct Frame
        {
            double t = 0.0;
            double midi = std::numeric_limits<double>::quiet_NaN(); // with a fraction; NaN when there's no voice
            double hz = 0.0;
            double clarity = 0.0, rms = 0.0;
            bool voiced() const { return ! std::isnan (midi); }
        };

        VoiceTracker() : VoiceTracker (Options {}) {}
        explicit VoiceTracker (const Options& o) : opt (o), yin (Yin::voice (o.minHz, o.maxHz)) {}

        const Options& options() const { return opt; }

        void reset()
        {
            raw.clear();
            out.reset();
            lastVoiced = -1.0;
            jump.reset();
        }

        // samples[0..n): the newest samples (at least `window` seconds of them,
        // oldest first); t: when, in seconds. Reads the last `window` seconds,
        // averaged down to about 24 kHz.
        Frame feed (const float* samples, int n, double sampleRate, double t)
        {
            const int len = std::min (n, (int) voice::roundHalfUp (sampleRate * opt.window));
            const int stride = std::max (1, (int) voice::roundHalfUp (sampleRate / 24000));
            const auto x = boxDownsample (samples + (n - len), len, stride);
            return push (yin.analyse (x.data(), (int) x.size(), sampleRate / stride), t);
        }

        // One reading already made (with Yin::voice()).
        Frame push (const Yin::Result& det, double t)
        {
            Frame frame;
            frame.t = t;
            frame.clarity = det.clarity;
            frame.rms = det.rms;
            if (! (det.hz > 0) || det.rms < opt.gate || det.clarity < opt.minClarity)
            {
                if (t - lastVoiced > 0.08) { raw.clear(); out.reset(); jump.reset(); }
                return frame;
            }
            double m = music::hzToMidi (det.hz, opt.a4);
            const std::optional<double> ref = out && t - lastVoiced < 0.15 ? out : std::nullopt;
            if (ref && std::abs (m - *ref) > 7)
            {
                // a big jump: either an octave slip (the old note is still one of the dips) or a real leap
                const bool sameJump = jump && std::abs (jump->m - m) < 1.5;
                jump = Jump { m, sameJump ? jump->since : t };
                if (t - jump->since < 0.1)
                {
                    const Yin::Candidate* alt = nullptr;
                    for (const auto& c : det.candidates)
                        if ((double) c.d < 0.35 && std::abs (music::hzToMidi (c.hz, opt.a4) - *ref) < 1) { alt = &c; break; }
                    if (! alt)
                    {
                        lastVoiced = t;
                        return frame; // not sure yet: nothing rather than a wrong note
                    }
                    m = music::hzToMidi (alt->hz, opt.a4);
                }
                else { raw.clear(); out.reset(); jump.reset(); }
            }
            else jump.reset();
            lastVoiced = t;
            raw.push_back (m);
            if (raw.size() > 3) raw.erase (raw.begin());
            auto sorted = raw;
            std::sort (sorted.begin(), sorted.end());
            out = sorted[sorted.size() >> 1];
            frame.midi = *out;
            frame.hz = music::midiToHz (*out, opt.a4);
            return frame;
        }

    private:
        struct Jump { double m, since; };
        Options opt;
        Yin yin;
        std::vector<double> raw;
        std::optional<double> out;
        double lastVoiced = -1.0;
        std::optional<Jump> jump;
    };

    // Turns a stream of pitch readings from an instrument (the nearest note,
    // every few milliseconds) into notes played: a note is reported once the
    // same pitch has held for `hold` readings, and again only after the pitch
    // changes, goes quiet for `quietReset` readings, or is plucked again (the
    // level jumps to more than 2.2 times the last).
    class NoteFinder
    {
    public:
        struct Reading
        {
            double t = 0.0;
            double midi = std::numeric_limits<double>::quiet_NaN(); // NaN: no pitch
            double level = 0.0;
        };
        struct Note { double midi = 0.0, t = 0.0; }; // t: when its first reading came

        explicit NoteFinder (int holdReadings = 3, int quietReadings = 4) : hold (holdReadings), quietReset (quietReadings) {}

        // r: a reading, or nullptr for none at all.
        std::optional<Note> feed (const Reading* r)
        {
            if (! r || std::isnan (r->midi))
            {
                if (++quiet >= quietReset) { current.reset(); cand.reset(); count = 0; }
                lastLevel = r ? levelOf (*r) : 0.0;
                return std::nullopt;
            }
            quiet = 0;
            const bool replucked = current && *current == r->midi && r->level > lastLevel * 2.2 && r->level > 0.01;
            lastLevel = levelOf (*r);
            if (replucked) { current.reset(); cand.reset(); count = 0; }
            if (current && r->midi == *current) return std::nullopt;
            if (! cand || r->midi != *cand) { cand = r->midi; count = 1; firstT = r->t; }
            else ++count;
            if (count >= hold)
            {
                current = cand;
                cand.reset();
                count = 0;
                return Note { *current, firstT };
            }
            return std::nullopt;
        }

    private:
        static double levelOf (const Reading& r) { return std::isnan (r.level) || r.level == 0.0 ? 0.0 : r.level; }
        int hold, quietReset;
        std::optional<double> cand, current;
        int count = 0, quiet = 0;
        double lastLevel = 0.0, firstT = 0.0;
    };

    // Timing against a click, in milliseconds.
    struct TimingReport
    {
        std::vector<double> offsets; // each played note's offset from its beat (late is positive)
        double mean = 0.0;
        double spread = 0.0;         // the standard deviation of the offsets
        double onTime = 0.0;         // the share of notes within `ok` seconds of their beat, percent
    };

    // times[i] and beatTimes[i] in seconds (NaN: not played, or no beat);
    // nothing when no note has both.
    inline std::optional<TimingReport> timingReport (const std::vector<double>& times, const std::vector<double>& beatTimes, double ok = 0.07)
    {
        std::vector<double> offs;
        for (size_t i = 0; i < times.size(); ++i)
        {
            const double b = i < beatTimes.size() ? beatTimes[i] : std::numeric_limits<double>::quiet_NaN();
            if (! std::isnan (times[i]) && ! std::isnan (b)) offs.push_back (times[i] - b);
        }
        if (offs.empty()) return std::nullopt;
        const double mean = stats::mean (offs.data(), (int) offs.size());
        const double spread = stats::standardDeviation (offs.data(), (int) offs.size());
        TimingReport r;
        int within = 0;
        for (double x : offs)
        {
            r.offsets.push_back (voice::roundHalfUp (x * 1000));
            if (std::abs (x) <= ok) ++within;
        }
        r.mean = voice::roundHalfUp (mean * 1000);
        r.spread = voice::roundHalfUp (spread * 1000);
        r.onTime = voice::roundHalfUp (100.0 * within / (double) offs.size());
        return r;
    }
}
