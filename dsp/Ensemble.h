#pragma once

// A score played by its instruments: each note of a performance (its time,
// length, pitch, velocity and where a held note's swell ends, its part, its
// instrument, and how it's played) through its part's instrument, a synth
// sound (Instruments.h) or a recorded instrument (the sampler's zones, played
// on the synth sound of its family for a note whose recordings haven't come),
// then each part at its volume and pan, a small hall (the reverb), and the
// mix through a gentle compressor. A whole score renders at once, brought to
// -1 dB sample peak (or left as played, so stems add up to the mix).
//
//     vs::Ensemble e; e.prepare (48000);
//     const int piano = e.addSynth ("piano");
//     e.setParts ({ { 0.8f, 0.0f } });
//     auto [l, r] = e.render (notes);
//
// Or live, on a running clock in frames (an AudioWorklet's): startLive, then
// notes scheduled ahead as a score plays (a note's time is seconds on that
// clock), notes held while a key is down (no length: until release), what
// hasn't started forgotten when playback stops, the metronome's classic click
// beside the mix, and renderLive block by block. The same parts, hall and
// compressor play a score live and render it.

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "Channels.h"
#include "Compressor.h"
#include "Instruments.h"
#include "Metronome.h"
#include "Mix.h"
#include "ReverbEngine.h"

namespace vs
{
    class Ensemble
    {
    public:
        // A technique that changes a note's sound
        enum class Technique { none, pizz, harmonic, bright, dark };

        struct Note
        {
            double time = 0.0, dur = 0.0;    // seconds
            int midi = 60;
            float velocity = 90.0f;          // 1 .. 127
            float velocityEnd = 0.0f;        // where a held note's swell ends (0: none)
            int part = 0, instrument = 0;
            bool slurred = false, muted = false;
            Technique technique = Technique::none;
            int bendTo = -1;                 // a glide to this note (-1: none)...
            float bendAt = 0.35f;            // ...starting this far through the note
            float gain = 1.0f;
        };

        struct Part { float volume = 0.8f, pan = 0.0f; };   // volume 0.8: as recorded

        // When a rendered score starts, and how long after its last note it runs on (reverb, releases)
        static constexpr double leadSeconds = 0.05, tailSeconds = 2.5;
        // The small hall each part is sent to, at this level
        static constexpr float reverbSend = 0.35f, reverbDecay = 3.6f;

        void prepare (double sampleRate) { fs = sampleRate; }

        // A synth sound for a family (Instruments.h); returns the instrument, or -1 for one it doesn't know
        int addSynth (const std::string& family)
        {
            if (! instruments::synthSound (family)) return -1;
            defs.push_back ({ family, {}, nullptr, 0.0 });
            return (int) defs.size() - 1;
        }

        // A recorded instrument (its manifest and recordings, at a trim), with its family's synth sound for
        // any note whose recordings haven't come; returns the instrument, or -1 for a manifest it can't read
        int addRecorded (const std::string& manifest, instruments::Recordings recordings, double trimDb, const std::string& family)
        {
            Sampler probe;
            probe.prepare (fs);
            if (instruments::loadRecorded (probe, manifest, recordings, trimDb) < 0 || ! instruments::synthSound (family)) return -1;
            defs.push_back ({ family, manifest, std::move (recordings), trimDb });
            return (int) defs.size() - 1;
        }

        void setParts (std::vector<Part> p) { parts = std::move (p); }

        // How much of each part goes to the hall (reverbSend as heard in a score), and how long it rings
        void setReverb (float send, float decaySeconds) { sendLevel = send; decay = decaySeconds; }
        // Where the mix's compressor starts and how hard it holds the mix down
        void setCompressor (float thresholdDb, float ratio) { compThreshold = thresholdDb; compRatio = ratio; }

