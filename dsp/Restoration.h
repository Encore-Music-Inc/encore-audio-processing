#pragma once

// The restoration chain for transfers of records and tapes (Restore's). Every
// stage streams (push a block in, get restored audio out, flush at the end),
// so a long LP side at 96 kHz goes through a few seconds at a time.
//
// The order matters:
//   1. rumble and DC     (the needle's low-frequency junk first)
//   2. channels          (line up cassette azimuth, then sum or pick for mono records)
//   3. de-click          (before anything smears the clicks)
//   4. de-hum
//   5. noise reduction   (learned on hiss that has had its clicks and hum taken out)
//   6. playback EQ       (an optional 78 curve, air)
//   7. speed and wow     (last: resampling moves everything, so it goes after the repairs)
//
// Settings come as restore::Settings, or as JSON (settingsFromJson) with the
// same names. The sources' starting points (defaults), the manual speed fixes
// (rpmFixes) and the 78 playback curves (playbackCurves) are here by id; their
// names are the product's.

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "Biquad.h"
#include "Declicker.h"
#include "FracDelay.h"
#include "Json.h"
#include "NoiseReduction.h"
#include "Resampler.h"

namespace vs
{
    namespace restore
    {
        enum class ChannelMode { stereo, sum, left, right };

        struct Settings
        {
            std::string source = "lp";
            struct { bool on = true; double hz = 20.0; bool dc = true; } rumble;
            struct { bool on = false; ChannelMode mode = ChannelMode::stereo; bool alignOn = false; double delay = 0.0;
                     bool balanceOn = false; double balanceDb = 0.0; bool flip = false; } channels;
            struct { bool on = false; double clicks = 50.0, crackle = 40.0, maxMs = 4.0; } declick;
            struct { bool on = false; double hz = 60.0; std::vector<int> harmonics; double width = 0.6; } dehum;
            struct { bool on = false; double amount = 40.0; std::vector<float> profile; } denoise;
            struct { bool on = false; std::string curve = "none"; double turnover = 500.0, rolloff = 0.0, shelf = 50.0;
                     bool riaaApplied = true; double air = 0.0; } eq;
            struct { bool on = false; double cents = 0.0; std::string rpm = "none"; double a4 = 440.0; } speed;
            // the measured wobble: `rate` values a second over the input's time,
            // cents (positive = running fast there); amount 0..100 of it is undone
            struct { bool on = false; double amount = 70.0; double rate = 50.0; std::vector<float> cents; } wow;
        };

        // The starting point for a kind of transfer ("lp", "78", "cassette",
        // "8track", "reel", "cd", "other"); the analysis then turns modules on
        // or off from what it actually hears.
        inline Settings defaults (const std::string& source)
        {
            Settings s;
            s.source = source;
            const bool tape = source == "cassette" || source == "8track" || source == "reel";
            if (source == "lp") s.declick.on = true;
            if (source == "78")
            {
                s.rumble.hz = 40.0;
                s.declick.on = true; s.declick.clicks = 65.0; s.declick.crackle = 60.0; s.declick.maxMs = 4.0;
                s.denoise.amount = 55.0;
                s.channels.on = true; s.channels.mode = ChannelMode::sum;
            }
            if (tape) { s.rumble.hz = 20.0; s.denoise.amount = source == "8track" ? 50.0 : 40.0; }
            if (source == "cd") s.rumble.on = false;
            return s;
        }

        // The manual speed fixes: the speed it was played at and the speed it was cut at.
        struct RpmFix { const char* id; double from, to; };
        inline const std::vector<RpmFix>& rpmFixes()
        {
            static const std::vector<RpmFix> list {
                { "none", 0, 0 },
                { "45at33", 100.0 / 3.0, 45.0 }, { "33at45", 45.0, 100.0 / 3.0 },
                { "78at80", 78.0, 80.0 }, { "78at76", 78.0, 76.0 }, { "78at75", 78.0, 75.0 }, { "78at82", 78.0, 82.0 },
                { "78at33", 100.0 / 3.0, 78.0 }, { "78at45", 45.0, 78.0 },
            };
            return list;
        }
        inline double rpmCents (const std::string& id)
        {
            for (const auto& f : rpmFixes()) if (id == f.id) return f.from > 0 ? 1200.0 * std::log2 (f.to / f.from) : 0.0;
            return 0.0;
        }

