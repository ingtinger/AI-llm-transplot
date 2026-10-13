// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// service_tier, speed and inference_geo spliced into a body forwarded as it arrives, with
// any model substitution: GATEWAY-INTERNALS.md 12e. Apart from request.cpp, whose byte-forward
// rebuild every request runs, so its code generation is not this file's to move.

#include "request.hpp"

#include <utility>

namespace llmbridge::detail
{
    namespace
    {
        constexpr std::string_view kMemberKeys[] = {"service_tier", "speed", "inference_geo"};
    } // namespace

    uint8_t MemberSplice::mask() const noexcept
    {
        return static_cast<uint8_t>((service_tier.empty() ? 0 : provider::BodyScan::kServiceTier) |
                                    (speed.empty() ? 0 : provider::BodyScan::kSpeed) |
                                    (inference_geo.empty() ? 0 : provider::BodyScan::kInferenceGeo));
    }

    size_t MemberSplice::grow() const noexcept
    {
        const std::string_view values[] = {service_tier, speed, inference_geo};
        size_t n = 0;
        for (size_t i = 0; i < 3; ++i)
            if (!values[i].empty()) n += kMemberKeys[i].size() + values[i].size() + 6; // "k":"v" and a separator
        return n;
    }

    void MemberSplice::append_members(std::string& out) const
    {
        const std::string_view values[] = {service_tier, speed, inference_geo};
        bool first = true;
        for (size_t i = 0; i < 3; ++i)
        {
            if (values[i].empty()) continue;
            out.append(first ? "\"" : ",\"").append(kMemberKeys[i]).append("\":\"").append(values[i]).append(1, '"');
            first = false;
        }
    }

    bool MemberSplice::plain() const noexcept
    {
        for (const std::string_view v : {service_tier, speed, inference_geo})
            for (const char ch : v)
            {
                const auto u = static_cast<unsigned char>(ch);
                if (u < 0x20 || u == 0x7F || ch == '"' || ch == '\\') return false;
            }
        return true;
    }

    bool Splice::substitutes(std::string_view body, const provider::BodyScan& s) const noexcept
    {
        return !model.sentinel.empty() && model.matches(body, s.model_at(), s.model_end());
    }

    // The scan holds each span back until it closes, and the root's `{` until its first
    // member is known, so a range holds a span whole or not at all.
    void Splice::append(std::string& out, std::string_view body, size_t from, size_t to, size_t body_len,
                        const provider::BodyScan& s) const
    {
        const uint8_t mask = members.mask();
        if (!mask)
        {
            if (!model.sentinel.empty()) model.append(out, body, from, to, body_len, s.model_at(), s.model_end());
            else out.append(body.substr(from, to - from));
            return;
        }
        // What replaces a span: the model's value, or a member of the client's ours replace.
        struct Cut
        {
            size_t at, end;
            bool model;
        };
        Cut cuts[4];
        size_t n = 0;
        bool first_cut = false; // the root's first member is one of them
        for (size_t i = 0; i < s.member_count(); ++i)
        {
            const provider::BodyScan::Member& m = s.members()[i];
            if (!(m.bit & mask)) continue; // tracked for another attempt: goes as sent
            cuts[n++] = {m.at, m.end, false};
            first_cut = first_cut || m.at == s.open_at();
        }
        const bool hit = substitutes(body, s);
        if (hit) cuts[n++] = {s.model_at(), s.model_end(), true};
        for (size_t i = 1; i < n; ++i) // at most four: an insertion sort
            for (size_t j = i; j > 0 && cuts[j - 1].at > cuts[j].at; --j) std::swap(cuts[j - 1], cuts[j]);

        size_t pos = from;
        if (const size_t open = s.open_at(); open >= from && open < to)
        {
            out.append(body.substr(pos, open - pos));
            members.append_members(out);
            // The comma apply_overrides writes, before a member that stays; else whitespace.
            out.push_back(s.has_member() && !first_cut ? ',' : ' ');
            pos = open;
        }
        for (size_t i = 0; i < n; ++i)
        {
            const Cut& c = cuts[i];
            if (c.at < pos || c.at >= to) continue;
            out.append(body.substr(pos, c.at - pos));
            if (c.model)
                out.append(1, '"').append(model.replacement).append(1, '"').append(model.shrink(), ' ');
            else out.append(c.end - c.at, ' ');
            pos = c.end;
        }
        if (pos < to) out.append(body.substr(pos, to - pos));
        if (to == body_len && !model.sentinel.empty() && !hit) out.append(model.grow(), ' ');
    }
} // namespace llmbridge::detail