        // The notes, rendered: leadSeconds of silence, the score, tailSeconds more. Notes whose part
        // isn't in onlyParts (when given) are left out; normalize brings the mix to -1 dB sample peak.
        std::pair<std::vector<float>, std::vector<float>> render (std::vector<Note> notes, bool normalize = true, const std::vector<int>& onlyParts = {})
        {
            if (! onlyParts.empty())
                notes.erase (std::remove_if (notes.begin(), notes.end(), [&] (const Note& n) { return std::find (onlyParts.begin(), onlyParts.end(), n.part) == onlyParts.end(); }), notes.end());
            double length = 0.0;
            for (const auto& n : notes) length = std::max (length, n.time + n.dur);
            const auto frames = (std::int64_t) std::ceil (std::max (0.5, length + tailSeconds) * fs);
            std::vector<float> L ((size_t) frames, 0.0f), R ((size_t) frames, 0.0f);
            start (notes);
            std::int64_t at = 0;
            while (at < frames)
            {
                const int n = (int) std::min<std::int64_t> (block, frames - at);
                renderBlock (L.data() + at, R.data() + at, at, n);
                at += n;
            }
            if (normalize)
            {
                float* ch[2] { L.data(), R.data() };
                const float peak = channels::peakOf (ch, 2, (int) frames);
                if (peak > 0.0f)
                {
                    const float g = (float) (std::pow (10.0, -1.0 / 20.0) / peak);
                    for (auto& v : L) v *= g;
                    for (auto& v : R) v *= g;
                }
            }
            return { std::move (L), std::move (R) };
        }

        // ---- live

        // Ready to play live: the parts' buses, the hall and the compressor, nothing sounding or scheduled
        void startLive()
        {
            start ({});
            clicks = std::make_shared<const metronome::Sounds> (metronome::makeSounds ("classic", fs));
            clickVoices.clear();
        }

        // A note to play: from n.time (seconds on the live clock) for n.dur seconds, or held until
        // release when n.dur is 0 (a key down). Returns its id. A time already past plays at once.
        int schedule (const Note& n)
        {
            if (n.part >= (int) buses.size())
            {
                const auto had = buses.size();
                buses.resize ((size_t) n.part + 1);
                for (size_t p = had; p < buses.size(); ++p) buses[p].part = p < parts.size() ? parts[p] : Part {};
            }
            const size_t i = score.size();
            score.push_back (n);
            sounding.push_back ({});
            insertEvent ({ frameAt (n.time), true, i });
            if (n.dur > 0.0) insertEvent ({ frameAt (n.time + std::max (n.dur, 0.02)), false, i });
            return (int) i;
        }

        // A held note (scheduled with no length) ends at `atSeconds` on the live clock
        void release (int id, double atSeconds)
        {
            if (id < 0 || id >= (int) score.size()) return;
            insertEvent ({ frameAt (atSeconds), false, (size_t) id });
        }

        // Forgets what was scheduled from `fromSeconds` on: a note that hasn't started by then never
        // does, and one sounding then (or starting before it) ends there (playback stopped)
        void cancel (double fromSeconds)
        {
            const auto from = frameAt (fromSeconds);
            std::vector<Event> rest (events.begin() + (std::ptrdiff_t) next, events.end());
            events.resize (next);
            std::vector<char> willStart (score.size(), 0), willEnd (score.size(), 0);
            for (const auto& e : rest)
                if (e.at < from)
                {
                    events.push_back (e);
                    (e.on ? willStart : willEnd)[e.note] = 1;
                }
            for (size_t i = 0; i < score.size(); ++i)
                if ((isOn (i) || willStart[i]) && ! willEnd[i]) events.push_back ({ from, false, i });
            std::stable_sort (events.begin() + (std::ptrdiff_t) next, events.end(), earlier);
            clickVoices.clear();
        }

        // Every note sounding ends now and nothing scheduled plays (each fades with its release)
        void allOff()
        {
            for (auto& b : buses)
                for (auto& [k, pl] : b.players)
                {
                    if (pl.sampler) pl.sampler->allNotesOff (false);
                    for (auto& [f, pool] : pl.synths) for (auto& sy : pool) sy->allNotesOff (false);
                }
            events.erase (events.begin() + (std::ptrdiff_t) next, events.end());
            std::fill (sounding.begin(), sounding.end(), Sounding {});
            clickVoices.clear();
        }

