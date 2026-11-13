#pragma once

// The chord and scale theory (Harmony.h, Voicings.h, ScaleTheory.h) and the
// voice exercises' notes (VoiceTracker.h) as JSON
// requests and answers, for the browser build (th_call in wasm/api_theory.cpp)
// and the reference set, so both run the same code on the same text.
//
// A request is an object: {"fn": "<name>", ...arguments}. The names and
// arguments follow the functions in those headers. Values cross as:
//   note    {"L": letter 0..6 (C..B), "a": accidental -2..2}, or text ("F#")
//   key     {"tonic": note, "mode": "major" | "minor"}, or text ("F# minor")
//   chord   {"root": note, "type": chord type id, "bass": note or null}, or a symbol ("Am7/G")
//   scale   a library id ("dorian")
//   courses [{"midi": [..], "open": n, "start": n}], as coursesOf answers
//   a fit chord: the text readChord reads ("C7alt")
//   range   {"low", "high", "tessLo", "tessHi"}: a saved vocal range, MIDI notes
//   rand    [numbers 0..1]: the random numbers an exercise uses, in order
// The answer is the result as JSON, or {"error": "..."} for a request it can't
// read. Numbers are written rounded to nine decimals, so every build writes
// the same text. Plain C++20, no JUCE.

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "ScaleTheory.h"
#include "VoiceTracker.h"

namespace vs::theorycalls
{
    // ---- a small JSON value, reader and writer

    struct Json
    {
        enum class Type { null, boolean, number, string, array, object } type = Type::null;
        bool b = false;
        double n = 0;
        std::string s;
        std::vector<Json> items;
        std::vector<std::pair<std::string, Json>> fields;

        const Json* get (std::string_view k) const
        {
            if (type != Type::object) return nullptr;
            for (const auto& [name, v] : fields) if (name == k) return &v;
            return nullptr;
        }
        bool isNull() const { return type == Type::null; }
    };

    namespace detail
    {
        struct Reader
        {
            std::string_view t;
            size_t i = 0;
            bool ok = true;

            void space() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) ++i; }
            bool eat (char c) { space(); if (i < t.size() && t[i] == c) { ++i; return true; } return false; }

            static void utf8 (std::string& out, unsigned cp)
            {
                if (cp < 0x80) out += (char) cp;
                else if (cp < 0x800) { out += (char) (0xC0 | (cp >> 6)); out += (char) (0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { out += (char) (0xE0 | (cp >> 12)); out += (char) (0x80 | ((cp >> 6) & 0x3F)); out += (char) (0x80 | (cp & 0x3F)); }
                else { out += (char) (0xF0 | (cp >> 18)); out += (char) (0x80 | ((cp >> 12) & 0x3F)); out += (char) (0x80 | ((cp >> 6) & 0x3F)); out += (char) (0x80 | (cp & 0x3F)); }
            }

            unsigned hex4()
            {
                unsigned v = 0;
                for (int k = 0; k < 4; ++k, ++i)
                {
                    if (i >= t.size()) { ok = false; return 0; }
                    const char c = t[i];
                    v = v * 16 + (unsigned) (c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : (ok = false, 0));
                }
                return v;
            }

            std::string str()
            {
                std::string out;
                ++i;   // the opening quote
                while (i < t.size() && t[i] != '"')
                {
                    if (t[i] != '\\') { out += t[i++]; continue; }
                    if (++i >= t.size()) break;
                    const char e = t[i++];
                    switch (e)
                    {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'u':
                        {
                            unsigned cp = hex4();
                            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u')
                            {
                                i += 2;
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (hex4() - 0xDC00);
                            }
                            utf8 (out, cp);
                            break;
                        }
                        default: out += e;
                    }
                }
                if (i >= t.size()) ok = false; else ++i;
                return out;
            }

            Json value()
            {
                Json v;
                space();
                if (i >= t.size()) { ok = false; return v; }
                const char c = t[i];
                if (c == '{')
                {
                    v.type = Json::Type::object;
                    ++i;
                    if (eat ('}')) return v;
                    do
                    {
                        space();
                        if (i >= t.size() || t[i] != '"') { ok = false; return v; }
                        auto k = str();
                        if (! eat (':')) { ok = false; return v; }
                        v.fields.push_back ({ std::move (k), value() });
                    } while (ok && eat (','));
                    if (! eat ('}')) ok = false;
                }
                else if (c == '[')
                {
                    v.type = Json::Type::array;
                    ++i;
                    if (eat (']')) return v;
                    do v.items.push_back (value()); while (ok && eat (','));
                    if (! eat (']')) ok = false;
                }
                else if (c == '"') { v.type = Json::Type::string; v.s = str(); }
                else if (t.substr (i, 4) == "true") { v.type = Json::Type::boolean; v.b = true; i += 4; }
                else if (t.substr (i, 5) == "false") { v.type = Json::Type::boolean; i += 5; }
                else if (t.substr (i, 4) == "null") { i += 4; }
                else
                {
                    const size_t from = i;
                    while (i < t.size() && (std::string_view ("+-.eE0123456789").find (t[i]) != std::string_view::npos)) ++i;
                    if (i == from) { ok = false; return v; }
                    v.type = Json::Type::number;
                    v.n = std::strtod (std::string (t.substr (from, i - from)).c_str(), nullptr);
                }
                return v;
            }
        };
    }

    inline std::optional<Json> parse (std::string_view text)
    {
        detail::Reader r { text };
        auto v = r.value();
        r.space();
        if (! r.ok || r.i != text.size()) return {};
        return v;
    }

    // Writes JSON text as it goes
    class Out
    {
    public:
        std::string text;