        // The 78 playback curves: bass turnover (Hz) and treble roll-off at 10 kHz
        // (dB). The commonly published starting points; labels changed curves over
        // the years, so ears and the label's era decide. "none" and "custom" carry
        // no numbers.
        struct Curve { const char* id; double turnover, rolloff; bool fixed; };
        inline const std::vector<Curve>& playbackCurves()
        {
            static const std::vector<Curve> list {
                { "none", 0, 0, false }, { "e500", 500, 0, true }, { "e300", 300, 0, true }, { "emi", 250, 0, true },
                { "aes", 400, -12, true }, { "nab", 500, -16, true }, { "custom", 0, 0, false },
            };
            return list;
        }

        // The whole-recording speed fix in cents (the manual RPM fix plus the tuning fix).
        inline double speedCents (const Settings& s) { return s.speed.on ? rpmCents (s.speed.rpm) + s.speed.cents : 0.0; }

        // The gain an export is made at: anything over -0.1 dBFS is brought down
        // evenly for the whole export (never clipped); otherwise none.
        inline double exportGain (double peakDb) { return peakDb > -0.1 ? std::pow (10.0, (-0.1 - peakDb) / 20.0) : 1.0; }

        // A moment of the original to where it lands in the restored file, and back.
        inline double toRestoredTime (double t, double cents) { return t / std::pow (2.0, cents / 1200.0); }
        inline double toOriginalTime (double t, double cents) { return t * std::pow (2.0, cents / 1200.0); }

        // A narrow notch on the hum and each harmonic found. Narrow (0.6 Hz wide at
        // 60 Hz by default), so a tuba note a semitone away is untouched; a touch
        // wider up high, where drift is multiplied.
        inline std::vector<BiquadCoefs> humSections (double fs, double hz, const std::vector<int>& harmonics, double width = 0.6)
        {
            std::vector<BiquadCoefs> out;
            for (int h : harmonics)
            {
                const double f = hz * h;
                if (f >= 0.45 * fs) break;
                const double bw = width * (1 + 0.05 * (h - 1));
                out.push_back (BiquadCoefs::rbj (BiquadCoefs::Type::notch, fs, f, f / bw));
            }
            return out;
        }

        // A first-order digital section fitted to the analog rise sqrt (1 + (f/fz)^2)
        // from 100 Hz to 16 kHz (or 0.42 fs), by a coarse-then-fine search of its
        // pole and zero, its gain matching the analog value at 100 Hz.
        inline BiquadCoefs fitRising (double fz, double fs)
        {
            const double twoPi = 2.0 * 3.14159265358979323846;
            std::vector<double> fr, want;
            std::vector<std::pair<double, double>> cw;
            for (double f = 100; f <= std::min (16000.0, 0.42 * fs); f *= 1.15) fr.push_back (f);
            for (double f : fr)
            {
                want.push_back (10.0 * std::log10 (1.0 + (f / fz) * (f / fz)));
                cw.push_back ({ std::cos (twoPi * f / fs), std::sin (twoPi * f / fs) });
            }
            struct Fit { double worst, g0, z, p; };
            auto err = [&] (double z, double p)
            {
                double worst = 0.0, g0 = 0.0;
                for (size_t i = 0; i < cw.size(); ++i)
                {
                    const auto [c, sn] = cw[i];
                    const double m = 10.0 * std::log10 (((1 - z * c) * (1 - z * c) + (z * sn) * (z * sn)) / ((1 - p * c) * (1 - p * c) + (p * sn) * (p * sn)));
                    if (i == 0) g0 = m;
                    worst = std::max (worst, std::abs (m - g0 - (want[i] - want[0])));
                }
                return std::pair<double, double> { worst, g0 };
            };
            Fit best { 1e9, 0, 0, 0 };
            double step = 0.02, lo0 = -0.98, lo1 = -0.98, hi0 = 0.99, hi1 = 0.95;
            for (int round = 0; round < 3; ++round, step /= 10)
            {
                for (double z = lo0; z <= hi0; z += step)
                    for (double p = lo1; p <= hi1; p += step)
                    {
                        if (std::abs (p) >= 0.999) continue;
                        const auto [worst, g0] = err (z, p);
                        if (worst < best.worst) best = { worst, g0, z, p };
                    }
                lo0 = best.z - 10 * step; lo1 = best.p - 10 * step;
                hi0 = best.z + 10 * step; hi1 = best.p + 10 * step;
            }
            const double g = std::pow (10.0, (want[0] - best.g0) / 20.0);
            return { g, -g * best.z, 0.0, -best.p, 0.0 };
        }