        // The metronome's classic click at `atSeconds` on the live clock (accent on a bar's first beat), at a level
        void click (double atSeconds, bool accent, float level)
        {
            if (! clicks) return;
            clickVoices.add (clicks, accent ? &clicks->accent : &clicks->beat, frameAt (atSeconds), level);
        }

        // Writes frames [frame, frame + n) of the live mix into l and r (the click beside it, after the
        // compressor); returns whether anything is sounding or still to come
        bool renderLive (float* l, float* r, std::int64_t frame, int n)
        {
            std::fill (l, l + n, 0.0f);
            std::fill (r, r + n, 0.0f);
            for (int done = 0; done < n;)
            {
                const int k = std::min (block, n - done);
                renderBlock (l + done, r + done, frame + done, k);
                done += k;
            }
            if (! clickVoices.empty())
            {
                if ((int) clickBuf.size() < n) clickBuf.resize ((size_t) n);
                std::fill (clickBuf.begin(), clickBuf.begin() + n, 0.0f);
                clickVoices.mix (clickBuf.data(), n, frame);
                for (int i = 0; i < n; ++i) { l[i] += clickBuf[(size_t) i]; r[i] += clickBuf[(size_t) i]; }
            }
            // what's done with is let go, so a long session stays small
            if (next > 4096) { events.erase (events.begin(), events.begin() + (std::ptrdiff_t) next); next = 0; }
            bool any = next < events.size() || ! clickVoices.empty();
            for (const auto& b : buses)
                for (const auto& [k, pl] : b.players)
                {
                    if (pl.sampler && pl.sampler->isSounding()) any = true;
                    for (const auto& [f, pool] : pl.synths) for (const auto& sy : pool) if (sy->isSounding()) any = true;
                }
            return any;
        }

    private:
        static constexpr int block = 256;


        struct Def { std::string family; std::string manifest; instruments::Recordings recordings; double trimDb; };

        // What plays one instrument in one part: a recorded instrument's sampler, and synths (by family) for
        // the synth sound, a technique's synth sound (pizz, harmonics) and notes the recordings can't play yet
        struct Player
        {
            std::unique_ptr<Sampler> sampler;
            std::map<std::string, std::vector<std::unique_ptr<Synth>>> synths;
        };

        // Moved, never copied: its players own their samplers and synths. Said outright, since a
        // vector of buses grows by copying wherever a map's move may throw (MSVC's does).
        struct Bus
        {
            Part part;
            std::map<int, Player> players;
            std::vector<float> l = std::vector<float> (block), r = std::vector<float> (block);

            Bus() = default;
            Bus (Bus&&) noexcept = default;
            Bus& operator= (Bus&&) noexcept = default;
            Bus (const Bus&) = delete;
            Bus& operator= (const Bus&) = delete;
        };

        struct Event
        {
            std::int64_t at;
            bool on;
            size_t note;
        };

        // A sounding note: where it plays, to end it
        struct Sounding { Synth* synth = nullptr; int voice = -1; Sampler* sampler = nullptr; std::uint64_t id = 0; };

        std::int64_t frameAt (double seconds) const { return (std::int64_t) std::llround (seconds * fs); }
        // Sounding now (started, not yet ended)
        bool isOn (size_t i) const { return sounding[i].synth != nullptr || sounding[i].sampler != nullptr; }

        // In time order, an end before a start at the same frame
        static bool earlier (const Event& a, const Event& b) { return a.at < b.at || (a.at == b.at && ! a.on && b.on); }

        // Into the events still to come, in time order
        void insertEvent (const Event& e)
        {
            events.insert (std::upper_bound (events.begin() + (std::ptrdiff_t) next, events.end(), e, earlier), e);
        }

