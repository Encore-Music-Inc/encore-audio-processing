// The metronome (dsp/Metronome.h) and the drum synthesizer (dsp/DrumSynth.h),
// built without JUCE as the browser build is. Each check prints PASS or FAIL;
// the suite exits non-zero on any failure.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "dsp/DrumSynth.h"
#include "dsp/Metronome.h"

namespace
{
    int failures = 0;
    namespace m = vs::metronome;
    namespace d = vs::drumsynth;
    using V = std::vector<int>;

    void expect (const std::string& name, bool ok, const std::string& detail = "")
    {
        std::printf ("%s %-62s %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
        if (! ok) ++failures;
    }

    std::string num (double v) { char b[48]; std::snprintf (b, sizeof b, "%.6g", v); return b; }

    float peak (const std::vector<float>& x)
    {
        float p = 0;
        for (float v : x) p = std::max (p, std::abs (v));
        return p;
    }

    double absSum (const std::vector<float>& x)
    {
        double s = 0;
        for (float v : x) s += std::abs ((double) v);
        return s;
    }

    // Upward zero crossings in [a, b) samples, as Hz
    double crossingHz (const std::vector<float>& x, size_t a, size_t b, double sr)
    {
        std::vector<double> at;
        for (size_t i = a + 1; i < std::min (b, x.size()); ++i)
            if (x[i - 1] < 0 && x[i] >= 0) at.push_back ((double) (i - 1) + x[i - 1] / (x[i - 1] - x[i]));
        return at.size() < 2 ? 0 : (double) (at.size() - 1) * sr / (at.back() - at.front());
    }

    // ---- the beat grid

    void meters()
    {
        std::string opts;
        for (auto& g : m::groupingOptions (7, 8)) opts += m::groupsLabel (g) + ";";
        expect ("7/8 offers every count, then 2+2+3, 2+3+2, 3+2+2", opts == "Every count;2+2+3;2+3+2;3+2+2;", opts);
        expect ("default feel: 6/8 in threes, 5/8 as 3+2, 7/8 as 2+2+3, 4/4 every count",
                m::defaultGroups (6, 8) == V { 3, 3 } && m::defaultGroups (5, 8) == V { 3, 2 } && m::defaultGroups (7, 8) == V { 2, 2, 3 }
                    && m::defaultGroups (4, 4) == V { 1, 1, 1, 1 });
        expect ("parseGroups: 3+2+2 for 7, nothing when the sum is wrong",
                m::parseGroups ("3+2+2", 7) == V { 3, 2, 2 } && ! m::parseGroups ("3+2", 7) && ! m::parseGroups ("0+7", 7) && ! m::parseGroups ("", 7));
        expect ("parseGroups: a run of digits too long for a number is no grouping", ! m::parseGroups (std::string (400, '9'), 9));
        expect ("accents: downbeat 3, group starts 2, the rest 1", m::defaultAccents ({ 2, 2, 3 }) == V { 3, 1, 2, 1, 2, 1, 1 });
        expect ("note values: 1 of 8, 3 of 8, 2 of 4, 5 of 8",
                m::noteValueName (1, 8) == "8th" && m::noteValueName (3, 8) == "dotted quarter" && m::noteValueName (2, 4) == "half"
                    && m::noteValueName (5, 8) == "5 \xC3\x97 8th");
        const auto u = m::unitOptions ({ 3, 3 }, 8);
        expect ("6/8 counts in 8ths or dotted quarters", u.size() == 2 && u[1].units == 3 && u[1].name == "dotted quarter");
        expect ("tempo names", std::string (m::tempoName (119.9)) == "Moderato" && std::string (m::tempoName (120)) == "Allegro"
                                   && std::string (m::tempoName (500)) == "Prestissimo");
        const auto sw = m::subFractions ("16", 66.7);
        expect ("16ths with swing", sw.size() == 4 && std::abs (sw[1] - 0.3335) < 1e-12 && sw[2] == 0.5 && std::abs (sw[3] - 0.8335) < 1e-12);
        expect ("an unknown subdivision is none; swing is kept to 50..75",
                m::subFractions ("x").size() == 1 && m::subFractions ("8", 90)[1] == 0.75 && m::subFractions ("8", 0)[1] == 0.5);
        expect ("bpm kept to 20..400, 0 is 120", m::clampBpm (5) == 20 && m::clampBpm (900) == 400 && m::clampBpm (0) == 120);
    }

    void trainerAndGap()
    {
        m::TrainerSettings steps { m::TrainerMode::steps, 80, 4, 2, 92, false, 5 };
        expect ("steps: +4 every 2 bars, then stays at 92",
                *m::trainerTempo (steps, 0, 0) == 80 && *m::trainerTempo (steps, 3, 0) == 84 && *m::trainerTempo (steps, 7, 0) == 92
                    && *m::trainerTempo (steps, 40, 0) == 92);
        steps.loop = true;
        expect ("steps with loop start again", *m::trainerTempo (steps, 8, 0) == 80);
        m::TrainerSettings ramp { m::TrainerMode::ramp, 100, 1, 1, 160, false, 1 };
        expect ("ramp: halfway after 30 s of 1 minute, then stays", *m::trainerTempo (ramp, 0, 30) == 130 && *m::trainerTempo (ramp, 0, 300) == 160);
        expect ("off is nothing", ! m::trainerTempo ({}, 3, 3));
        const m::GapSettings gap { true, 2, 1 };
        expect ("gap: 2 bars on, 1 off", ! m::isGapBar (gap, 0) && ! m::isGapBar (gap, 1) && m::isGapBar (gap, 2) && ! m::isGapBar (gap, 3));
    }

    void cleaning()
    {
        m::MetronomeSettings s;
        s.top = 7; s.bottom = 8; s.bpm = 1000; s.swing = 90; s.randomMute = -5; s.sub = "nope"; s.poly.n = 40; s.gap.play = 0;
        const auto c = m::cleanSettings (s);
        expect ("cleanSettings mends a bad preset",
                c.groups == V { 2, 2, 3 } && c.accents == V { 3, 1, 2, 1, 2, 1, 1 } && c.bpm == 400 && c.swing == 75 && c.randomMute == 0
                    && c.sub == "none" && c.poly.n == 16 && c.gap.play == 1);
        m::MetronomeSettings t;
        t.top = 3; t.bottom = 3; t.units = 2;
        const auto c2 = m::cleanSettings (t);
        expect ("bottom only 2, 4, 8 or 16; units must fit the grouping", c2.bottom == 4 && c2.units == 1 && c2.accents == V { 3, 2, 2 });
    }

    // ---- the sequencer

    void sequencer()
    {
        m::Sequencer q (48000);
        bool ok = true;
        for (int i = 0; i < 8; ++i)
        {
            const auto e = q.next();
            ok = ok && e.frame == i * 24000.0 && e.len == 24000 && e.bar == i / 4 && e.count == i % 4 && e.level == (i % 4 == 0 ? 3 : 2);
        }
        expect ("120 bpm 4/4: a count every 24000 frames, accent on 1", ok);

        m::MetronomeSettings s;
        s.accents = { 3, 0, 0, 0 };
        m::Sequencer ci (44100, s);
        ci.start (100, { true, 1 });
        const auto first = ci.next();
        expect ("count-in: bar -1, silent counts sound, starts at the frame given", first.bar == -1 && first.countIn && first.frame == 100 && ci.next().level == 1);

        m::Sequencer ch (48000);
        ch.next();
        m::MetronomeSettings w;
        w.top = 3; w.bpm = 60;
        ch.set (w);
        const auto mid = ch.next();
        expect ("mid-bar: tempo changes now, the meter at the next bar", mid.top == 4 && mid.len == 48000);
        ch.next(); ch.next();
        expect ("the new meter from the next bar", ch.next().top == 3);

        m::MetronomeSettings p;
        p.poly = { true, 3 };
        m::Sequencer pq (48000, p);
        std::string fr;
        for (int i = 0; i < 4; ++i) for (auto& c : pq.next().poly) fr += std::to_string (c.j) + "@" + num (c.frac) + " ";
        expect ("3 over 4: the polyrhythm clicks land in counts 1, 2 and 3", fr == "0@0 1@0.333333 2@0.666667 ", fr);

        m::MetronomeSettings r;
        r.randomMute = 50;
        auto muted = [&] (std::uint32_t seed)
        {
            m::Sequencer x (48000, r);
            x.start (0, { false, seed });
            std::string out;
            for (int i = 0; i < 32; ++i) out += x.next().muted ? '1' : '0';
            return out;
        };
        const auto a = muted (7);
        expect ("random muting repeats for a seed", a == muted (7) && a != muted (8) && a.find ('1') != std::string::npos && a.find ('0') != std::string::npos, a);
    }

    // ---- the click sounds and the engine

    void clickSounds()
    {
        std::set<std::string> names;
        bool ok = true;
        for (auto& set : m::soundSets)
        {
            names.insert (set.name);
            const auto s = m::makeSounds (set.id, 48000);
            ok = ok && std::abs (peak (s.accent) - 1.0f) < 1e-6f && std::abs (peak (s.beat) - 0.8f) < 1e-6f && std::abs (peak (s.sub) - 0.55f) < 1e-6f
                 && std::abs (peak (s.poly) - 0.75f) < 1e-6f && (s.alt.empty() || std::abs (peak (s.alt) - 0.75f) < 1e-6f);
            // each starts loud on its first samples, so the click is where it was scheduled
            float early = 0;
            for (size_t i = 0; i < 4; ++i) early = std::max (early, std::abs (s.beat[i]));
            ok = ok && early >= 0.3f * peak (s.beat) - 1e-6f;
        }
        expect ("9 sound sets, one name each, each sound at its level, loud at once", names.size() == 9 && std::size (m::soundSets) == 9 && ok);
        // at low sample rates too, where a hat's filters would sit above Nyquist
        bool low = true;
        for (double sr : { 16000.0, 22050.0 })
            for (auto& set : m::soundSets)
            {
                const auto s = m::makeSounds (set.id, sr);
                for (const auto* b : { &s.accent, &s.beat, &s.sub, &s.poly, &s.alt })
                    for (float v : *b) low = low && std::isfinite (v);
                low = low && std::abs (peak (s.accent) - 1.0f) < 1e-6f && std::abs (peak (s.beat) - 0.8f) < 1e-6f;
            }
        expect ("every set at 16 and 22.05 kHz: finite, at its levels", low);
        expect ("an unknown set is the classic click", m::makeSounds ("nope", 48000).beat == m::makeSounds ("classic", 48000).beat);
        expect ("only Kick and hat has an alt sound", ! m::makeSounds ("groove", 48000).alt.empty() && m::makeSounds ("classic", 48000).alt.empty());

        const auto clave = m::makeSounds ("clave", 48000), tick = m::makeSounds ("softtick", 48000);
        const double cb = crossingHz (clave.beat, 0, clave.beat.size(), 48000), ca = crossingHz (clave.accent, 0, clave.accent.size(), 48000);
        const double tb = crossingHz (tick.beat, 0, tick.beat.size(), 48000), ta = crossingHz (tick.accent, 0, tick.accent.size(), 48000);
        expect ("Clave rings at 2500 Hz, its beat one at 3750", std::abs (cb - 2500) < 5 && std::abs (ca - 3750) < 5, num (cb) + " " + num (ca));
        expect ("Soft tick rings at 1300 Hz, its beat one at 1950", std::abs (tb - 1300) < 5 && std::abs (ta - 1950) < 10, num (tb) + " " + num (ta));
        // decays: 18 ms and 6 ms time constants, so 20 dB down after 41 ms and 14 ms
        auto at = [] (const std::vector<float>& x, double ms) { float p = 0; for (size_t i = (size_t) (ms * 48); i < (size_t) (ms * 48) + 48 && i < x.size(); ++i) p = std::max (p, std::abs (x[i])); return p; };
        expect ("Clave and Soft tick decay as before (exp(-t / 18 ms), exp(-t / 6 ms))",
                std::abs (at (clave.beat, 41.4) / peak (clave.beat) - 0.1f) < 0.02f && std::abs (at (tick.beat, 13.8) / peak (tick.beat) - 0.1f) < 0.02f);
    }

    std::vector<float> run (m::ClickEngine& e, int blocks, int block, std::vector<std::vector<m::CountEvent>>* events = nullptr)
    {
        std::vector<float> out ((size_t) (blocks * block));
        for (int b = 0; b < blocks; ++b)
        {
            const auto& ev = e.render (out.data() + (size_t) (b * block), block, (std::int64_t) b * block);
            if (events) events->push_back (ev);
        }
        return out;
    }

    void engine()
    {
        m::ClickEngine e (48000);
        e.setSounds (m::makeSounds ("classic", 48000));
        e.set ({});
        e.start (480);
        auto out = run (e, 400, 128);
        bool silentBefore = true;
        for (size_t i = 0; i < 480; ++i) silentBefore = silentBefore && out[i] == 0;
        expect ("the first click starts on its frame, not before", silentBefore && out[480] != 0 && out[480 + 24000] != 0 && out[480 + 23999] == 0);
        expect ("accent at slider 100, beats at 80 (0.64)", std::abs (peak ({ out.begin() + 480, out.begin() + 4000 }) - 1.0f) < 1e-6f
                                                              && std::abs (peak ({ out.begin() + 24480, out.begin() + 28000 }) - 0.8f * 0.64f) < 1e-6f);

        m::ClickEngine f (48000);
        f.setSounds (m::makeSounds ("groove", 48000));
        m::MetronomeSettings s;
        s.sub = "16"; s.swing = 60; s.poly = { true, 3 }; s.randomMute = 20; s.bpm = 173.3;
        f.set (s);
        f.start (37.5, { true, 3 });
        const auto a = run (f, 3000, 128);
        f.start (37.5, { true, 3 });
        const auto b = run (f, 384, 1000);
        expect ("the same audio whatever the block size", a == b);

        std::vector<std::vector<m::CountEvent>> ev;
        m::ClickEngine g (48000);
        g.setSounds (m::makeSounds ("beep", 48000));
        g.set ({});
        g.start (0);
        run (g, 200, 128, &ev);
        int counts = 0;
        for (auto& v : ev) counts += (int) v.size();
        g.stop();
        std::vector<float> tail (128 * 20);
        for (int k = 0; k < 20; ++k) g.render (tail.data() + k * 128, 128, 25600 + k * 128);
        expect ("render hands out each count once; stop lets the last click ring out",
                counts == 2 && ev[0].size() == 1 && ev[187].size() == 1 && tail[0] != 0 && peak ({ tail.begin() + 1280, tail.end() }) == 0);
    }

    // The four ways a desktop click marks beat one, with this engine's accents
    void beatOneModes()
    {
        auto bar = [] (const m::Sounds& s, std::vector<int> accents)
        {
            m::ClickEngine e (48000);
            e.setSounds (s);
            m::MetronomeSettings st;
            st.accents = std::move (accents);
            e.set (st);
            e.start (0);
            const auto out = run (e, 750, 128);   // two counts
            return std::pair<std::vector<float>, std::vector<float>> { { out.begin(), out.begin() + 24000 }, { out.begin() + 24000, out.begin() + 48000 } };
        };
        const auto clave = m::makeSounds ("clave", 48000);
        const auto higher = bar (clave, { 3, 2, 2, 2 });
        expect ("higher click on beat 1: accents 3 2 2 2 (the set's higher accent)",
                std::abs (crossingHz (higher.first, 0, 4000, 48000) - 3750) < 10 && std::abs (crossingHz (higher.second, 0, 4000, 48000) - 2500) < 10);
        const auto louder = bar (clave, { 2, 1, 1, 1 });
        expect ("louder click on beat 1: accents 2 1 1 1 (the others at 0.4)", std::abs (peak (louder.second) / peak (louder.first) - 0.4f) < 1e-5f
                                                                                && std::abs (crossingHz (louder.first, 0, 4000, 48000) - 2500) < 10);
        auto withCowbell = clave;
        withCowbell.accent = m::makeSounds ("cowbell", 48000).accent;
        const auto other = bar (withCowbell, { 3, 2, 2, 2 });
        expect ("cowbell on beat 1: accents 3 2 2 2 with the cowbell set's accent",
                std::equal (withCowbell.accent.begin(), withCowbell.accent.end(), other.first.begin()));
        const auto same = bar (clave, { 2, 2, 2, 2 });
        expect ("same click on every beat: accents 2 2 2 2", same.first == same.second);
    }

    // ---- the drums

    void kits()
    {
        bool ok = true;
        std::set<std::string> ids, kitNames;
        for (auto& s : d::sounds()) ids.insert (s.id);
        for (auto& k : d::kits())
        {
            kitNames.insert (k.name);
            for (auto& p : k.pads) ok = ok && d::findSound (p.sound) != nullptr;
        }
        expect ("7 kits, every pad has a sound, every id once", d::kits().size() == 7 && kitNames.size() == 7 && ok && ids.size() == d::sounds().size());
        bool levels = true;
        for (auto& s : d::sounds())
        {
            const auto x = d::renderSound (s.id, 44100);
            levels = levels && ! x.empty() && std::abs (peak (x) - (float) s.level) < 1e-6f;
        }
        expect ("every sound renders at its kit level", levels);
        expect ("an unknown id is nothing", d::renderSound ("nope", 48000).empty());
        expect ("names: the kit's and the drum's",
                d::soundName ("boomKick") == "Boom: Kick" && d::soundName ("kick") == "Kick" && d::soundName ("studioFloorTom") == "Studio: Low floor tom"
                    && d::soundName ("rim") == "Side stick" && d::soundName ("shaker") == "Shaker");
        const auto* encore = d::findKit ("encore");
        expect ("a note without a pad plays the pad standing in for it (ride on the crash)", encore && std::string (d::kitSound (*encore, 51)) == "crash"
                                                                                           && d::kitSound (*encore, 81) == nullptr);
    }

    struct DrumPrint { const char* id; int length; double absSum; };
    struct ClickPrint { const char* set; const char* role; int length; double absSum; };

    // Length and sum of |x| of every sound at 48 kHz, as they are made now,
    // so a change to any of them shows here
    void fingerprints()
    {
        static const DrumPrint drums[] {
            { "boomClap", 28558, 2003.71172 },
            { "boomClosedHat", 5760, 182.38003 },
            { "boomCowbell", 21600, 479.034874 },
            { "boomHighTom", 33600, 7779.50261 },
            { "boomKick", 72000, 20389.2581 },
            { "boomLowTom", 43200, 10688.0431 },
            { "boomOpenHat", 38398, 1635.99258 },
            { "boomRim", 2880, 78.9695099 },
            { "boomSnare", 10799, 1804.73278 },
            { "clap", 18457, 975.872749 },
            { "closedHat", 7680, 231.094683 },
            { "clubClap", 23519, 1673.54353 },
            { "clubClosedHat", 5759, 185.525445 },
            { "clubCrash", 105600, 2839.50922 },
            { "clubKick", 24000, 10636.3053 },
            { "clubOpenHat", 33600, 1331.64101 },
            { "clubRide", 105599, 2066.52593 },
            { "clubRim", 3840, 78.4988616 },
            { "clubSnare", 24480, 4834.14575 },
            { "clubTom", 28800, 7439.82553 },
            { "crash", 115200, 3242.00504 },
            { "highTom", 24960, 7982.13946 },
            { "kick", 28800, 13258.1161 },
            { "lowTom", 37440, 12240.8449 },
            { "openHat", 45600, 1748.05457 },
            { "rim", 6720, 521.761243 },
            { "shaker", 5760, 229.465288 },
            { "snare", 18720, 3045.39358 },
            { "808Clap", 21839, 2130.76959 },
            { "808ClosedHat", 6048, 547.085424 },
            { "808Crash", 105600, 20139.464 },
            { "808FloorTom", 60000, 21726.8108 },
            { "808HighTom", 60000, 21843.5393 },
            { "808Kick", 129600, 40814.8159 },
            { "808LowTom", 60000, 21804.9854 },
            { "808OpenHat", 23519, 3682.8908 },
            { "808Ride", 76800, 7867.10219 },
            { "808Snare", 19200, 3151.9619 },
            { "lofiClap", 21838, 1907.50237 },
            { "lofiClosedHat", 7776, 621.229088 },
            { "lofiCrash", 105600, 15448.9515 },
            { "lofiFloorTom", 33600, 10160.5458 },
            { "lofiHighTom", 33600, 10279.5861 },
            { "lofiKick", 43199, 12735.0691 },
            { "lofiLowTom", 33600, 10158.6173 },
            { "lofiOpenHat", 30240, 3830.90478 },
            { "lofiRide", 76797, 7422.80833 },
            { "lofiSnare", 19200, 2510.14338 },
            { "rockClap", 21840, 2490.1268 },
            { "rockClosedHat", 11232, 1379.09307 },
            { "rockCrash", 105600, 27026.4677 },
            { "rockFloorTom", 48000, 20840.1834 },
            { "rockHighTom", 48000, 20820.4666 },
            { "rockKick", 31680, 11995.312 },
            { "rockLowTom", 48000, 20811.7023 },
            { "rockOpenHat", 43679, 9191.13615 },
            { "rockRide", 76800, 10448.9141 },
            { "rockSnare", 19200, 7914.60293 },
            { "studioClap", 21840, 1507.56612 },
            { "studioClosedHat", 8640, 479.739685 },
            { "studioCrash", 105600, 9045.18658 },
            { "studioFloorTom", 33600, 6316.54109 },
            { "studioHighTom", 33600, 6177.85641 },
            { "studioKick", 46080, 4404.79133 },
            { "studioLowTom", 33600, 6046.89304 },
            { "studioOpenHat", 33600, 2983.53115 },
            { "studioRide", 76800, 5169.32014 },
            { "studioSnare", 19200, 1305.7326 },
        };
        static const ClickPrint clicks[] {
            { "beep", "accent", 3360, 1945.00524 },
            { "beep", "beat", 2880, 1312.15631 },
            { "beep", "poly", 2400, 1000.48518 },
            { "beep", "sub", 1440, 398.212826 },
            { "classic", "accent", 2880, 209.949519 },
            { "classic", "beat", 2400, 143.439726 },
            { "classic", "poly", 2400, 146.932539 },
            { "classic", "sub", 1440, 76.9306401 },
            { "cowbell", "accent", 24000, 936.592585 },
            { "cowbell", "beat", 19200, 560.501922 },
            { "cowbell", "poly", 16800, 392.225422 },
            { "cowbell", "sub", 7200, 168.466913 },
            { "groove", "accent", 19200, 4660.6755 },
            { "groove", "alt", 5754, 387.558784 },
            { "groove", "beat", 14400, 2706.35065 },
            { "groove", "poly", 8640, 377.708988 },
            { "groove", "sub", 2878, 122.548196 },
            { "hihat", "accent", 21594, 1628.94967 },
            { "hihat", "beat", 5754, 372.347754 },
            { "hihat", "poly", 7200, 457.682591 },
            { "hihat", "sub", 2878, 122.548196 },
            { "rim", "accent", 2880, 150.18142 },
            { "rim", "beat", 2400, 96.0885752 },
            { "rim", "poly", 2400, 117.392143 },
            { "rim", "sub", 1440, 39.6210553 },
            { "woodblock", "accent", 7200, 537.924593 },
            { "woodblock", "beat", 6240, 395.342948 },
            { "woodblock", "poly", 5760, 363.944357 },
            { "woodblock", "sub", 3840, 178.759285 },
            { "clave", "accent", 6240, 551.367561 },
            { "clave", "beat", 6240, 441.908009 },
            { "clave", "poly", 5760, 369.74606 },
            { "clave", "sub", 3360, 153.232999 },
            { "softtick", "accent", 2160, 187.059048 },
            { "softtick", "beat", 2160, 151.031799 },
            { "softtick", "poly", 2160, 140.691279 },
            { "softtick", "sub", 1440, 70.8446698 },
        };
        const double sumTolerance = 1e-5;
        int bad = 0;
        std::string first;
        for (auto& p : drums)
        {
            const auto x = d::renderSound (p.id, 48000);
            if ((int) x.size() != p.length || std::abs (absSum (x) - p.absSum) > sumTolerance * p.absSum)
            {
                if (! bad++) first = std::string (p.id) + " " + std::to_string (x.size()) + " " + num (absSum (x));
            }
        }
        expect ("every drum as made before (length, sum of |x|)", bad == 0, first);
        bad = 0;
        for (auto& p : clicks)
        {
            const auto s = m::makeSounds (p.set, 48000);
            const std::string r = p.role;
            const auto& x = r == "accent" ? s.accent : r == "beat" ? s.beat : r == "sub" ? s.sub : r == "poly" ? s.poly : s.alt;
            if ((int) x.size() != p.length || std::abs (absSum (x) - p.absSum) > sumTolerance * p.absSum)
            {
                if (! bad++) first = std::string (p.set) + " " + r + " " + std::to_string (x.size()) + " " + num (absSum (x));
            }
        }
        expect ("every click sound as made before (length, sum of |x|)", bad == 0, first);
    }

    // The Style kits keep the character of the desktop kits they come from
    void styleKits()
    {
        const double sr = 48000;
        bool ok = true;
        std::string detail;
        for (const auto& st : d::styles)
        {
            const auto kick = d::renderSound (std::string (st.id) + "Kick", sr);
            const double late = crossingHz (kick, (size_t) (0.2 * sr), (size_t) (0.3 * sr), sr);
            const auto tom = d::renderSound (std::string (st.id) + "LowTom", sr);
            const double tomLate = crossingHz (tom, (size_t) (0.15 * sr), (size_t) (0.25 * sr), sr);
            const bool good = std::abs (late - st.kickLo) < 2 && std::abs (tomLate - 124 * st.tomPitch) < 3;
            if (! good) detail += std::string (st.name) + " " + num (late) + " " + num (tomLate) + " ";
            ok = ok && good;
        }
        expect ("kicks settle on their low pitch, toms on theirs", ok, detail);
        // Lo-fi: before its low-pass, every sample is a multiple of 1/128 held for 3 samples
        auto x = d::cymbal (sr, 0.1, { 1.7, 5600, 12320, 0.035, 0.035, 1.0, 0.5, {}, 7, 0 });
        auto st = d::styles[3];
        st.lowpass = 0;
        d::styleStages (x, sr, st);
        bool crushed = true;
        for (size_t i = 0; i < x.size(); ++i)
            crushed = crushed && std::abs (x[i] * 128 - std::round (x[i] * 128)) < 1e-4f && (i % 3 == 0 || x[i] == x[i - 1]);
        expect ("Lo-fi crushes to 8 bits and holds each value for 3 samples", crushed);
    }
}

int main()
{
    meters();
    trainerAndGap();
    cleaning();
    sequencer();
    clickSounds();
    engine();
    beatOneModes();
    kits();
    fingerprints();
    styleKits();
    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "SOME FAILED", failures);
    return failures == 0 ? 0 : 1;
}