        // The playback curve for a 78: bass boosted below the turnover (levelling
        // off from `shelf` Hz down), treble cut by `rolloff` dB at 10 kHz.
        // riaaApplied: the transfer went through a normal (RIAA) phono preamp, so
        // that is undone first. 0 dB at 1 kHz.
        inline std::vector<BiquadCoefs> curveSections (double fs, double turnover = 500.0, double rolloff = 0.0, double shelf = 50.0, bool riaaApplied = true)
        {
            std::vector<BiquadCoefs> s;
            auto add = [&] (std::optional<BiquadCoefs> c) { if (c) s.push_back (*c); };
            if (riaaApplied)
            {
                // inverse RIAA: zeros where RIAA has poles (50.05 Hz, 2122 Hz), a pole at
                // its zero (500.5 Hz). The rising treble is fitted numerically: a plain
                // digital version bends away from the analog curve near 10 kHz.
                add (firstOrder (50.05, 500.5, fs));
                s.push_back (fitRising (2122.0, fs));
            }
            if (turnover > 0) add (firstOrder (turnover, shelf != 0 ? shelf : 1.0, fs));
            if (rolloff < 0 && fs > 20000)
            {
                // a single pole, placed so the digital filter is exactly the roll-off down at 10 kHz
                const double pi = 3.14159265358979323846;
                const double t = std::tan (pi * 10000.0 / fs) / std::sqrt (std::pow (10.0, -rolloff / 10.0) - 1.0);
                add (firstOrder (0.0, fs / pi * std::atan (t), fs));
            }
            const double g = sectionsResponse (s, 1000.0, fs);
            if (! s.empty() && g > 0)
            {
                s[0].b0 /= g; s[0].b1 /= g; s[0].b2 /= g;
            }
            return s;
        }

        using Buf = std::vector<float>;

        // One stage of the chain: push takes both channels' next samples and
        // leaves its output in their place (any length, the same for both);
        // flush leaves what it still holds.
        struct Stage
        {
            virtual ~Stage() = default;
            virtual void push (Buf& l, Buf& r) = 0;
            virtual void flush (Buf& l, Buf& r) { l.clear(); r.clear(); }
        };

        struct FilterStage : Stage
        {
            BiquadChain f;
            explicit FilterStage (std::vector<BiquadCoefs> s) : f (std::move (s), 2) {}
            void push (Buf& l, Buf& r) override { f.process (0, l.data(), (int) l.size()); f.process (1, r.data(), (int) r.size()); }
        };

        // 24 dB an octave below `hz` (records and decks have nothing musical down
        // there); with that off, DC alone: a gentle 5 Hz first-order cut.
        inline std::vector<BiquadCoefs> rumbleSections (double fs, bool on, double hz, bool dc)
        {
            if (on && hz > 0) return butterworth (BiquadCoefs::Type::highPass, fs, hz, 4);
            if (dc) return { dcBlocker (fs) };
            return {};
        }

