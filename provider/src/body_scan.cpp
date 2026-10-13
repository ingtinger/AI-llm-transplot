// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// A resumable RFC 8259 recognizer, one state per place a byte can be, with the
// top-level key rules of top_level_facts applied as each key closes.

#include "provider/body_scan.hpp"

#include <algorithm>
#include <cstring>

#include "provider/json.hpp" // kMaxDepth, string_stop

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
        using json::detail::string_stop;

        bool ws(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
        uint8_t member_bit(std::string_view k) noexcept
        {
            if (k == "service_tier") return BodyScan::kServiceTier;
            if (k == "speed") return BodyScan::kSpeed;
            if (k == "inference_geo") return BodyScan::kInferenceGeo;
            return 0;
        }
        bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
        bool hex(char c) noexcept
        {
            return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }
    } // namespace

    void BodyScan::reset(bool all_keys, bool track_model, uint8_t members) noexcept
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
        _all_keys = all_keys;
        _track_model = track_model;
        _model_next = _model_open = _model_seen = false;
        _model_at = _model_end = SIZE_MAX;
        _members = members;
        _members_seen = _nfound = _member_bit = 0;
        _open_at = SIZE_MAX;
        _member_at = _member_value = 0;
        _has_member = _member_next = _member_open = false;
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

    // After a value of any kind, ending before `end`: what its container expects next.
    bool BodyScan::value_done(size_t end) noexcept
    {
        if (_model_open && _depth == 1)
        {
            // The tracked model closed: its bytes may go, or something else in their place.
            _model_open = false;
            _model_end = end;
            _hold = SIZE_MAX;
        }
        if (_member_open && _depth == 1)
        {
            // A member the mask names closed: told whole, with the separator before it.
            _member_open = false;
            _found[_nfound++] = {_member_at, _member_value, end, _member_bit};
            _hold = SIZE_MAX;
        }
        // The root closed: an empty object held from its `{` lets it go.
        if (_depth == 0 && _members) _hold = SIZE_MAX;
        if (_depth == 0) _st = St::End;
        else _st = (_kinds >> (_depth - 1)) & 1 ? St::ObjNext : St::ArrNext;
        return true;
    }

    // A key at a checked level closed at `end`: refused if escaped, repeated or one too many.
    bool BodyScan::key_done(std::string_view body, size_t end) noexcept
    {
        _key = false;
        if (!_tracked) return true;
        const std::string_view k = body.substr(_str, end - _str);
        std::vector<Span>& set = _depth == 1 ? _top : _options;
        if (k.find('\\') != std::string_view::npos) { _why = "escaped top-level key"; return false; }
        if (_depth == 1 && _members)
        {
            // Held from the separator before it: a member the mask names until its value
            // closes, any other one only until now.
            _has_member = true;
            const uint8_t bit = member_bit(k) & _members;
            if (bit)
            {
                if (_members_seen & bit) { _why = "repeated top-level key"; return false; }
                _members_seen |= bit;
                _member_bit = bit;
                _member_at = _hold;
                _member_next = true;
            }
            else _hold = SIZE_MAX;
        }
        else _hold = SIZE_MAX;
        if (_depth == 1 && k == "model")
        {
            if (_model_seen) { _why = "repeated top-level key"; return false; }
            _model_seen = true;
            _model_next = _track_model;
        }
        if (!_all_keys) return true;
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
                const char* p = b.data();
                const size_t n = b.size();
                for (i = string_stop(p, i, n); i + 1 < n && p[i] == '\\'; i = string_stop(p, i + 2, n))
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
                if (!_key) return value_done(i);
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
                if ((++_lit & 7) == lit.size()) return value_done(i);
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
                return value_done(i); // `c` is reread by the container
            case St::NumDot:
                ++i;
                _st = St::NumFrac;
                return digit(c);
            case St::NumFrac:
                if (digit(c)) { ++i; return true; }
                if (c == 'e' || c == 'E') { ++i; _st = St::NumE; return true; }
                return value_done(i);
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
                return value_done(i);
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
                if (_tracked) _hold = std::min(_hold, i - 1); // a separator held before it stays
                _str = i;
                _st = St::Str;
                return true;
            case St::ObjNext:
                if (c == ',')
                {
                    if (_members && _depth == 1) _hold = i - 1; // with the member after it
                    _st = St::ObjKey;
                    return true;
                }
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
                if (_model_next)
                {
                    // Held from its first byte until it closes (value_done).
                    _model_next = false;
                    _model_open = true;
                    _model_at = _hold = i - 1;
                }
                if (_members)
                {
                    // Members go in after the root's `{` and replace a string: anything else
                    // is refused, as the held rewrite (apply_overrides) refuses it.
                    if (_depth == 0 && c != '{') { _why = "body is not a JSON object"; return false; }
                    if (_member_next)
                    {
                        _member_next = false;
                        if (c != '"') { _why = "top-level override value not a string"; return false; }
                        _member_open = true;
                        _member_value = i - 1;
                    }
                }
                if (c == '"') { _key = false; _st = St::Str; return true; }
                if (c == '{' || c == '[')
                {
                    if (_depth == static_cast<uint32_t>(json::detail::kMaxDepth)) { _why = "body nests too deep"; return false; }
                    const uint64_t bit = uint64_t{1} << _depth;
                    _kinds = c == '{' ? (_kinds | bit) : (_kinds & ~bit);
                    ++_depth;
                    // The first member waits, with the `{`'s insertion, until it is known.
                    if (_members && _depth == 1) _open_at = _hold = i;
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
        return value_done(i);
    }
} // namespace llmbridge::provider