        void start (const std::vector<Note>& notes)
        {
            score = notes;
            buses.clear();
            const int numParts = std::max ((int) parts.size(), [&] { int m = 0; for (const auto& n : score) m = std::max (m, n.part + 1); return m; }());
            buses.resize ((size_t) numParts);
            for (int p = 0; p < numParts; ++p) buses[(size_t) p].part = p < (int) parts.size() ? parts[(size_t) p] : Part {};
            events.clear();
            for (size_t i = 0; i < score.size(); ++i)
            {
                const auto& n = score[i];
                const auto on = (std::int64_t) std::llround ((n.time + leadSeconds) * fs);
                events.push_back ({ on, true, i });
                events.push_back ({ (std::int64_t) std::llround ((n.time + leadSeconds + std::max (n.dur, 0.02)) * fs), false, i });
            }
            std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b) { return a.at < b.at || (a.at == b.at && ! a.on && b.on); });
            next = 0;
            sounding.assign (score.size(), {});
            reverb.prepare (fs, block);
            auto rs = ReverbEngine::Settings {};
            rs.mode = ReverbEngine::hall;
            rs.predelayMs = 18.0f;
            rs.decaySec = decay;
            rs.size = 0.35f;
            rs.toneHz = 3300.0f;
            rs.lowCutHz = 20.0f;
            rs.width = 1.0f;
            rs.mod = 0.2f;
            reverb.setSettings (rs);
            reverb.reset();
            comp.prepare (fs);
            comp.setDetail ({ compThreshold, compRatio, 10.0f, 200.0f, 0.0f, false });
            sendL.assign (block, 0.0f); sendR.assign (block, 0.0f); wetL.assign (block, 0.0f); wetR.assign (block, 0.0f);
        }

        Synth& synthFor (Player& pl, const std::string& family)
        {
            auto& pool = pl.synths[family];
            for (auto& s : pool) if (s->freeVoices() > 0) return *s;
            auto s = std::make_unique<Synth>();
            s->prepare (fs);
            s->setSettings (*instruments::synthSound (family));
            pool.push_back (std::move (s));
            return *pool.back();
        }

        Player& playerFor (int part, int instrument)
        {
            auto& pl = buses[(size_t) part].players[instrument];
            const auto& d = defs[(size_t) instrument];
            if (! d.manifest.empty() && ! pl.sampler)
            {
                pl.sampler = std::make_unique<Sampler>();
                pl.sampler->prepare (fs);
                instruments::loadRecorded (*pl.sampler, d.manifest, d.recordings, d.trimDb);
            }
            return pl;
        }

        void noteOn (size_t i)
        {
            const auto& n = score[i];
            if (n.instrument < 0 || n.instrument >= (int) defs.size() || n.part < 0) return;
            const auto& d = defs[(size_t) n.instrument];
            auto& pl = playerFor (n.part, n.instrument);
            const float vel = std::clamp (n.velocity, 1.0f, 127.0f) / 127.0f;
            NoteExpression e;
            e.gain = n.gain;
            if (n.bendTo >= 0 && n.dur > 0.0) { e.bendSemitones = (float) (n.bendTo - n.midi); e.bendFrom = (float) (n.dur * n.bendAt); e.bendTo = (float) n.dur; }
            const bool swell = n.velocityEnd > 0.0f && n.dur > 0.3 && std::abs (n.velocityEnd - n.velocity) > 3.0f;
            if (pl.sampler && pl.sampler->ready (n.midi, vel))
            {
                // a recording: muted darker and softer, a technique's shade, a slur sliding in from 0.3
                e.slurred = n.slurred; e.slurLevel = 0.3f; e.slurAttack = 0.03f;
                if (n.muted) { e.brightness *= 0.4f; e.gain *= 0.75f; }
                if (n.technique == Technique::dark) e.brightness *= 0.5f;
                if (n.technique == Technique::bright) e.brightness *= 1.8f;
                if (swell) { e.swellTo = std::pow (n.velocityEnd / n.velocity, instruments::recordedVelocityCurve); e.swellUntil = (float) n.dur; }
                sounding[i] = { nullptr, -1, pl.sampler.get(), pl.sampler->noteOn (n.midi, vel, e) };
                return;
            }
            // a synth sound: its family's, or the technique's
            const std::string family = n.technique == Technique::pizz ? "pizz" : n.technique == Technique::harmonic ? "harmonic" : d.family;
            e.slurred = n.slurred; e.slurLevel = 0.35f; e.slurAttack = 0.03f;
            if (n.muted) { e.brightness *= 0.4f; e.gain *= 0.7f; }
            if (n.technique == Technique::dark) e.brightness *= 0.55f;
            if (n.technique == Technique::bright) e.brightness *= 1.8f;
            if (swell && instruments::swellsWithin (family)) { e.swellTo = std::pow (n.velocityEnd / n.velocity, 1.5f); e.swellUntil = (float) n.dur; }
            auto& s = synthFor (pl, family);
            sounding[i] = { &s, s.noteOn (n.midi, vel, e), nullptr, 0 };
        }

        void noteOff (size_t i)
        {
            const auto& s = sounding[i];
            if (s.synth != nullptr) s.synth->noteOffVoice (s.voice, score[i].midi);
            if (s.sampler != nullptr && s.id != 0) s.sampler->noteOffId (s.id);
            sounding[i] = {};
        }

        void renderBlock (float* outL, float* outR, std::int64_t at, int n)
        {
            int done = 0;
            while (done < n)
            {
                while (next < events.size() && events[next].at <= at + done)
                {
                    if (events[next].on) noteOn (events[next].note); else noteOff (events[next].note);
                    ++next;
                }
                int len = n - done;
                if (next < events.size()) len = (int) std::min<std::int64_t> (len, std::max<std::int64_t> (1, events[next].at - (at + done)));
                mixInto (outL + done, outR + done, len);
                done += len;
            }
        }

        void mixInto (float* outL, float* outR, int n)
        {
            std::fill (sendL.begin(), sendL.begin() + n, 0.0f);
            std::fill (sendR.begin(), sendR.begin() + n, 0.0f);
            for (auto& b : buses)
            {
                std::fill (b.l.begin(), b.l.begin() + n, 0.0f);
                std::fill (b.r.begin(), b.r.begin() + n, 0.0f);
                for (auto& [k, pl] : b.players)
                {
                    if (pl.sampler) pl.sampler->render (b.l.data(), b.r.data(), n);
                    for (auto& [f, pool] : pl.synths) for (auto& s : pool) s->render (b.l.data(), b.r.data(), n);
                }
                // the part at its volume (0.8 as recorded) and pan (the mixer's balance)
                const auto pg = mix::panGains (b.part.volume / 0.8f, std::clamp (b.part.pan, -1.0f, 1.0f), mix::PanLaw::linear);
                for (int i = 0; i < n; ++i)
                {
                    const float l = b.l[(size_t) i] * pg.left, r = b.r[(size_t) i] * pg.right;
                    outL[i] += l; outR[i] += r;
                    sendL[(size_t) i] += l; sendR[(size_t) i] += r;
                }
            }
            reverb.process (sendL.data(), sendR.data(), wetL.data(), wetR.data(), n);
            for (int i = 0; i < n; ++i) { outL[i] += wetL[(size_t) i] * sendLevel; outR[i] += wetR[(size_t) i] * sendLevel; }
            float* ch[2] { outL, outR };
            comp.process (ch, 2, n);
        }

        double fs = 48000.0;
        std::vector<Def> defs;
        std::vector<Part> parts;
        std::vector<Bus> buses;
        std::vector<Note> score;
        std::vector<Event> events;
        std::vector<Sounding> sounding;
        size_t next = 0;
        ReverbEngine reverb;
        Compressor comp;
        std::vector<float> sendL, sendR, wetL, wetR;
        float sendLevel = reverbSend, decay = reverbDecay, compThreshold = -16.0f, compRatio = 3.0f;
        std::shared_ptr<const metronome::Sounds> clicks;
        metronome::ClickVoices clickVoices;
        std::vector<float> clickBuf;
    };
}