        // delay: how many samples the right channel arrives after the left
        // (cassette azimuth); the left is held back to match, through a
        // fractional delay whose own latency is taken off again. Then balance
        // (balanceDb: how much louder the left is, split between the two),
        // polarity, and sum or pick for a mono record.
        struct ChannelStage : Stage
        {
            ChannelMode mode;
            bool flip;
            double gL = 1.0, gR = 1.0;
            std::unique_ptr<FracDelay> fd;
            int skip = 0;

            ChannelStage (const Settings& s)
                : mode (s.channels.mode), flip (s.channels.flip)
            {
                if (s.channels.balanceOn) { gL = std::pow (10.0, -s.channels.balanceDb / 40.0); gR = std::pow (10.0, s.channels.balanceDb / 40.0); }
                if (s.channels.alignOn && std::abs (s.channels.delay) > 0.01)
                {
                    fd = std::make_unique<FracDelay> (std::vector<double> { std::max (0.0, s.channels.delay), std::max (0.0, -s.channels.delay) });
                    skip = fd->latency();
                }
            }

            void push (Buf& l, Buf& r) override
            {
                if (fd)
                {
                    fd->process (0, l.data(), (int) l.size());
                    fd->process (1, r.data(), (int) r.size());
                    const size_t drop = std::min<size_t> ((size_t) skip, l.size());
                    l.erase (l.begin(), l.begin() + (long) drop);
                    r.erase (r.begin(), r.begin() + (long) drop);
                    skip -= (int) drop;
                }
                mix (l, r);
            }

            void flush (Buf& l, Buf& r) override
            {
                l.clear(); r.clear();
                if (! fd) return;
                l.assign ((size_t) fd->latency(), 0.0f);
                r.assign ((size_t) fd->latency(), 0.0f);
                push (l, r);
            }

            void mix (Buf& l, Buf& r) const
            {
                for (size_t i = 0; i < l.size(); ++i)
                {
                    double a = l[i] * gL, b = r[i] * gR * (flip ? -1.0 : 1.0);
                    if (mode == ChannelMode::sum) { const double m = 0.5 * (a + b); a = m; b = m; }
                    else if (mode == ChannelMode::left) b = a;
                    else if (mode == ChannelMode::right) a = b;
                    l[i] = (float) a; r[i] = (float) b;
                }
            }
        };

        struct DeclickStage : Stage
        {
            Declicker d;
            DeclickStage (double fs, const declick::Settings& s) : d (fs, s) {}
            void push (Buf& l, Buf& r) override
            {
                const float* ch[2] = { l.data(), r.data() };
                d.push (ch, (int) l.size());
                l = d.output (0); r = d.output (1);
            }
            void flush (Buf& l, Buf& r) override { d.flush(); l = d.output (0); r = d.output (1); }
        };

        struct DenoiseStage : Stage
        {
            SpectralDenoiser d;
            DenoiseStage (double fs, double amount, const std::vector<float>& profile, bool keep) : d (fs, amount, profile, keep) {}
            void push (Buf& l, Buf& r) override { d.push (l.data(), r.data(), (int) l.size()); l = d.output (0); r = d.output (1); }
            void flush (Buf& l, Buf& r) override { d.flush(); l = d.output (0); r = d.output (1); }
        };

        // cents: the fix for the whole recording (negative = lower and slower).
        // The wobble curve, amount 0..1 of it undone. offset: where in the
        // recording this stage's first sample is (previews start mid-side).
        struct SpeedStage : Stage
        {
            Resampler r;

            static Resampler make (double fs, double cents, const std::vector<float>& curve, double rate, double amount, double offset)
            {
                const int n = (int) curve.size();
                double maxDev = 0.0;
                for (int i = 0; i < n; ++i) maxDev = std::max (maxDev, curve[(size_t) i] * amount);
                auto at = [curve, rate, n, fs, offset] (double pos) -> double
                {
                    if (n == 0) return 0.0;
                    const double t = (pos + offset) / fs * rate;
                    const double i = std::floor (t);
                    if (i < 0) return curve[0];
                    if (i >= n - 1) return curve[(size_t) (n - 1)];
                    const int k = (int) i;
                    return curve[(size_t) k] + (t - i) * (curve[(size_t) (k + 1)] - curve[(size_t) k]);
                };
                // input samples per output sample: running fast (a positive wobble) means read slower
                return Resampler ([at, cents, amount] (double pos) { return std::pow (2.0, (cents - amount * at (pos)) / 1200.0); },
                                  std::pow (2.0, (cents + amount * maxDev) / 1200.0), 2);
            }

