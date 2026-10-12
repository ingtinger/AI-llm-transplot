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
    const std::string_view body(reinterpret_cast<const char*>(data + 1), size - 1);
    const std::string head = "POST /v1/chat/completions HTTP/1.1\r\nHost: client\r\nContent-Type: "
                             "application/json\r\nConnection: keep-alive\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n";
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
    scan.reset();
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
        if (first)
        {
            // The head goes with whatever is already known good.
            const bool ok_head = detail::build_forward(head, head.size(), f, strip, "venue", "",
                                                       body.substr(0, scan.safe()), sent, body.size()) == nullptr;
            assert(ok_head);
            first = false;
        }
        else sent.append(body.substr(out, scan.safe() - out));
        out = scan.safe();
    } while (have < body.size());

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
