// The effects, instruments and helpers products share: gain and mix stages,
// meter ballistics, note editing, delay times, loudness targets, the
// resonance band rule, the MP3 rate, the tuner's follower, the presets list,
// the chorus, the phaser, time stretch and pitch shift, the synth, the sampler
// and the compressor's general setting. Built without JUCE. Each check prints
// PASS or FAIL; the suite exits non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "dsp/Chorus.h"
#include "dsp/Compressor.h"
#include "dsp/Fft.h"
#include "dsp/MasterChain.h"
#include "dsp/Meters.h"
#include "dsp/Mix.h"
#include "dsp/NoteEdit.h"
#include "dsp/Phaser.h"
#include "dsp/Presets.h"
#include "dsp/Resampler.h"
#include "dsp/ResonanceFinder.h"
#include "dsp/Sampler.h"
#include "dsp/Space.h"
#include "dsp/Synth.h"
#include "dsp/TimeStretch.h"
#include "dsp/TunerPitch.h"

namespace
{
    int failures = 0;
    constexpr double twoPi = 6.283185307179586;

    void expect (const char* name, bool ok, const char* fmt = "", double a = 0, double b = 0, double c = 0)
    {
        std::printf ("%s %-70s ", ok ? "PASS" : "FAIL", name);
        std::printf (fmt, a, b, c);
        std::printf ("\n");
        if (! ok) ++failures;
    }

    std::vector<float> sine (int n, double fs, double hz, double amp = 0.5)
    {
        std::vector<float> x ((size_t) n);
        for (int i = 0; i < n; ++i) x[(size_t) i] = (float) (amp * std::sin (twoPi * hz * i / fs));
        return x;
    }

    double rms (const float* x, int n)
    {
        double s = 0;
        for (int i = 0; i < n; ++i) s += (double) x[i] * x[i];
        return std::sqrt (s / std::max (1, n));
    }

    // The pitch of a stretch of audio, Hz (0 for none)
    double pitchOf (const float* x, int n, double fs)
    {
        const int stride = std::max (1, (int) std::round (fs / 24000.0));
        const auto d = vs::boxDownsample (x, n, stride);
        return vs::TunerPitch::estimate (d.data(), (int) d.size(), fs / stride);
    }

