// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Differential target: a byte-forwarded OpenAI body sent as it arrives, chunk by chunk
// through BodyScan, against the same body held and sent whole. The streamed bytes are
// always a prefix of the held ones; they are all of them exactly when the body is JSON
// whose top-level keys the gateway's readers agree on, as json::parse and
// top_level_facts decide it; otherwise they stop short of the declared length.
//
// With the first byte's top bit set, a model substitution is spliced in as it streams
// (GATEWAY-INTERNALS.md 12d), for an OpenAI body or, with bit 6, another dialect's: the
// bytes sent equal the held rewrite (rewrite_model) when the model is the sentinel, plus
// spaces after it for a shorter replacement, else the body plus trailing spaces, and
// their length is the declared Content-Length; the sentinel is matched exactly when the
// gateway's own model reader (model_of) reads it.
//
// With bit 3 set too, service_tier, speed and inference_geo overrides (one of four sets,
// bits 1 and 2) are spliced into the same stream (GATEWAY-INTERNALS.md 12e): when the scan
// takes the body whole, the bytes sent are the declared length, valid JSON with one model and
// one of each member we set, exactly the root's `{`, our members, a separator, and the
// client's bytes with each member we replace blanked from the separator before it; and they
// equal the held rewrite (apply_overrides) but for whitespace and member order.