        Out& raw (std::string_view s) { text += s; return *this; }

        Out& str (std::string_view s)
        {
            text += '"';
            for (unsigned char c : s)
            {
                if (c == '"') text += "\\\"";
                else if (c == '\\') text += "\\\\";
                else if (c == '\n') text += "\\n";
                else if (c < 0x20) { char b[8]; std::snprintf (b, sizeof b, "\\u%04x", c); text += b; }
                else text += (char) c;
            }
            text += '"';
            return *this;
        }

        // rounded to nine decimals, without trailing zeros ("0.3", "-2", "1.25")
        Out& num (double v)
        {
            if (! std::isfinite (v)) return raw ("null");
            double r = std::round (v * 1e9) / 1e9;
            if (r == 0) r = 0;   // no "-0"
            char b[48];
            std::snprintf (b, sizeof b, "%.9f", r);
            std::string s (b);
            while (! s.empty() && s.back() == '0') s.pop_back();
            if (! s.empty() && s.back() == '.') s.pop_back();
            if (s == "-0") s = "0";
            text += s;
            return *this;
        }

        Out& boolean (bool b) { return raw (b ? "true" : "false"); }
        Out& null() { return raw ("null"); }

        // objects and arrays: open, then key/next, then close
        Out& open (char c) { text += c; first.push_back (true); return *this; }
        Out& close (char c) { text += c; first.pop_back(); return *this; }
        Out& next() { if (! first.back()) text += ','; first.back() = false; return *this; }
        Out& key (std::string_view k) { next(); str (k); text += ':'; return *this; }

    private:
        std::vector<bool> first;
    };

    namespace detail
    {
        using music::Note;
        using music::Key;
        using harmony::Chord;

        // ---- reading arguments

        // The first problem with a request (the build has no exceptions, so a reader that
        // can't read its argument notes why and returns a harmless value; the answer is then the error)
        inline std::string& problem() { static std::string p; return p; }
        inline void fail (std::string why) { if (problem().empty()) problem() = std::move (why); }

        inline const Json& need (const Json& req, std::string_view k)
        {
            static const Json none;
            const auto* v = req.get (k);
            if (! v) { fail ("missing " + std::string (k)); return none; }
            return *v;
        }

        inline int intArg (const Json& req, std::string_view k, int fallback)
        {
            const auto* v = req.get (k);
            if (! v || v->isNull()) return fallback;
            if (v->type == Json::Type::boolean) return v->b ? 1 : 0;
            if (v->type != Json::Type::number) { fail (std::string (k) + " is not a number"); return fallback; }
            return (int) v->n;
        }

        inline bool boolArg (const Json& req, std::string_view k, bool fallback)
        {
            const auto* v = req.get (k);
            if (! v || v->isNull()) return fallback;
            return v->type == Json::Type::boolean ? v->b : v->n != 0;
        }

        inline std::string textArg (const Json& req, std::string_view k, std::string fallback = {})
        {
            const auto* v = req.get (k);
            if (! v || v->isNull()) return fallback;
            if (v->type != Json::Type::string) { fail (std::string (k) + " is not text"); return fallback; }
            return v->s;
        }

        inline double numArg (const Json& req, std::string_view k, double fallback)
        {
            const auto* v = req.get (k);
            if (! v || v->isNull()) return fallback;
            if (v->type != Json::Type::number) { fail (std::string (k) + " is not a number"); return fallback; }
            return v->n;
        }

        inline voice::VocalRange rangeFrom (const Json& v)
        {
            return { numArg (v, "low", 0), numArg (v, "high", 0), numArg (v, "tessLo", 0), numArg (v, "tessHi", 0) };
        }

        // The request's random numbers, handed out in order (0 and a problem once they run out)
        inline voice::Rand randFrom (const Json& req)
        {
            auto list = std::make_shared<std::vector<double>>();
            if (const auto* r = req.get ("rand")) for (const auto& x : r->items) list->push_back (x.n);
            auto next = std::make_shared<size_t> (0);
            return [list, next]
            {
                if (*next >= list->size()) { fail ("rand ran out"); return 0.0; }
                return (*list)[(*next)++];
            };
        }

        inline std::vector<int> ints (const Json& v)
        {
            std::vector<int> out;
            for (const auto& x : v.items) out.push_back ((int) x.n);
            return out;
        }

        inline Note noteFrom (const Json& v)
        {
            if (v.type == Json::Type::string)
            {
                const auto n = music::parseNote (v.s);
                if (! n) { fail ("not a note: " + v.s); return {}; }
                return *n;
            }
            const auto* L = v.get ("L");
            const auto* a = v.get ("a");
            if (! L) { fail ("a note needs L"); return {}; }
            return music::note ((int) L->n, a ? (int) a->n : 0);
        }

        inline std::optional<Note> optNote (const Json& req, std::string_view k)
        {
            const auto* v = req.get (k);
            if (! v || v->isNull()) return {};
            return noteFrom (*v);
        }

        inline Key keyFrom (const Json& v)
        {
            if (v.type == Json::Type::string)
            {
                const auto k = music::parseKey (v.s);
                if (! k) { fail ("not a key: " + v.s); return {}; }
                return *k;
            }
            const auto* m = v.get ("mode");
            const bool minor = m && m->type == Json::Type::string && m->s == "minor";
            return { noteFrom (need (v, "tonic")), minor ? music::Mode::minor : music::Mode::major };
        }

        inline int typeFrom (const Json& v)
        {
            const int t = v.type == Json::Type::string ? harmony::typeIndex (v.s) : (int) v.n;
            if (t < 0 || t >= (int) chords::types.size()) { fail ("not a chord type: " + v.s); return 0; }
            return t;
        }