            SpeedStage (double fs, double cents, const std::vector<float>& curve, double rate, double amount, double offset)
                : r (make (fs, cents, curve, rate, amount, offset)) {}

            void push (Buf& l, Buf& rr) override
            {
                const float* ch[2] = { l.data(), rr.data() };
                const int n = r.push (ch, (int) l.size());
                l.assign (r.output (0), r.output (0) + n);
                rr.assign (r.output (1), r.output (1) + n);
            }
            void flush (Buf& l, Buf& rr) override
            {
                const int n = r.flush();
                l.assign (r.output (0), r.output (0) + n);
                rr.assign (r.output (1), r.output (1) + n);
            }
        };

        // Which modules a run leaves out, for "what was removed" (the chain run
        // with and without them, and the difference), and whether the de-clicker
        // and noise reducer are kept in place (changing nothing) when left out, so
        // both runs line up sample for sample.
        struct Run
        {
            bool offDeclick = false, offDehum = false, offDenoise = false;
            bool keepDelays = false;
            double offset = 0.0;   // the input sample this run starts at
        };

        class Chain
        {
        public:
            Chain (double fs, const Settings& s, const Run& run = {})
            {
                if (s.rumble.on || s.rumble.dc) stages.push_back (std::make_unique<FilterStage> (rumbleSections (fs, s.rumble.on, s.rumble.hz, s.rumble.dc)));
                const bool linked = s.channels.on && s.channels.mode != ChannelMode::stereo;
                if (s.channels.on) stages.push_back (std::make_unique<ChannelStage> (s));
                declick::Settings dc;
                dc.clicks = s.declick.clicks; dc.crackle = s.declick.crackle; dc.maxMs = s.declick.maxMs; dc.linked = linked;
                if (s.declick.on && ! run.offDeclick) addDeclick (fs, dc);
                else if (run.keepDelays) { dc.clicks = 0; dc.crackle = 0; dc.maxMs = 4.0; addDeclick (fs, dc); }
                if (s.dehum.on && ! run.offDehum && ! s.dehum.harmonics.empty())
                    stages.push_back (std::make_unique<FilterStage> (humSections (fs, s.dehum.hz, s.dehum.harmonics, s.dehum.width)));
                if (s.denoise.on && ! run.offDenoise && ! s.denoise.profile.empty())
                    stages.push_back (std::make_unique<DenoiseStage> (fs, s.denoise.amount, s.denoise.profile, false));
                else if (run.keepDelays && ! s.denoise.profile.empty())
                    stages.push_back (std::make_unique<DenoiseStage> (fs, 0.0, s.denoise.profile, true));
                if (s.eq.on && (s.eq.curve != "none" || s.eq.air > 0))
                {
                    std::vector<BiquadCoefs> eq;
                    if (s.eq.curve != "none")
                    {
                        double turnover = s.eq.turnover, rolloff = s.eq.rolloff;
                        for (const auto& c : playbackCurves()) if (s.eq.curve == c.id && c.fixed) { turnover = c.turnover; rolloff = c.rolloff; }
                        for (const auto& c : curveSections (fs, turnover, rolloff, s.eq.shelf, s.eq.riaaApplied)) eq.push_back (c);
                    }
                    if (s.eq.air > 0) eq.push_back (BiquadCoefs::rbj (BiquadCoefs::Type::highShelf, fs, 9000.0, 0.6, s.eq.air));
                    stages.push_back (std::make_unique<FilterStage> (std::move (eq)));
                }
                const double cents = speedCents (s);
                const bool wow = s.wow.on && ! s.wow.cents.empty();
                if (std::abs (cents) > 0.01 || wow)
                    stages.push_back (std::make_unique<SpeedStage> (fs, cents, wow ? s.wow.cents : std::vector<float> {}, wow ? s.wow.rate : 1.0,
                                                                     wow ? s.wow.amount / 100.0 : 1.0, run.offset));
            }