    void helpers()
    {
        // gain stage: the same as the loop each plugin had
        {
            vs::Ramp<true> a, b;
            a.reset (48000, 0.05); b.reset (48000, 0.05);
            a.setTarget (0.5f); b.setTarget (0.5f);
            auto x = sine (4800, 48000, 440), y = x, z = x;
            float* ch[2] { y.data(), z.data() };
            vs::mix::applyGain (a, ch, 2, 4800);
            bool same = true;
            for (int i = 0; i < 4800; ++i) { const float g = b.next(); same = same && y[(size_t) i] == x[(size_t) i] * g && z[(size_t) i] == x[(size_t) i] * g; }
            expect ("a ramping gain is applied sample by sample, one value for every channel", same);
            const auto half = vs::mix::sendLaw (0.5f), dry = vs::mix::sendLaw (0.0f), wet = vs::mix::sendLaw (1.0f);
            expect ("the send law: both at full in the middle, one alone at each end",
                    half.dry == 1.0f && half.wet == 1.0f && dry.dry == 1.0f && dry.wet == 0.0f && wet.dry == 0.0f && wet.wet == 1.0f);
        }
        // meters
        {
            float shown = -60.0f;
            vs::meters::step (shown, 0.0f, vs::meters::level);
            vs::meters::PeakMeter m;
            bool clip = false;
            m.step (-3.0f, 0); m.step (-20.0f, 100);
            const float afterFall = m.level();
            clip = m.step (0.0f, 200) && m.isClipped();
            expect ("meters: ease up by the rise, fall at most a step per frame, hold, latch a clip",
                    std::abs (shown + 24.0f) < 1e-4f && std::abs (afterFall + 3.7f) < 1e-4f && clip && m.hold() == 0.0f,
                    "(%.2f, %.2f)", shown, afterFall);
            expect ("meters: the scale puts 0 dB near the top and -60 at the bottom",
                    vs::meters::scalePosition (-60.0f) == 0.0f && vs::meters::scalePosition (6.0f) == 1.0f && vs::meters::scalePosition (0.0f) > 0.85f);
        }
        // note editing
        {
            namespace ne = vs::noteedit;
            const ne::StepGrid g { 0.25, 0.0 };
            expect ("note editing: quantize, transpose and velocity stay in range, steps cover a clip",
                    ne::quantize (1.13, 0.25) == 1.25 && ne::transpose (125, 5) == 127 && ne::transpose (2, -5) == 0
                        && ne::velocity (120, -1, 20) == 127 && ne::velocity (5, -1, -10) == 1 && ne::velocity (50, 100) == 100
                        && g.steps (4.0) == 16 && g.steps (4.1) == 17 && std::abs (g.hitLength() - 0.225) < 1e-12);
        }
        // delay times, loudness targets, resonance band, MP3 rate
        {
            expect ("delay times: a slap is fixed, notes follow the tempo (kept to 30..300 BPM)",
                    vs::Space::delaySeconds (vs::Space::slap, 140) == vs::Space::slapSeconds
                        && std::abs (vs::Space::delaySeconds (vs::Space::quarter, 120) - 0.5f) < 1e-6f
                        && std::abs (vs::Space::delaySeconds (vs::Space::quarter, 1000) - 0.2f) < 1e-6f);
            expect ("loudness targets: streaming, balanced, loud", vs::MasterChain::targetLufs (0) == -14.0f && vs::MasterChain::targetLufs (2) == -9.0f
                                                                     && vs::MasterChain::targetLufs (9) == -9.0f);
            const auto b = vs::bandFor ({ 2800.0f, 12.0f, -10.0f });
            expect ("a resonance's band: at its frequency, Q 5, the cut kept to 3..9 dB",
                    b.freq == 2800.0f && b.q == 5.0f && b.rangeDb == 9.0f && b.thresholdDb == -16.0f);
            expect ("an MP3's rate: 48 kHz for the 48 family and 32 kHz, else 44.1",
                    vs::mp3Rate (96000) == 48000 && vs::mp3Rate (32000) == 48000 && vs::mp3Rate (88200) == 44100 && vs::mp3Rate (22050) == 44100);
        }
        // the tuner's follower
        {
            vs::TunerFollow f;
            const float a = f.push (110.0f), b2 = f.push (112.0f), c = f.push (220.0f);
            for (int i = 0; i < 8; ++i) f.push (0.0f);
            const bool held = f.hasNote();
            f.push (0.0f);
            vs::TunerReading r; r.string = 2; r.cents = 1.0f;
            vs::TunerFollow g;
            int steady = 0;
            for (int i = 0; i < 6; ++i) steady = g.steady (r, 3.0f);
            expect ("tuner follow: smooths close readings, jumps to a new note, clears after silence, counts steady reads",
                    a == 110.0f && std::abs (b2 - (110.0f * 0.6f + 112.0f * 0.4f)) < 1e-4f && c == 220.0f && held && ! f.hasNote() && steady == 5);
        }
        // presets
        {
            const auto& master = vs::presets::forChain ("master");
            bool styles = master.size() == 6;
            for (const auto& p : master)
                for (const auto& v : p.values)
                    if (std::string (v.id) == "style") styles = styles && v.value >= 0 && v.value < 4;
            expect ("presets: five chains, each starting from a preset with nothing set",
                    vs::presets::chains().size() == 5 && vs::presets::forChain ("vox").front().values.empty() && vs::presets::forChain ("nope").empty() && styles);
            const auto& punk = master[2];
            double style = -1;
            for (const auto& v : punk.values) if (std::string (v.id) == "style") style = v.value;
            expect ("presets: Master's Punk / Loud picks the Heavy style", std::string (vs::MasterChain::style ((int) style).name) == "Heavy");
        }
    }