        inline Chord chordFrom (const Json& v)
        {
            if (v.type == Json::Type::string)
            {
                const auto c = harmony::parseChord (v.s);
                if (! c) { fail ("not a chord: " + v.s); return {}; }
                return *c;
            }
            Chord c;
            c.root = noteFrom (need (v, "root"));
            c.type = typeFrom (need (v, "type"));
            if (const auto* b = v.get ("bass"); b && ! b->isNull()) c.bass = noteFrom (*b);
            return c;
        }

        inline std::optional<Chord> optChord (const Json& req, std::string_view k)
        {
            const auto* v = req.get (k);
            if (! v || v->isNull()) return {};
            return chordFrom (*v);
        }

        inline const scales::LibraryScale& scaleFrom (const Json& req, std::string_view k = "scale")
        {
            const auto id = textArg (req, k);
            const int i = scaletheory::scaleIndex (id);
            if (i < 0) { fail ("not a library scale: " + id); return scales::library[0]; }
            return scales::library[(size_t) i];
        }

        inline std::vector<voicings::Course> coursesFrom (const Json& v)
        {
            std::vector<voicings::Course> out;
            for (const auto& c : v.items)
            {
                voicings::Course k;
                k.midi = ints (need (c, "midi"));
                k.open = intArg (c, "open", k.midi.empty() ? 0 : k.midi[0]);
                k.start = intArg (c, "start", 0);
                out.push_back (k);
            }
            return out;
        }

        inline scaletheory::FitChord fitChordFrom (const Json& v)
        {
            const auto c = v.type == Json::Type::string ? scaletheory::readChord (v.s) : std::nullopt;
            if (! c) { fail (v.type == Json::Type::string ? "not a chord: " + v.s : std::string ("a fit chord is the text readChord reads")); return *scaletheory::readChord ("C"); }
            return *c;
        }

        // ---- writing answers

        inline const char* familyId (chords::Family f)
        {
            static const char* const ids[] { "maj", "min", "dim", "aug", "sus", "power", "dom", "hdim" };
            return ids[(int) f];
        }
        inline const char* fnCode (harmony::Function f) { return f == harmony::Function::home ? "T" : f == harmony::Function::away ? "S" : "D"; }
        inline const char* groupId (harmony::PaletteGroup g)
        {
            static const char* const ids[] { "diatonic", "sevenths", "secondary", "borrowed", "relative", "spice" };
            return ids[(int) g];
        }
        inline const char* kindId (harmony::MoveKind k)
        {
            static const char* const ids[] { "resolve", "surprise", "strong", "smooth", "step", "leap", "colour" };
            return ids[(int) k];
        }
        inline const char* reasonId (harmony::MoveReason r)
        {
            static const char* const ids[] { "cadence", "ownDominant", "tritoneSub", "leadingTone", "deceptive", "plagal", "backdoor", "motion" };
            return ids[(int) r];
        }

        inline void put (Out& o, Note n) { o.open ('{').key ("L").num (n.letter).key ("a").num (n.acc).close ('}'); }
        inline void put (Out& o, const std::optional<Note>& n) { if (n) put (o, *n); else o.null(); }
        inline void put (Out& o, const Key& k) { o.open ('{').key ("tonic"); put (o, k.tonic); o.key ("mode").str (k.mode == music::Mode::minor ? "minor" : "major").close ('}'); }
        inline void put (Out& o, const Chord& c)
        {
            o.open ('{').key ("root"); put (o, c.root);
            o.key ("type").str (chords::types[(size_t) c.type].id).key ("bass"); put (o, c.bass);
            o.close ('}');
        }
        inline void put (Out& o, const std::optional<Chord>& c) { if (c) put (o, *c); else o.null(); }
        inline void put (Out& o, const harmony::Degree& g)
        {
            o.open ('{').key ("d").str (g.d).key ("n").num (g.n).key ("step").num (g.step).key ("acc").num (g.acc).key ("semis").num (g.semis).close ('}');
        }
        inline void put (Out& o, const harmony::Interval& iv) { o.open ('{').key ("steps").num (iv.steps).key ("semis").num (iv.semis).close ('}'); }

        template <typename T, typename F> inline void list (Out& o, const std::vector<T>& v, F each)
        {
            o.open ('[');
            for (const auto& x : v) { o.next(); each (x); }
            o.close (']');
        }
        inline void notes (Out& o, const std::vector<Note>& v) { list (o, v, [&] (Note n) { put (o, n); }); }
        inline void nums (Out& o, const std::vector<int>& v) { list (o, v, [&] (int x) { o.num (x); }); }
        inline void nums (Out& o, const std::vector<double>& v) { list (o, v, [&] (double x) { o.num (x); }); }
        inline void strs (Out& o, const std::vector<std::string>& v) { list (o, v, [&] (const std::string& x) { o.str (x); }); }
        inline void chordList (Out& o, const std::vector<Chord>& v) { list (o, v, [&] (const Chord& c) { put (o, c); }); }
        inline void degrees (Out& o, const std::vector<harmony::Degree>& v) { list (o, v, [&] (const harmony::Degree& g) { put (o, g); }); }

        inline void put (Out& o, const harmony::ChordTone& t)
        {
            o.open ('{').key ("d").str (t.d).key ("note"); put (o, t.note);
            o.key ("pc").num (t.pc).key ("semis").num (t.semis).close ('}');
        }

        inline void put (Out& o, const scaletheory::ScaleNote& n)
        {
            o.open ('{').key ("d").str (n.d).key ("note"); put (o, n.note);
            o.key ("pc").num (n.pc).key ("semis").num (n.semis).close ('}');
        }
        inline void scaleNotes (Out& o, const std::vector<scaletheory::ScaleNote>& v) { list (o, v, [&] (const scaletheory::ScaleNote& n) { put (o, n); }); }

