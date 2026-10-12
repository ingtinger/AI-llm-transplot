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

#include "provider/body_scan.hpp"
#include "provider/json.hpp"
#include "provider/translate.hpp"
#include "request.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    using namespace llmbridge;
    if (size < 1) return 0;
    uint32_t rng = data[0] | 1u; // the chunk sizes
    const bool spliced = (data[0] & 0x80) != 0;
    const bool all_keys = (data[0] & 0x40) == 0;
    static constexpr std::string_view kPairs[][2] = {
        {"auto", "gpt-4o-mini"}, {"auto-router", "o3"}, {"auto", "gpt4"}, {"m", "claude-sonnet-4-5"}};
    const detail::ModelSplice splice{kPairs[(data[0] >> 4) & 3][0], kPairs[(data[0] >> 4) & 3][1]};
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
    if (spliced) scan.reset(all_keys, /*track_model=*/true);
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
            if (spliced)
                splice.append(sent, body.substr(0, have), 0, scan.safe(), body.size(), scan.model_at(),
                              scan.model_end());
            first = false;
        }
        else if (spliced)
            splice.append(sent, body.substr(0, have), out, scan.safe(), body.size(), scan.model_at(),
                          scan.model_end());
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
        if (all_keys) assert(!good);
        assert(sent.empty() || sent.size() - head_len < declared); // never a whole request
        return 0;
    }
    // The length is ours: the client's, and what the replacement adds.
    const std::string length = "\r\nContent-Length: " + std::to_string(declared) + "\r\n";
    assert(sent.substr(0, head_len).find(length) != std::string::npos);
    if (all_keys) assert(good);
    assert(ok);
    const std::string_view got = std::string_view(sent).substr(head_len);
    assert(got.size() == declared);
    const size_t at = scan.model_at(), end = scan.model_end();
    const bool hit = splice.matches(body, at, end);
    // The splice and the gateway's own model reader agree on whether the sentinel is there,
    // so a confirm() that sees model == sentinel sees a body that went substituted.
    assert(hit == (provider::model_of(body) == splice.sentinel));
    std::string want;
    if (hit)
    {
        want.assign(body.substr(0, at)).append(1, '"').append(splice.replacement).append(1, '"');
        // The held rewrite, but for the spaces a shorter replacement leaves after it.
        const std::string rewrite = provider::rewrite_model(body, splice.replacement);
        if (!rewrite.empty()) assert(rewrite == want + std::string(body.substr(end)));
        want.append(splice.shrink(), ' ').append(body.substr(end));
    }
    else want.assign(body).append(splice.grow(), ' ');
    assert(got == want);
    bool parsed = false;
    (void)provider::json::parse(got, parsed, provider::json::Keys::Any);
    assert(parsed);
    return 0;
}