    void effects()
    {
        const double fs = 48000;
        // chorus: all dry at mix 0, moving copies at mix 1
        {
            vs::Chorus c;
            c.prepare (fs);
            vs::Chorus::Settings s; s.mix = 0.0f;
            c.setSettings (s);
            auto x = sine (9600, fs, 440), y = x;
            float* ch[1] { y.data() };
            c.process (ch, 1, 9600);
            expect ("chorus: at mix 0 the sound is untouched", y == x);
            s.mix = 1.0f; c.setSettings (s); c.reset();
            auto l = sine (48000, fs, 440), r = l;
            float* st[2] { l.data(), r.data() };
            c.process (st, 2, 48000);
            double diff = 0;
            for (int i = 24000; i < 48000; ++i) diff += std::abs (l[(size_t) i] - r[(size_t) i]);
            expect ("chorus: the two sides sweep apart (width)", diff > 100.0, "(%.1f)", diff);
        }
        // phaser: notches move, output stays bounded
        {
            vs::Phaser p;
            p.prepare (fs);
            p.setSettings ({ 5.0f, 0.4f, 0.7f });
            std::vector<float> x ((size_t) fs * 3);
            vs::Xorshift32 rng (7);
            for (auto& v : x) v = (float) (rng.uniform() * 0.4 - 0.2);
            auto y = x;
            float* ch[1] { y.data() };
            p.process (ch, 1, (int) y.size());
            float peak = 0;
            for (auto v : y) peak = std::max (peak, std::abs (v));
            // the level in a narrow band changes as the notches sweep past it
            auto bandLevel = [&] (int from)
            {
                double s = 0;
                for (int i = from; i < from + 4800; ++i) s += (double) y[(size_t) i] * y[(size_t) i];
                return s;
            };
            expect ("phaser: bounded, and it changes over time", peak < 2.0f && std::abs (bandLevel (24000) - bandLevel (72000)) > 1e-3, "(peak %.2f)", peak);
        }
        // pitch shift: up an octave
        {
            vs::PitchShift ps;
            ps.prepare (fs, 1, 1024);
            ps.setSemitones (12.0f);
            auto x = sine ((int) fs * 2, fs, 220, 0.4);
            for (int at = 0; at < (int) x.size(); at += 1024)
            {
                float* ch[1] { x.data() + at };
                ps.process (ch, 1, std::min (1024, (int) x.size() - at));
            }
            const double hz = pitchOf (x.data() + (int) fs, 8192, fs);
            expect ("pitch shift: +12 semitones turns 220 Hz into 440 Hz", std::abs (hz - 440.0) < 3.0 && ps.latency() > 0, "(%.1f Hz, latency %.0f)", hz, ps.latency());
        }
        // time stretch: twice as long, same pitch
        {
            vs::TimeStretch t;
            t.prepare (fs, 1, 4096);
            auto x = sine ((int) fs, fs, 330, 0.4);
            std::vector<float> y ((size_t) fs * 2, 0.0f);
            for (int i = 0; i < 24; ++i)
            {
                const float* in[1] { x.data() + i * 2000 };
                float* out[1] { y.data() + i * 4000 };
                t.process (in, 2000, out, 4000);
            }
            const double hz = pitchOf (y.data() + 48000, 8192, fs);
            expect ("time stretch: twice as long keeps the pitch (330 Hz)", std::abs (hz - 330.0) < 3.0 && rms (y.data() + 48000, 8192) > 0.1, "(%.1f Hz)", hz);
        }
        // compressor, set by hand
        {
            vs::Compressor c;
            c.prepare (fs);
            c.setDetail ({ -20.0f, 4.0f, 1.0f, 50.0f, 0.0f, false });
            auto x = sine ((int) fs, fs, 1000, 0.5);   // about -6 dB peaks
            float* ch[1] { x.data() };
            c.process (ch, 1, (int) x.size());
            float peak = 0;
            for (int i = (int) fs / 2; i < (int) fs; ++i) peak = std::max (peak, std::abs (x[(size_t) i]));
            const float db = vs::ampToDb (peak);
            expect ("compressor set by hand: -6 dB peaks over a -20 dB threshold at 4:1 come out near -16.5 dB", std::abs (db + 16.5f) < 1.5f, "(%.2f dB)", db);
        }
    }