        inline void put (Out& o, const harmony::NoteSetName& r)
        {
            using K = harmony::NoteSetName::Kind;
            o.open ('{').key ("kind").str (r.kind == K::note ? "note" : r.kind == K::chord ? "chord" : "interval");
            o.key ("symbol").str (r.symbol).key ("cost").num (r.cost).key ("notes"); notes (o, r.notes);
            o.key ("labels").open ('{');
            for (const auto& [pc, label] : r.labels) o.key (std::to_string (pc)).str (label);
            o.close ('}');
            if (r.kind == K::chord)
            {
                o.key ("chord"); put (o, r.chord);
                o.key ("no3").boolean (r.no3).key ("foreign").boolean (r.foreign).key ("bassDegree").str (r.bassDegree);
                o.key ("powerFifthInBass").boolean (r.powerFifthInBass).key ("formula"); strs (o, r.formula);
                o.key ("missing"); strs (o, r.missing);
            }
            if (r.kind == K::interval)
            {
                o.key ("semis").num (r.semis).key ("low"); put (o, r.low);
                o.key ("top"); put (o, r.top);
                o.key ("hintType").str (r.hintType);
                if (! r.hintType.empty()) { o.key ("hintRoot"); put (o, r.hintRoot); o.key ("hintOverBass").boolean (r.hintOverBass); }
            }
            o.close ('}');
        }

        inline void put (Out& o, const harmony::InversionPart& p) { o.open ('{').key ("slash").str (p.slash).key ("overBass"); put (o, p.overBass); o.close ('}'); }

        inline void put (Out& o, const harmony::PaletteEntry& e)
        {
            o.open ('{').key ("group").str (groupId (e.group)).key ("rn").str (e.rn).key ("label").str (e.label).key ("relativeRn").str (e.relativeRn);
            o.key ("chord"); put (o, e.chord);
            o.key ("alias").str (e.alias).key ("fn").str (fnCode (e.fn)).key ("target").str (e.target).key ("outside"); notes (o, e.outside);
            o.close ('}');
        }

        inline void put (Out& o, const harmony::KeyPalette& p)
        {
            o.open ('{').key ("key"); put (o, p.key);
            o.key ("groups").open ('[');
            for (const auto& [g, items] : p.groups)
            {
                o.next().open ('{').key ("id").str (groupId (g)).key ("items");
                list (o, items, [&] (const harmony::PaletteEntry& e) { put (o, e); });
                o.close ('}');
            }
            o.close (']');
            o.key ("relative"); put (o, p.relative);
            o.key ("relativeHome"); put (o, p.relativeHome);
            o.close ('}');
        }

        inline void put (Out& o, const harmony::Pivot& p) { o.open ('{').key ("here").str (p.here).key ("there").str (p.there).key ("chord"); put (o, p.chord); o.close ('}'); }

        inline void put (Out& o, const harmony::Modulation& m)
        {
            o.open ('{').key ("key"); put (o, m.key);
            o.key ("target").str (m.target).key ("pivot"); put (o, m.pivot);
            o.key ("shared"); list (o, m.shared, [&] (const harmony::Pivot& p) { put (o, p); });
            o.key ("route"); chordList (o, m.route);
            o.close ('}');
        }

        inline void put (Out& o, const harmony::MoveFacts& f)
        {
            o.open ('{').key ("reason").str (reasonId (f.reason)).key ("rootInterval").num (f.rootInterval);
            o.key ("toHome").boolean (f.toHome).key ("fromHome").boolean (f.fromHome).key ("dominantSeventh").boolean (f.dominantSeventh);
            o.key ("third"); put (o, f.third);
            o.key ("seventh"); put (o, f.seventh);
            o.key ("nextThird"); put (o, f.nextThird);
            o.key ("seventhSettles").boolean (f.seventhSettles);
            o.key ("expected"); put (o, f.expected);
            o.key ("expectedChord"); put (o, f.expectedChord);
            o.key ("kept"); notes (o, f.kept);
            o.key ("minorThird"); put (o, f.minorThird);
            o.key ("nextFifth"); put (o, f.nextFifth);
            o.key ("common"); notes (o, f.common);
            o.key ("fromFn").str (fnCode (f.fromFn)).key ("toFn").str (fnCode (f.toFn));
            o.key ("wantsNext"); put (o, f.wantsNext);
            o.close ('}');
        }

        inline void put (Out& o, const harmony::Relation& r)
        {
            o.open ('{').key ("score").num (r.score).key ("kind").str (kindId (r.kind)).key ("facts"); put (o, r.facts);
            o.close ('}');
        }

        inline void put (Out& o, const voicings::Course& c) { o.open ('{').key ("midi"); nums (o, c.midi); o.key ("open").num (c.open).key ("start").num (c.start).close ('}'); }
        inline void courseList (Out& o, const std::vector<voicings::Course>& v) { list (o, v, [&] (const voicings::Course& c) { put (o, c); }); }

        inline void put (Out& o, const std::optional<voicings::Barre>& b)
        {
            if (! b) { o.null(); return; }
            o.open ('{').key ("fret").num (b->fret).key ("from").num (b->from).key ("to").num (b->to).close ('}');
        }

        inline void put (Out& o, const voicings::Voicing& v)
        {
            o.open ('{').key ("frets"); nums (o, v.frets);
            o.key ("score").num (v.score).key ("pos").num (v.pos).key ("fingers").num (v.fingers).key ("barre"); put (o, v.barre);
            o.key ("notes"); nums (o, v.notes);
            o.key ("rootless").boolean (v.rootless).key ("missing"); strs (o, v.missing);
            o.close ('}');
        }