            // The next n samples (r may be null: l for both). Returns how many
            // restored samples per channel are ready (output (c)).
            int push (const float* l, const float* r, int n)
            {
                bufL.assign (l, l + n);
                bufR.assign (r != nullptr ? r : l, (r != nullptr ? r : l) + n);
                for (auto& st : stages)
                {
                    st->push (bufL, bufR);
                    if (bufL.empty()) break;
                }
                return (int) bufL.size();
            }

            // The rest, after the last push: each stage flushed in turn, what it
            // gives fed through the stages after it.
            int flush()
            {
                Buf allL, allR, l, r;
                for (size_t i = 0; i < stages.size(); ++i)
                {
                    stages[i]->flush (l, r);
                    for (size_t j = i + 1; j < stages.size() && ! l.empty(); ++j) stages[j]->push (l, r);
                    allL.insert (allL.end(), l.begin(), l.end());
                    allR.insert (allR.end(), r.begin(), r.end());
                }
                bufL = std::move (allL);
                bufR = std::move (allR);
                return (int) bufL.size();
            }

            const Buf& output (int c) const { return c == 0 ? bufL : bufR; }

            // What the de-clicker found and rebuilt so far (empty without one).
            const std::vector<declick::Event>& clicks() const
            {
                static const std::vector<declick::Event> none;
                return declicker != nullptr ? declicker->d.events() : none;
            }

        private:
            std::vector<std::unique_ptr<Stage>> stages;
            DeclickStage* declicker = nullptr;
            Buf bufL, bufR;

            void addDeclick (double fs, const declick::Settings& dc)
            {
                auto st = std::make_unique<DeclickStage> (fs, dc);
                declicker = st.get();
                stages.push_back (std::move (st));
            }
        };

        // ---- settings as JSON (the same names as Settings; the wobble as {"rate", "cents": [..]})