    void instruments()
    {
        const double fs = 48000;
        // synth: A4 sounds at 440 Hz, the release ends it, voices stack
        {
            vs::Synth s;
            s.prepare (fs);
            vs::Synth::Settings set;
            set.osc[0].wave = vs::Oscillator::Wave::sine;
            set.filter.type = vs::Synth::FilterType::off;
            s.setSettings (set);
            s.noteOn (69, 1.0f);
            std::vector<float> l ((size_t) fs, 0.0f), r ((size_t) fs, 0.0f);
            s.render (l.data(), r.data(), (int) fs);
            const double hz = pitchOf (l.data() + 24000, 8192, fs);
            s.noteOff (69);
            std::vector<float> l2 ((size_t) fs, 0.0f), r2 ((size_t) fs, 0.0f);
            s.render (l2.data(), r2.data(), (int) fs);
            expect ("synth: A4 is 440 Hz, and its release ends the note", std::abs (hz - 440.0) < 1.0 && ! s.isSounding()
                                                                             && rms (l2.data() + 40000, 8000) < 1e-6, "(%.2f Hz)", hz);
            float* lp = l.data(); (void) lp;
            set.osc[0].wave = vs::Oscillator::Wave::sawUp;
            set.filter.type = vs::Synth::FilterType::lowPass;
            set.filter.hz = 500.0f;
            s.setSettings (set);
            s.noteOn (48, 0.8f); s.noteOn (52, 0.8f); s.noteOn (55, 0.8f);
            std::fill (l.begin(), l.end(), 0.0f); std::fill (r.begin(), r.end(), 0.0f);
            s.render (l.data(), r.data(), 24000);
            float peak = 0;
            for (int i = 0; i < 24000; ++i) peak = std::max (peak, std::abs (l[(size_t) i]));
            expect ("synth: a filtered three-note chord plays, bounded", peak > 0.05f && peak < 2.0f, "(peak %.2f)", peak);
        }
        // sampler
        {
            vs::Sampler s;
            s.prepare (fs);
            vs::Sampler::Sound snd;
            snd.left = sine ((int) fs, fs, 220, 0.5);
            snd.rootNote = 57; snd.lowNote = 40; snd.highNote = 80; snd.oneShot = false; snd.choke = 1;
            s.addSound (snd);
            s.noteOn (69, 1.0f);   // an octave over the root
            std::vector<float> l (16384, 0.0f), r (16384, 0.0f);
            s.render (l.data(), r.data(), 16384);
            const double hz = pitchOf (l.data() + 4096, 8192, fs);
            s.noteOff (69);
            std::vector<float> l2 (8192, 0.0f), r2 (8192, 0.0f);
            s.render (l2.data(), r2.data(), 8192);
            expect ("sampler: an octave over the root plays an octave up, and a note-off releases it",
                    std::abs (hz - 440.0) < 2.0 && ! s.isSounding() && std::abs (l[100] - r[100]) < 1e-6f, "(%.1f Hz)", hz);
            s.noteOn (57, 1.0f); s.noteOn (59, 1.0f);   // the second chokes the first (same group)
            std::fill (l.begin(), l.end(), 0.0f); std::fill (r.begin(), r.end(), 0.0f);
            s.render (l.data(), r.data(), 16384);
            const double hz2 = pitchOf (l.data() + 8192, 8192, fs);
            expect ("sampler: a sound in the same choke group cuts the one before", std::abs (hz2 - 220.0 * std::pow (2.0, 2.0 / 12.0)) < 2.0, "(%.1f Hz)", hz2);
        }
    }
}

int main()
{
    helpers();
    effects();
    instruments();
    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
