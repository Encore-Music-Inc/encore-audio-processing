#pragma once

// A small JSON value, reader and writer, for the browser build's calls that
// take a request and give an answer as text (the theory's th_call, the
// restoration's settings and reports) and the reference set. Numbers are
// written rounded to nine decimals, so every build writes the same text.
// Plain C++20, no JUCE.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vs::json
{
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
}