        inline void put (Out& o, const scaletheory::Fit& f)
        {
            o.open ('{').key ("missNeed"); degrees (o, f.missNeed);
            o.key ("missMay"); degrees (o, f.missMay);
            o.key ("avoid"); scaleNotes (o, f.avoid);
            o.key ("colour"); scaleNotes (o, f.colour);
            o.key ("notes"); scaleNotes (o, f.notes);
            o.close ('}');
        }

        inline void put (Out& o, const scaletheory::FitChord& c)
        {
            o.open ('{').key ("text").str (c.text).key ("root"); put (o, c.root);
            o.key ("alt").boolean (c.alt).key ("chord"); put (o, c.chord);
            o.key ("fam").str (familyId (c.family)).key ("need"); degrees (o, c.need);
            o.key ("may"); degrees (o, c.may);
            o.key ("bass"); put (o, c.bass);
            o.key ("symbol").str (c.symbol).close ('}');
        }

        inline void put (Out& o, const scaletheory::ScaleChoice& c)
        {
            o.open ('{').key ("scale").str (scales::library[(size_t) c.scale].id).key ("root"); put (o, c.root);
            o.key ("score").num (c.score).key ("fit"); put (o, c.fit);
            o.key ("inKey").boolean (c.inKey).close ('}');
        }

        inline void put (Out& o, const scaletheory::ProgressionScale& p)
        {
            o.open ('{').key ("scale").str (scales::library[(size_t) p.scale].id).key ("root"); put (o, p.root);
            o.key ("score").num (p.score).key ("missing"); notes (o, p.missing);
            o.key ("pcs"); nums (o, p.pcs);
            o.key ("notes"); scaleNotes (o, p.notes);
            o.key ("blues").boolean (p.blues).close ('}');
        }

        inline void put (Out& o, const scaletheory::StackedChord& c)
        {
            o.open ('{').key ("notes"); notes (o, c.notes);
            o.key ("type");
            if (c.type < 0) o.null(); else o.str (chords::types[(size_t) c.type].id);
            o.key ("chord");
            if (c.type < 0) o.null(); else put (o, c.chord);
            o.key ("symbol").str (c.symbol).key ("numeral").str (c.numeral).close ('}');
        }

        inline void put (Out& o, const scaletheory::NeckNote& n)
        {
            o.open ('{').key ("course").num (n.course).key ("fret").num (n.fret).key ("midi").num (n.midi).key ("pc").num (n.pc);
            o.key ("d").str (n.d).key ("note"); put (o, n.note);
            o.close ('}');
        }
        inline void neckNotes (Out& o, const std::vector<scaletheory::NeckNote>& v) { list (o, v, [&] (const scaletheory::NeckNote& n) { put (o, n); }); }

        // ---- the calls