#include "provider/body_scan.hpp"
#include "provider/json.hpp"
#include "provider/translate.hpp"
#include "request.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using namespace llmbridge;

    /// A value as its raw text, quotes around a string, for comparing two parses.
    std::string raw_of(const provider::json::Value& v)
    {
        using T = provider::json::Value::Type;
        switch (v.type)
        {
            case T::String: return "\"" + std::string(v.sv) + "\"";
            case T::Bool: return v.boolean ? "true" : "false";
            case T::Null: return "null";
            default: return std::string(v.sv);
        }
    }

    /// The root object's members as sorted (key, raw value) pairs; empty when not an object.
    std::vector<std::pair<std::string, std::string>> members_of(std::string_view body, bool& object)
    {
        bool ok = false;
        const provider::json::Value v = provider::json::parse(body, ok, provider::json::Keys::Any);
        object = ok && v.is_object();
        std::vector<std::pair<std::string, std::string>> out;
        if (!object) return out;
        for (const auto& [k, val] : v.obj) out.emplace_back(std::string(k), raw_of(val));
        std::sort(out.begin(), out.end());
        return out;
    }

    /// The members spliced, built from json::parse's spans rather than BodyScan's: ours after
    /// the root's `{`, then `,` before a member the client keeps or a space, and each client
    /// member we replace blanked from the separator before it through its value.
    std::string splice_reference(std::string_view body, const detail::MemberSplice& m)
    {
        bool ok = false;
        const provider::json::Value v = provider::json::parse(body, ok, provider::json::Keys::Any);
        assert(ok && v.is_object());
        const char* base = body.data();
        const size_t open = static_cast<size_t>(v.sv.data() - base) + 1;
        const std::string_view set[] = {m.service_tier, m.speed, m.inference_geo};
        const std::string_view keys[] = {"service_tier", "speed", "inference_geo"};
        std::string out(body);
        bool first_blanked = false;
        for (size_t i = 0; i < v.obj.size(); ++i)
        {
            const auto& [k, val] = v.obj[i];
            bool ours = false;
            for (size_t j = 0; j < 3; ++j) ours = ours || (!set[j].empty() && k == keys[j]);
            if (!ours) continue;
            assert(val.is_string());
            size_t at = static_cast<size_t>(k.data() - base) - 1; // the key's quote
            while (at > open && (body[at - 1] == ' ' || body[at - 1] == '\t' || body[at - 1] == '\n' ||
                                 body[at - 1] == '\r'))
                --at;
            if (at > open) --at; // the comma before it
            first_blanked = first_blanked || i == 0;
            const size_t end = static_cast<size_t>(val.sv.data() - base) + val.sv.size() + 1;
            out.replace(at, end - at, end - at, ' ');
        }
        std::string ins;
        m.append_members(ins);
        ins.push_back(!v.obj.empty() && !first_blanked ? ',' : ' ');
        out.insert(open, ins);
        return out;
    }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    using namespace llmbridge;
    if (size < 1) return 0;
    uint32_t rng = data[0] | 1u; // the chunk sizes
    const bool spliced = (data[0] & 0x80) != 0;
    const bool all_keys = (data[0] & 0x40) == 0;
    static constexpr std::string_view kPairs[][2] = {
        {"auto", "gpt-4o-mini"}, {"auto-router", "o3"}, {"auto", "gpt4"}, {"m", "claude-sonnet-4-5"}};
    const detail::ModelSplice model{kPairs[(data[0] >> 4) & 3][0], kPairs[(data[0] >> 4) & 3][1]};
    static constexpr std::string_view kSets[][3] = {
        {"flex", "", ""}, {"", "fast", "us"}, {"priority", "fast", "eu"}, {"auto", "", "us"}};
    const bool overrides = spliced && (data[0] & 0x08) != 0;
    const std::string_view* set = kSets[(data[0] >> 1) & 3];
    const detail::Splice splice{model, overrides ? detail::MemberSplice{set[0], set[1], set[2]} : detail::MemberSplice{}};
    const std::string_view body(reinterpret_cast<const char*>(data + 1), size - 1);
    const size_t declared = body.size() + (spliced ? splice.grow() : 0);
    const std::string head = "POST /v1/chat/completions HTTP/1.1\r\nHost: client\r\nContent-Type: "
                             "application/json\r\nX-Kottos-Key-Fuzz: k\r\nConnection: keep-alive\r\n"
                             "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    const std::vector<std::string> strip;
    HeadFacts f;
    if (!net::http::walk_headers(std::string_view(head).substr(0, head.size() - 4),
                                 detail::HeadReader{head, strip, f}))
        return 0;
    std::string held, sent;
    const bool built = detail::build_forward(head, head.size(), f, strip, "venue", "", body, held,
                                             body.size()) == nullptr;
    assert(built);

    bool ok = false;
    (void)provider::json::parse(body, ok, provider::json::Keys::Any);
    const bool good = ok && provider::top_level_facts(body).keys == provider::KeyCheck::Ok;

    provider::BodyScan scan;
    if (spliced) scan.reset(all_keys, /*track_model=*/true, splice.members.mask());
    else scan.reset();
    size_t have = 0, out = 0;
    provider::BodyScan::Verdict v = provider::BodyScan::Verdict::More;
    bool first = true;
    do
    {
        rng = rng * 1103515245u + 12345u;
        have = std::min(body.size(), have + 1 + (rng >> 16) % 97);
        v = scan.feed(body.substr(0, have), have == body.size());
        if (v == provider::BodyScan::Verdict::Refused) break;
        assert(scan.safe() >= out && scan.safe() <= have);
        // Never a split model value: the scan holds it whole or lets it go whole.
        assert(scan.model_end() != SIZE_MAX || scan.model_at() == SIZE_MAX || scan.safe() <= scan.model_at());
        if (first)
        {
            // The head goes with whatever is already known good.
            const bool ok_head = detail::build_forward(head, head.size(), f, strip, "venue", "",
                                                       spliced ? std::string_view{} : body.substr(0, scan.safe()),
                                                       sent, declared) == nullptr;
            assert(ok_head);
            if (spliced) splice.append(sent, body.substr(0, have), 0, scan.safe(), body.size(), scan);
            first = false;
        }
        else if (spliced) splice.append(sent, body.substr(0, have), out, scan.safe(), body.size(), scan);
        else sent.append(body.substr(out, scan.safe() - out));
        out = scan.safe();
    } while (have < body.size());

    // No key family header in either head, whatever the body holds.
    for (const std::string* s : {&held, &sent})
        assert(!net::http::detail::contains_ci(s->substr(0, s->find("\r\n\r\n")), "kottos-key"));
    const size_t head_len = sent.empty() ? 0 : sent.find("\r\n\r\n") + 4;
    if (!spliced)
    {
        assert(held.compare(0, sent.size(), sent) == 0); // never a byte the held request lacks
        if (v == provider::BodyScan::Verdict::Done)
        {
            assert(good);
            assert(sent == held);
        }
        else
        {
            assert(!good);
            assert(sent.size() < held.size()); // short of the declared length
        }
        return 0;
    }
    if (v != provider::BodyScan::Verdict::Done)
    {
        // With members a JSON body is refused too when its root is not an object or a
        // member we replace is not a string, which the held rewrite refuses as well.
        if (all_keys && !overrides) assert(!good);
        assert(sent.empty() || sent.size() - head_len < declared); // never a whole request
        return 0;
    }
    // The length is ours: the client's, and what the replacement and members add.
    const std::string length = "\r\nContent-Length: " + std::to_string(declared) + "\r\n";
    assert(sent.substr(0, head_len).find(length) != std::string::npos);
    if (all_keys) assert(good);
    assert(ok);
    const std::string_view got = std::string_view(sent).substr(head_len);
    assert(got.size() == declared);
    const size_t at = scan.model_at(), end = scan.model_end();
    const bool hit = model.matches(body, at, end);
    assert(hit == splice.substitutes(body, scan));
    // The splice and the gateway's own model reader agree on whether the sentinel is there,
    // so a confirm() that sees model == sentinel sees a body that went substituted.
    assert(hit == (provider::model_of(body) == model.sentinel));
    // The members first, at the client's offsets plus what they add before the model.
    std::string want = overrides ? splice_reference(body, splice.members) : std::string(body);
    const size_t shift = splice.members.grow();
    if (hit)
    {
        // The held rewrite, but for the spaces a shorter replacement leaves after it.
        if (!overrides)
        {
            const std::string rewrite = provider::rewrite_model(body, model.replacement);
            if (!rewrite.empty())
                assert(rewrite == std::string(body.substr(0, at)) + "\"" + std::string(model.replacement) + "\"" +
                                      std::string(body.substr(end)));
        }
        want.replace(at + shift, end - at,
                     "\"" + std::string(model.replacement) + "\"" + std::string(model.shrink(), ' '));
    }
    else want.append(model.grow(), ' ');
    assert(got == want);
    bool parsed = false;
    (void)provider::json::parse(got, parsed, provider::json::Keys::Any);
    assert(parsed);
    if (!overrides) return 0;

    // Never two of the model, nor of a member the policy set: the venue reads the one the
    // policy set. Another dialect's body may repeat a member we leave alone, both the client's.
    bool object = false;
    const auto sent_members = members_of(got, object);
    assert(object);
    const std::pair<std::string_view, uint8_t> keys[] = {{"service_tier", provider::BodyScan::kServiceTier},
                                                         {"speed", provider::BodyScan::kSpeed},
                                                         {"inference_geo", provider::BodyScan::kInferenceGeo},
                                                         {"model", 0}};
    for (const auto& [k, bit] : keys)
        if (all_keys || bit == 0 || (splice.members.mask() & bit))
            assert(std::count_if(sent_members.begin(), sent_members.end(),
                                 [k](const auto& m) { return m.first == k; }) <= 1);
    if (all_keys) assert(provider::top_level_facts(got).keys == provider::KeyCheck::Ok);
    // The held rewrite has the same members with the same values. It refuses a body that
    // repeats a key at any depth (json::parse's unique keys), which streams.
    std::string rewrite;
    const provider::Overrides o{hit ? model.replacement : std::string_view{}, splice.members.service_tier,
                                splice.members.speed, splice.members.inference_geo};
    if (!provider::apply_overrides(body, o, nullptr, rewrite))
    {
        bool unique = false;
        (void)provider::json::parse(body, unique);
        assert(!unique);
        return 0;
    }
    bool held_object = false;
    assert(members_of(rewrite, held_object) == sent_members);
    assert(held_object);
    // With none of the client's own replaced, the same bytes but for spaces we add.
    if (scan.member_count() == 0)
    {
        std::string squeezed(got);
        if (hit) squeezed.erase(at + shift + model.replacement.size() + 2, model.shrink());
        else squeezed.erase(squeezed.size() - model.grow());
        if (scan.has_member()) assert(squeezed == rewrite);
        else // an empty object: the space where the comma would be
            assert(squeezed.erase(scan.open_at() + shift - 1, 1) == rewrite);
    }
    return 0;
}