        inline Settings settingsFromJson (const json::Json& j)
        {
            Settings s = defaults ("other");
            auto num = [] (const json::Json* o, const char* k, double& v) { if (o) if (const auto* x = o->get (k); x && x->type == json::Json::Type::number) v = x->n; };
            auto flag = [] (const json::Json* o, const char* k, bool& v) { if (o) if (const auto* x = o->get (k); x && x->type == json::Json::Type::boolean) v = x->b; };
            auto text = [] (const json::Json* o, const char* k, std::string& v) { if (o) if (const auto* x = o->get (k); x && x->type == json::Json::Type::string) v = x->s; };
            auto floats = [] (const json::Json* a, std::vector<float>& v) { v.clear(); if (a && a->type == json::Json::Type::array) for (const auto& x : a->items) v.push_back ((float) x.n); };
            text (&j, "source", s.source);
            if (const auto* o = j.get ("rumble")) { flag (o, "on", s.rumble.on); num (o, "hz", s.rumble.hz); flag (o, "dc", s.rumble.dc); }
            if (const auto* o = j.get ("channels"))
            {
                flag (o, "on", s.channels.on);
                std::string mode = "stereo";
                text (o, "mode", mode);
                s.channels.mode = mode == "sum" ? ChannelMode::sum : mode == "left" ? ChannelMode::left : mode == "right" ? ChannelMode::right : ChannelMode::stereo;
                flag (o, "alignOn", s.channels.alignOn); num (o, "delay", s.channels.delay);
                flag (o, "balanceOn", s.channels.balanceOn); num (o, "balanceDb", s.channels.balanceDb); flag (o, "flip", s.channels.flip);
            }
            if (const auto* o = j.get ("declick")) { flag (o, "on", s.declick.on); num (o, "clicks", s.declick.clicks); num (o, "crackle", s.declick.crackle); num (o, "maxMs", s.declick.maxMs); }
            if (const auto* o = j.get ("dehum"))
            {
                flag (o, "on", s.dehum.on); num (o, "hz", s.dehum.hz); num (o, "width", s.dehum.width);
                s.dehum.harmonics.clear();
                if (const auto* h = o->get ("harmonics"); h && h->type == json::Json::Type::array) for (const auto& x : h->items) s.dehum.harmonics.push_back ((int) x.n);
            }
            if (const auto* o = j.get ("denoise")) { flag (o, "on", s.denoise.on); num (o, "amount", s.denoise.amount); floats (o->get ("profile"), s.denoise.profile); }
            if (const auto* o = j.get ("eq"))
            {
                flag (o, "on", s.eq.on); text (o, "curve", s.eq.curve); num (o, "turnover", s.eq.turnover); num (o, "rolloff", s.eq.rolloff);
                num (o, "shelf", s.eq.shelf); flag (o, "riaaApplied", s.eq.riaaApplied); num (o, "air", s.eq.air);
            }
            if (const auto* o = j.get ("speed")) { flag (o, "on", s.speed.on); num (o, "cents", s.speed.cents); text (o, "rpm", s.speed.rpm); num (o, "a4", s.speed.a4); }
            if (const auto* o = j.get ("wow"))
            {
                flag (o, "on", s.wow.on); num (o, "amount", s.wow.amount);
                s.wow.cents.clear();
                if (const auto* c = o->get ("curve"); c && c->type == json::Json::Type::object) { num (c, "rate", s.wow.rate); floats (c->get ("cents"), s.wow.cents); }
            }
            return s;
        }

        inline void settingsToJson (const Settings& s, json::Out& o)
        {
            const char* modes[] = { "stereo", "sum", "left", "right" };
            o.open ('{').key ("source").str (s.source);
            o.key ("rumble").open ('{').key ("on").boolean (s.rumble.on).key ("hz").num (s.rumble.hz).key ("dc").boolean (s.rumble.dc).close ('}');
            o.key ("channels").open ('{').key ("on").boolean (s.channels.on).key ("mode").str (modes[(int) s.channels.mode])
             .key ("alignOn").boolean (s.channels.alignOn).key ("delay").num (s.channels.delay).key ("balanceOn").boolean (s.channels.balanceOn)
             .key ("balanceDb").num (s.channels.balanceDb).key ("flip").boolean (s.channels.flip).close ('}');
            o.key ("declick").open ('{').key ("on").boolean (s.declick.on).key ("clicks").num (s.declick.clicks).key ("crackle").num (s.declick.crackle)
             .key ("maxMs").num (s.declick.maxMs).close ('}');
            o.key ("dehum").open ('{').key ("on").boolean (s.dehum.on).key ("hz").num (s.dehum.hz).key ("harmonics").open ('[');
            for (int h : s.dehum.harmonics) o.next().num (h);
            o.close (']').key ("width").num (s.dehum.width).close ('}');
            o.key ("denoise").open ('{').key ("on").boolean (s.denoise.on).key ("amount").num (s.denoise.amount).close ('}');
            o.key ("eq").open ('{').key ("on").boolean (s.eq.on).key ("curve").str (s.eq.curve).key ("turnover").num (s.eq.turnover)
             .key ("rolloff").num (s.eq.rolloff).key ("shelf").num (s.eq.shelf).key ("riaaApplied").boolean (s.eq.riaaApplied).key ("air").num (s.eq.air).close ('}');
            o.key ("speed").open ('{').key ("on").boolean (s.speed.on).key ("cents").num (s.speed.cents).key ("rpm").str (s.speed.rpm).key ("a4").num (s.speed.a4).close ('}');
            o.key ("wow").open ('{').key ("on").boolean (s.wow.on).key ("amount").num (s.wow.amount).close ('}');
            o.close ('}');
        }
    }
}