        inline void call (const std::string& fn, const Json& q, Out& o)
        {
            namespace H = harmony;
            namespace V = voicings;
            namespace S = scaletheory;

            // building blocks
            if (fn == "degree")
            {
                const auto g = H::degree (textArg (q, "d"));
                if (g) put (o, *g); else o.null();
            }
            else if (fn == "above")
            {
                const auto g = H::degree (textArg (q, "d"));
                if (! g) fail ("not a degree");
                else put (o, H::above (noteFrom (need (q, "root")), *g));
            }
            else if (fn == "intervalBetween") put (o, H::intervalBetween (noteFrom (need (q, "a")), noteFrom (need (q, "b"))));
            else if (fn == "moveBy")
                put (o, H::moveBy (noteFrom (need (q, "note")), { intArg (q, "steps", 0), intArg (q, "semis", 0) }));
            // chords
            else if (fn == "chordTones") list (o, H::chordTones (chordFrom (need (q, "chord"))), [&] (const H::ChordTone& t) { put (o, t); });
            else if (fn == "chordSymbol") o.str (H::chordSymbol (chordFrom (need (q, "chord"))));
            else if (fn == "parseChord")
            {
                const auto c = H::parseChord (textArg (q, "text"));
                put (o, c);
            }
            else if (fn == "chordPcs") nums (o, H::chordPcs (chordFrom (need (q, "chord"))));
            else if (fn == "spellRoot" || fn == "spellBass")
            {
                const auto tonic = optNote (q, "keyTonic");
                const Note* t = tonic ? &*tonic : nullptr;
                const int pc = intArg (q, "pc", 0), side = intArg (q, "side", 0);
                put (o, fn == "spellRoot" ? H::spellRoot (pc, typeFrom (need (q, "type")), t, side)
                                          : H::spellBass (pc, noteFrom (need (q, "root")), t, side));
            }
            else if (fn == "nameNotes")
            {
                const auto tonic = optNote (q, "keyTonic");
                list (o, H::nameNotes (ints (need (q, "midis")), tonic ? &*tonic : nullptr, intArg (q, "side", 0)),
                      [&] (const H::NoteSetName& r) { put (o, r); });
            }
            // numerals and keys
            else if (fn == "numeralChord") put (o, H::numeralChord (keyFrom (need (q, "key")), textArg (q, "rn")));
            else if (fn == "plainNumeral") o.str (H::plainNumeral (keyFrom (need (q, "key")), chordFrom (need (q, "chord"))));
            else if (fn == "inversionPart") put (o, H::inversionPart (chordFrom (need (q, "chord"))));
            else if (fn == "numeralOf")
            {
                const auto key = keyFrom (need (q, "key"));
                const auto next = optChord (q, "next");
                const auto r = H::numeralOf (key, chordFrom (need (q, "chord")), nullptr, next ? &*next : nullptr);
                o.open ('{').key ("label").str (r.label).key ("inversion"); put (o, r.inversion);
                o.close ('}');
            }
            else if (fn == "keyPalette") put (o, H::keyPalette (keyFrom (need (q, "key"))));
            else if (fn == "modulations") list (o, H::modulations (keyFrom (need (q, "key"))), [&] (const H::Modulation& m) { put (o, m); });
            else if (fn == "relate") put (o, H::relate (keyFrom (need (q, "key")), chordFrom (need (q, "from")), chordFrom (need (q, "to"))));
            else if (fn == "nextMoves")
                list (o, H::nextMoves (keyFrom (need (q, "key")), chordFrom (need (q, "from")), intArg (q, "limit", 8)),
                      [&] (const H::NextMove& m)
                      {
                          o.open ('{').key ("relation"); put (o, m.relation);
                          o.key ("entry"); put (o, m.entry);
                          o.close ('}');
                      });
            else if (fn == "keyInterval") put (o, H::keyInterval (keyFrom (need (q, "from")), keyFrom (need (q, "to"))));
            else if (fn == "transposeChord")
                put (o, H::transposeChord (chordFrom (need (q, "chord")), { intArg (q, "steps", 0), intArg (q, "semis", 0) }));
            else if (fn == "keyBySemis") put (o, H::keyBySemis (keyFrom (need (q, "key")), intArg (q, "semis", 0)));
            // fretted and keyboard voicings
            else if (fn == "frettable")
            {
                std::vector<std::vector<int>> tunings;
                if (const auto* t = q.get ("tunings")) for (const auto& x : t->items) tunings.push_back (ints (x));
                o.boolean (V::frettable (textArg (q, "name"), textArg (q, "part"), tunings));
            }
            else if (fn == "coursesOf")
            {
                std::vector<int> sizes;
                if (const auto* c = q.get ("courses"); c && ! c->isNull()) sizes = ints (*c);
                courseList (o, V::coursesOf (ints (need (q, "midi")), sizes, textArg (q, "name")));
            }
            else if (fn == "courseNotes")
            {
                const auto courses = coursesFrom (need (q, "courses"));
                const int i = intArg (q, "course", 0);
                if (i < 0 || i >= (int) courses.size()) { fail ("no such course"); return; }
                const auto m = V::courseNotes (courses[(size_t) i], intArg (q, "fret", 0), intArg (q, "capo", 0));
                if (m) nums (o, *m); else o.null();
            }
            else if (fn == "shapeNotes") nums (o, V::shapeNotes (coursesFrom (need (q, "courses")), ints (need (q, "frets")), intArg (q, "capo", 0)));
            else if (fn == "ascending") o.boolean (V::ascending (coursesFrom (need (q, "courses"))));
            else if (fn == "fingering")
            {
                const auto f = V::fingering (ints (need (q, "frets")));
                o.open ('{').key ("fingers").num (f.fingers).key ("barre"); put (o, f.barre);
                o.key ("partial").boolean (f.partial).close ('}');
            }
            else if (fn == "fretVoicings")
            {
                V::FretOptions opts;
                opts.capo = intArg (q, "capo", opts.capo);
                opts.maxFret = intArg (q, "maxFret", opts.maxFret);
                opts.limit = intArg (q, "limit", opts.limit);
                opts.minSound = intArg (q, "minSound", opts.minSound);
                list (o, V::fretVoicings (chordFrom (need (q, "chord")), coursesFrom (need (q, "courses")), opts),
                      [&] (const V::Voicing& v) { put (o, v); });
            }
            else if (fn == "shapeText") o.str (V::shapeText (ints (need (q, "frets"))));
            else if (fn == "pianoVoicing")
            {
                std::vector<int> prev;
                if (const auto* p = q.get ("prev"); p && ! p->isNull()) prev = ints (*p);
                const auto v = V::pianoVoicing (chordFrom (need (q, "chord")), prev);
                o.open ('{').key ("bass").num (v.bass).key ("rh"); nums (o, v.rh);
                o.key ("notes"); nums (o, v.notes);
                o.close ('}');
            }
            else if (fn == "shapeFor") put (o, V::shapeFor (chordFrom (need (q, "chord")), intArg (q, "capo", 0)));
            else if (fn == "capoOptions")
            {
                std::vector<Chord> cs;
                for (const auto& c : need (q, "chords").items) cs.push_back (chordFrom (c));
                list (o, V::capoOptions (cs, coursesFrom (need (q, "courses")), intArg (q, "maxCapo", 7)), [&] (const V::CapoOption& c)
                {
                    o.open ('{').key ("capo").num (c.capo).key ("shapes"); chordList (o, c.shapes);
                    o.key ("ease"); nums (o, c.ease);
                    o.key ("score").num (c.score).key ("all"); chordList (o, c.all);
                    o.close ('}');
                });
            }
            // scales
            else if (fn == "spellScale") scaleNotes (o, S::spellScale (scaleFrom (q), noteFrom (need (q, "root"))));
            else if (fn == "readCost") o.num (S::readCost (scaleFrom (q), noteFrom (need (q, "root"))));
            else if (fn == "easyRoot") put (o, S::easyRoot (intArg (q, "pc", 0), scaleFrom (q)));
            else if (fn == "simplerRoot") put (o, S::simplerRoot (scaleFrom (q), noteFrom (need (q, "root"))));
            else if (fn == "stepSizes") nums (o, S::stepSizes (scaleFrom (q)));
            else if (fn == "stepText") strs (o, S::stepText (scaleFrom (q)));
            else if (fn == "compareScales")
            {
                const auto c = S::compareScales (scaleFrom (q, "a"), scaleFrom (q, "b"));
                o.open ('{').key ("swaps").open ('[');
                for (const auto& [x, y] : c.swaps) { o.next().open ('[').next().str (x).next().str (y).close (']'); }
                o.close (']').key ("added"); strs (o, c.added);
                o.key ("removed"); strs (o, c.removed);
                o.close ('}');
            }
            else if (fn == "scalesWithSemis")
                list (o, S::scalesWithSemis (ints (need (q, "semis"))), [&] (int i) { o.str (scales::library[(size_t) i].id); });
            else if (fn == "familyOf")
            {
                const auto f = S::familyOf (scaleFrom (q));
                if (! f) o.null();
                else o.open ('{').key ("family").str (scales::families[(size_t) f->family].id).key ("mode").num (f->mode)
                      .key ("parentId").str (scales::library[(size_t) f->parent].id).close ('}');
            }
            else if (fn == "rotations")
                list (o, S::rotations (scaleFrom (q), noteFrom (need (q, "root"))), [&] (const S::Rotation& r)
                {
                    o.open ('{').key ("root"); put (o, r.root);
                    o.key ("deg").str (r.deg).key ("k").num (r.k).key ("scales");
                    list (o, r.scales, [&] (int i) { o.str (scales::library[(size_t) i].id); });
                    o.close ('}');
                });
            else if (fn == "parentOf")
            {
                const auto p = S::parentOf (scaleFrom (q), noteFrom (need (q, "root")));
                if (! p) o.null();
                else
                {
                    o.open ('{').key ("scale").str (scales::library[(size_t) p->scale].id).key ("root"); put (o, p->root);
                    o.key ("mode").num (p->mode).key ("family").str (scales::families[(size_t) p->family].id).close ('}');
                }
            }
            else if (fn == "familyModes")
            {
                const auto m = S::familyModes (scaleFrom (q), noteFrom (need (q, "root")));
                if (! m) o.null();
                else list (o, *m, [&] (const S::FamilyMode& x)
                {
                    o.open ('{').key ("scale").str (scales::library[(size_t) x.scale].id).key ("root"); put (o, x.root);
                    o.key ("mode").num (x.mode).key ("current").boolean (x.current).close ('}');
                });
            }
            else if (fn == "typeForSemis")
            {
                const int t = S::typeForSemis (ints (need (q, "semis")));
                if (t < 0) o.null(); else o.str (chords::types[(size_t) t].id);
            }
            else if (fn == "scaleChords")
                list (o, S::scaleChords (scaleFrom (q), noteFrom (need (q, "root"))), [&] (const S::ScaleChord& c)
                {
                    o.open ('{').key ("deg").str (c.deg).key ("root"); put (o, c.root);
                    o.key ("triad"); put (o, c.triad);
                    o.key ("seventh"); put (o, c.seventh);
                    o.close ('}');
                });
            else if (fn == "intervalDeg") o.str (S::intervalDeg (noteFrom (need (q, "a")), noteFrom (need (q, "b"))));
            else if (fn == "chordsInside")
                list (o, S::chordsInside (scaleFrom (q), noteFrom (need (q, "root"))), [&] (const S::InsideChord& c)
                {
                    o.open ('{').key ("deg").str (c.deg).key ("root"); put (o, c.root);
                    o.key ("type").str (chords::types[(size_t) c.type].id).key ("chord"); put (o, c.chord);
                    o.key ("notes"); notes (o, c.notes);
                    o.key ("symbol").str (c.symbol).key ("numeral").str (c.numeral).close ('}');
                });
            else if (fn == "readChord")
            {
                const auto c = S::readChord (textArg (q, "text"));
                if (c) put (o, *c); else o.null();
            }
            else if (fn == "readProgression")
            {
                const auto r = S::readProgression (textArg (q, "text"));
                o.open ('{').key ("chords"); list (o, r.chords, [&] (const S::FitChord& c) { put (o, c); });
                o.key ("bad"); strs (o, r.bad);
                o.close ('}');
            }
            else if (fn == "fitOver") put (o, S::fitOver (scaleFrom (q), noteFrom (need (q, "root")), fitChordFrom (need (q, "chord"))));
            else if (fn == "scalesForChord")
            {
                S::Context context;
                const auto* c = q.get ("context");
                if (c && ! c->isNull()) context.pcs = ints (*c);
                list (o, S::scalesForChord (fitChordFrom (need (q, "chord")), intArg (q, "limit", 8), c && ! c->isNull() ? &context : nullptr),
                      [&] (const S::ScaleChoice& x) { put (o, x); });
            }
            else if (fn == "scalesForProgression")
            {
                std::vector<S::FitChord> cs;
                for (const auto& c : need (q, "chords").items) cs.push_back (fitChordFrom (c));
                const auto r = S::scalesForProgression (cs, intArg (q, "limit", 6));
                o.open ('{').key ("overall"); list (o, r.overall, [&] (const S::ProgressionScale& p) { put (o, p); });
                o.key ("perChord");
                list (o, r.perChord, [&] (const std::vector<S::ScaleChoice>& v) { list (o, v, [&] (const S::ScaleChoice& x) { put (o, x); }); });
                o.key ("tonic");
                if (r.tonic < 0) o.null(); else o.num (r.tonic);
                o.key ("blues").boolean (r.blues).key ("best");
                if (r.best < 0) o.null(); else o.num (r.best);
                o.close ('}');
            }
            // a scale on a fretted neck
            else if (fn == "neckNotes")
                neckNotes (o, S::neckNotes (scaleFrom (q), noteFrom (need (q, "root")), coursesFrom (need (q, "courses")), intArg (q, "capo", 0), intArg (q, "frets", 15)));
            else if (fn == "boxPositions" || fn == "perStringPatterns")
            {
                const auto& scale = scaleFrom (q);
                const auto root = noteFrom (need (q, "root"));
                const auto courses = coursesFrom (need (q, "courses"));
                const int capo = intArg (q, "capo", 0), frets = intArg (q, "frets", 15);
                const bool box = fn == "boxPositions";
                const auto ps = box ? S::boxPositions (scale, root, courses, capo, frets, boolArg (q, "standard", false))
                                    : S::perStringPatterns (scale, root, courses, capo, frets);
                list (o, ps, [&] (const S::NeckPattern& p)
                {
                    o.open ('{').key ("lo").num (p.lo).key ("hi").num (p.hi).key ("notes"); neckNotes (o, p.notes);
                    if (box) o.key ("s").num (p.s).key ("shape").str (p.shape);
                    else o.key ("startDeg").str (p.startDeg).key ("perString").num (p.perString);
                    o.close ('}');
                });
            }
            else if (fn == "isStandardSix") o.boolean (S::isStandardSix (coursesFrom (need (q, "courses"))));
            else if (fn == "upAndDown")
            {
                std::optional<S::NeckPattern> pattern;
                if (const auto* p = q.get ("pattern"); p && ! p->isNull())
                {
                    S::NeckPattern np;
                    for (const auto& x : need (*p, "notes").items)
                        np.notes.push_back ({ intArg (x, "course", 0), intArg (x, "fret", 0), intArg (x, "midi", 0), intArg (x, "pc", 0), textArg (x, "d"), noteFrom (need (x, "note")) });
                    pattern = np;
                }
                list (o, S::upAndDown (scaleFrom (q), noteFrom (need (q, "root")), pattern ? &*pattern : nullptr, intArg (q, "around", 52)),
                      [&] (const S::RunNote& r)
                      {
                          o.open ('{').key ("midi").num (r.midi);
                          if (r.course >= 0) o.key ("course").num (r.course).key ("fret").num (r.fret);
                          o.key ("pc").num (r.pc).key ("d").str (r.d).key ("note"); put (o, r.note);
                          o.close ('}');
                      });
            }
            // a song's key and a capo for a shift
            else if (fn == "guessKey")
            {
                std::vector<Chord> cs;
                for (const auto& x : need (q, "chords").items)
                    if (x.type == Json::Type::string) { if (const auto c = H::parseChord (x.s)) cs.push_back (*c); }
                const auto k = H::guessKey (cs);
                if (k) put (o, *k); else o.null();
            }
            else if (fn == "capoForShift")
            {
                const auto c = H::capoForShift (intArg (q, "semis", 0), intArg (q, "maxCapo", 7));
                if (c) o.num (*c); else o.null();
            }
            // voice exercises
            else if (fn == "keyShiftsForRange")
                list (o, voice::keyShiftsForRange (numArg (q, "songLow", 0), numArg (q, "songHigh", 0), rangeFrom (need (q, "range")), intArg (q, "originalPc", -1)),
                      [&] (const voice::KeyShift& k)
                      {
                          o.open ('{').key ("shift").num (k.shift).key ("low").num (k.low).key ("high").num (k.high)
                           .key ("fits").boolean (k.fits).key ("comfortable").boolean (k.comfortable)
                           .key ("outside").num (k.outside).key ("strain").num (k.strain).key ("keyPc");
                          if (k.keyPc < 0) o.null(); else o.num (k.keyPc);
                          o.key ("rank").num (k.rank).close ('}');
                      });
            else if (fn == "nextMatchNote")
            {
                std::optional<double> prev;
                if (const auto* p = q.get ("prev"); p && ! p->isNull()) prev = p->n;
                o.num (voice::nextMatchNote (intArg (q, "level", 1), rangeFrom (need (q, "range")), prev, randFrom (q)));
            }
            else if (fn == "buildSingBack")
                list (o, voice::buildSingBack (intArg (q, "level", 1), rangeFrom (need (q, "range")), randFrom (q)), [&] (double m) { o.num (m); });
            else if (fn == "buildWarmup")
                list (o, voice::buildWarmup (rangeFrom (need (q, "range")), boolArg (q, "full", false)), [&] (const voice::WarmupExercise& e)
                {
                    o.open ('{').key ("id").str (e.id).key ("patterns");
                    list (o, e.patterns, [&] (const voice::WarmupPattern& p)
                    {
                        o.open ('{');
                        if (p.glide) o.key ("glide").open ('{').key ("from").num (p.glide->from).key ("to").num (p.glide->to)
                                      .key ("up").num (p.glide->up).key ("down").num (p.glide->down).close ('}');
                        else
                        {
                            o.key ("notes");
                            list (o, p.notes, [&] (const voice::PatternNote& n)
                            {
                                o.open ('{').key ("midi").num (n.midi).key ("at").num (n.at).key ("dur").num (n.dur).close ('}');
                            });
                        }
                        o.close ('}');
                    });
                    o.close ('}');
                });
            else fail ("no such function: " + fn);
        }
    }

    // Answers one request (see the top of this file)
    inline std::string call (std::string_view request)
    {
        Out o;
        const auto q = parse (request);
        if (! q || q->type != Json::Type::object) return R"({"error":"not a JSON object"})";
        detail::problem().clear();
        detail::call (detail::textArg (*q, "fn"), *q, o);
        if (! detail::problem().empty())
        {
            Out e;
            e.open ('{').key ("error").str (detail::problem()).close ('}');
            return e.text;
        }
        return o.text;
    }
}
