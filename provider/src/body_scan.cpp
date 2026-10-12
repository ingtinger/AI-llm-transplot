// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// A resumable RFC 8259 recognizer, one state per place a byte can be, with the
// top-level key rules of top_level_facts applied as each key closes.

#include "provider/body_scan.hpp"

#include <bit>
#include <cstring>

#include "provider/json.hpp" // json::detail::kMaxDepth

namespace llmbridge::provider
{
    enum class BodyScan::St : uint8_t
    {
        Value,    // a value is due: the root, after ':' or after ',' in an array
        ArrFirst, // after '[': a value or ']'
        ObjFirst, // after '{': a key or '}'
        ObjKey,   // after ',' in an object: a key
        Colon,
        ObjNext,  // after a member: ',' or '}'
        ArrNext,  // after an element: ',' or ']'
        Str,
        StrEsc,
        StrHex,
        NumSign,
        NumZero,
        NumInt,
        NumDot,
        NumFrac,
        NumE,
        NumESign,
        NumExp,
        Lit,
        End,
    };

    namespace
    {
        constexpr std::string_view kLits[] = {"true", "false", "null"};
        constexpr size_t kMaxKeys = 256; // top_level_facts' limit

        bool ws(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
        bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
        bool hex(char c) noexcept
        {
            return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        // The first byte at or after `i` that ends a plain run inside a string: a quote,
        // a backslash or a control byte. Eight bytes a step, since strings are the bulk.
        size_t plain_run(std::string_view b, size_t i) noexcept
        {
            constexpr uint64_t kLo = 0x0101010101010101ULL, kHi = 0x8080808080808080ULL;
            const char* p = b.data();
            const size_t n = b.size();
            for (; i + 8 <= n; i += 8)
            {
                uint64_t v;
                std::memcpy(&v, p + i, 8);
                const uint64_t q = v ^ (kLo * '"'), s = v ^ (kLo * '\\');
                // Each term's lowest set bit is exact, so the first hit is the lowest byte.
                const uint64_t hit = (((q - kLo) & ~q) | ((s - kLo) & ~s) | ((v - kLo * 0x20) & ~v)) & kHi;
                if (hit && std::endian::native == std::endian::little)
                    return i + static_cast<size_t>(std::countr_zero(hit)) / 8;
                if (hit) break;
            }
            for (; i < n; ++i)
            {
                const auto c = static_cast<unsigned char>(p[i]);
                if (c == '"' || c == '\\' || c < 0x20) return i;
            }
            return n;
        }
    } // namespace

    void BodyScan::reset() noexcept
    {
        _top.clear();
        _options.clear();
        _kinds = 0;
        _pos = _str = 0;
        _hold = SIZE_MAX;
        _depth = _options_depth = 0;
        _st = St::Value;
        _lit = _hex = 0;
        _key = _tracked = _options_next = false;
        _why = nullptr;
        _verdict = Verdict::More;
    }

    BodyScan::Verdict BodyScan::refuse(const char* why) noexcept
    {
        _why = why;
        return _verdict = Verdict::Refused;
    }

    BodyScan::Verdict BodyScan::feed(std::string_view body, bool last) noexcept
    {
        if (_verdict != Verdict::More) return _verdict;
        size_t i = _pos;
        while (i < body.size())
            if (!step(body, i)) return refuse(_why ? _why : "body is not valid JSON");
        _pos = i;
        if (!last) return Verdict::More;
        // A number at the root ends with the body.
        if (_depth == 0 && (_st == St::NumZero || _st == St::NumInt || _st == St::NumFrac || _st == St::NumExp))
            _st = St::End;
        return _st == St::End ? (_verdict = Verdict::Done) : refuse("body ends inside a value");
    }

    // After a value of any kind: what its container expects next.
    bool BodyScan::value_done() noexcept
    {
        if (_depth == 0) _st = St::End;
        else _st = (_kinds >> (_depth - 1)) & 1 ? St::ObjNext : St::ArrNext;
        return true;
    }

    // A key at a checked level closed at `end`: refused if escaped, repeated or one too many.
    bool BodyScan::key_done(std::string_view body, size_t end) noexcept
    {
        _key = false;
        if (!_tracked) return true;
        _hold = SIZE_MAX;
        const std::string_view k = body.substr(_str, end - _str);
        std::vector<Span>& set = _depth == 1 ? _top : _options;
        if (k.find('\\') != std::string_view::npos) { _why = "escaped top-level key"; return false; }
        if (set.size() == kMaxKeys) { _why = "too many top-level keys"; return false; }
        for (const Span& s : set)
            if (s.len == k.size() && std::memcmp(body.data() + s.at, k.data(), k.size()) == 0)
            {
                _why = "repeated top-level key";
                return false;
            }
        set.push_back({static_cast<uint32_t>(_str), static_cast<uint32_t>(k.size())});
        if (_depth == 1) _options_next = k == "stream_options";
        return true;
    }

    // One byte, or a run of plain string bytes. False refuses; `i` stays on a byte to
    // reread when a number ended at it.
    bool BodyScan::step(std::string_view b, size_t& i) noexcept
    {
        const char c = b[i];
        switch (_st)
        {
            case St::Str:
            {
                // A two-byte escape is taken in the run: prose carries one every few words.
                for (i = plain_run(b, i); i + 1 < b.size() && b[i] == '\\'; i = plain_run(b, i + 2))
                {
                    const char e = b[i + 1];
                    if (e == 'u') break;
                    if (!(e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' ||
                          e == 'r' || e == 't'))
                        return false;
                }
                if (i == b.size()) return true;
                const char d = b[i++];
                if (d == '\\') { _st = St::StrEsc; return true; }
                if (d != '"') { _why = "control byte in a string"; return false; }
                if (!_key) return value_done();
                _st = St::Colon;
                return key_done(b, i - 1);
            }
            case St::StrEsc:
                ++i;
                if (c == 'u') { _st = St::StrHex; _hex = 0; return true; }
                _st = St::Str;
                return c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' || c == 'n' ||
                       c == 'r' || c == 't';
            case St::StrHex:
                ++i;
                if (++_hex == 4) _st = St::Str;
                return hex(c);
            case St::Lit:
            {
                const std::string_view lit = kLits[_lit >> 3];
                if (c != lit[_lit & 7]) return false;
                ++i;
                if ((++_lit & 7) == lit.size()) return value_done();
                return true;
            }
            case St::NumSign:
                ++i;
                _st = c == '0' ? St::NumZero : St::NumInt;
                return digit(c);
            case St::NumZero:
            case St::NumInt:
                if (digit(c))
                {
                    ++i;
                    return _st == St::NumInt; // a leading zero stands alone
                }
                if (c == '.') { ++i; _st = St::NumDot; return true; }
                if (c == 'e' || c == 'E') { ++i; _st = St::NumE; return true; }
                return value_done(); // `c` is reread by the container
            case St::NumDot:
                ++i;
                _st = St::NumFrac;
                return digit(c);
            case St::NumFrac:
                if (digit(c)) { ++i; return true; }
                if (c == 'e' || c == 'E') { ++i; _st = St::NumE; return true; }
                return value_done();
            case St::NumE:
                ++i;
                if (c == '+' || c == '-') { _st = St::NumESign; return true; }
                _st = St::NumExp;
                return digit(c);
            case St::NumESign:
                ++i;
                _st = St::NumExp;
                return digit(c);
            case St::NumExp:
                if (digit(c)) { ++i; return true; }
                return value_done();
            default:
                break;
        }
        if (ws(c)) { ++i; return true; }
        ++i;
        switch (_st)
        {
            case St::Colon:
                _st = St::Value;
                return c == ':';
            case St::ObjFirst:
            case St::ObjKey:
                if (c == '}' && _st == St::ObjFirst) break; // an empty object closes
                if (c != '"') return false;
                _key = true;
                _tracked = _depth == 1 || (_options_depth != 0 && _depth == _options_depth);
                if (_tracked) _hold = i - 1;
                _str = i;
                _st = St::Str;
                return true;
            case St::ObjNext:
                if (c == ',') { _st = St::ObjKey; return true; }
                if (c != '}') return false;
                break;
            case St::ArrNext:
                if (c == ',') { _st = St::Value; return true; }
                if (c != ']') return false;
                break;
            case St::ArrFirst:
                if (c == ']') break;
                [[fallthrough]];
            case St::Value:
            {
                // A value starts. `stream_options`' keys are checked when it is an object.
                const bool options = _options_next;
                _options_next = false;
                if (c == '"') { _key = false; _st = St::Str; return true; }
                if (c == '{' || c == '[')
                {
                    if (_depth == static_cast<uint32_t>(json::detail::kMaxDepth)) { _why = "body nests too deep"; return false; }
                    const uint64_t bit = uint64_t{1} << _depth;
                    _kinds = c == '{' ? (_kinds | bit) : (_kinds & ~bit);
                    ++_depth;
                    if (c == '{' && options && _depth == 2) _options_depth = 2;
                    _st = c == '{' ? St::ObjFirst : St::ArrFirst;
                    return true;
                }
                if (c == 't' || c == 'f' || c == 'n')
                {
                    _lit = static_cast<uint8_t>((c == 't' ? 0 : c == 'f' ? 1 : 2) << 3 | 1);
                    _st = St::Lit;
                    return true;
                }
                if (c == '-') { _st = St::NumSign; return true; }
                if (c == '0') { _st = St::NumZero; return true; }
                if (digit(c)) { _st = St::NumInt; return true; }
                return false;
            }
            default: // End: only whitespace may follow the root
                _why = "content after the body";
                return false;
        }
        // A container closed.
        --_depth;
        if (_depth < _options_depth) _options_depth = 0;
        return value_done();
    }
} // namespace llmbridge::provider
