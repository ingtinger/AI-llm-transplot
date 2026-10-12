// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The byte-forward rebuild keeps its destination's allocation across requests.
// A request that grows a little each turn used to reallocate, and fault in, a fresh
// copy of itself every time; the fault run was 2.6 ms of a 3 ms request leg on the
// live tenant. These pin the capacity contract, and the content, on both sides.

#include "request.hpp"

#include "net/http.hpp"
#include "provider/json.hpp"

#include "gateway/gateway.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    std::string request_of(size_t body_bytes)
    {
        const std::string body(body_bytes, 'x');
        return "POST /v1/messages HTTP/1.1\r\nHost: client\r\nContent-Type: application/json\r\n"
               "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }
    size_t head_len(const std::string& msg) { return msg.find("\r\n\r\n") + 4; }
} // namespace

TEST(Rebuild, ASlightlyLargerRequestReusesTheBuffer)
{
    const std::vector<std::string> strip;
    std::string into;
    const std::string first = request_of(1'000'000);
    ASSERT_TRUE(llmbridge::detail::request_without(first, head_len(first), strip, "venue", "", {}, into));
    const char* where = into.data();
    const size_t cap = into.capacity();
    EXPECT_GE(cap, into.size() + into.size() / 8 - 200) << "headroom of an eighth is reserved";

    const std::string second = request_of(1'050'000); // 5% larger: inside the headroom
    ASSERT_TRUE(llmbridge::detail::request_without(second, head_len(second), strip, "venue", "", {}, into));
    EXPECT_EQ(into.data(), where) << "no reallocation for growth inside the headroom";
    EXPECT_EQ(into.capacity(), cap);
    EXPECT_EQ(into.substr(into.size() - 1'050'000), std::string(1'050'000, 'x')) << "the body is verbatim";
    EXPECT_NE(into.find("\r\nHost: venue\r\n"), std::string::npos) << "the venue's Host replaces the client's";
    EXPECT_EQ(into.find("Host: client"), std::string::npos);
    EXPECT_NE(into.find("Content-Length: 1050000\r\n"), std::string::npos) << "the length describes the body sent";
}

TEST(Rebuild, ARefusedRequestLeavesTheBufferEmpty)
{
    const std::vector<std::string> strip;
    std::string into;
    const std::string ok = request_of(64);
    ASSERT_TRUE(llmbridge::detail::request_without(ok, head_len(ok), strip, "venue", "", {}, into));
    ASSERT_FALSE(into.empty());
    // An absolute-form target cannot take a base path prefix, so the rebuild refuses.
    const std::string absolute = "POST http://elsewhere/v1 HTTP/1.1\r\nContent-Length: 2\r\n\r\nxx";
    EXPECT_FALSE(llmbridge::detail::request_without(absolute, head_len(absolute), strip, "venue", "/base", {}, into));
    EXPECT_TRUE(into.empty());
}

// The client's target is spliced after the venue's base path, so a dot segment, in
// plain or encoded form, would reach a path outside it; and with no base path an
// absolute-form target was forwarded verbatim, letting the client pick the vhost.
TEST(Rebuild, ATargetThatCouldLeaveTheBasePathIsRefused)
{
    const std::vector<std::string> strip;
    std::string into;
    const auto req = [](std::string target)
    { return "POST " + target + " HTTP/1.1\r\nContent-Length: 2\r\n\r\nxx"; };
    for (const char* bad : {"/../admin/keys", "/v1/chat/completions/../../admin", "/%2e%2e/admin",
                            "/.%2E/admin", "/v1/..%2fadmin", "/v1/x%5cy", "/v1\\..\\admin", "/v1/.",
                            "/v1/..", "http://elsewhere/v1"})
    {
        const std::string m = req(bad);
        EXPECT_FALSE(llmbridge::detail::request_without(m, head_len(m), strip, "venue", "/openai", {}, into))
            << bad << " under a base path";
        EXPECT_FALSE(llmbridge::detail::request_without(m, head_len(m), strip, "venue", "", {}, into))
            << bad << " without a base path";
    }
    for (const char* good : {"/v1/chat/completions", "/v1/models?after=a..b", "/v1/files/f.json",
                             "/v1/.well-known", "//v1/chat/completions", "/v1/x%41y"})
    {
        const std::string m = req(good);
        EXPECT_TRUE(llmbridge::detail::request_without(m, head_len(m), strip, "venue", "/openai", {}, into))
            << good;
    }
}

// The policy and dialect detection read the first line as the request line. If the
// header-strip rules ran over it, a first line shaped like a header vanished and the
// next line, unseen by the policy, became the upstream's request line.
TEST(Rebuild, TheRequestLineIsNeverStrippedAsAHeader)
{
    const std::vector<std::string> strip{"authorization"};
    std::string into;
    for (const char* decoy : {"Expect: /v1/chat/completions HTTP/1.1", "Host: /v1/chat/completions HTTP/1.1",
                              "Authorization: /v1/chat/completions HTTP/1.1"})
    {
        const std::string m = std::string(decoy) +
                              "\r\nDELETE /v1/files/x?a:b HTTP/1.1\r\nContent-Length: 2\r\n\r\nxx";
        if (!llmbridge::detail::request_without(m, head_len(m), strip, "venue", "", {}, into)) continue;
        EXPECT_EQ(into.rfind(std::string(decoy) + "\r\n", 0), 0u) << "upstream request line for " << decoy
                                                                     << ":\n" << into.substr(0, 80);
    }
}

// The three other producers of an upstream body get the same treatment: the
// buffer form must yield the bytes of the value form and keep its storage.

// The hop-by-hop headers, and any the client's Connection names, are this hop's: the
// venue never sees them, and the rest of the head survives in order.
TEST(Rebuild, HopByHopAndConnectionListedHeadersAreDropped)
{
    const std::vector<std::string> strip;
    std::string into;
    const std::string m = "POST /v1/chat/completions HTTP/1.1\r\nHost: client\r\nX-A: 1\r\n"
                          "Connection: close, X-Listed\r\nkeep-alive: timeout=5\r\nX-Listed: 2\r\n"
                          "TE: trailers\r\nUpgrade: h2c\r\nProxy-Connection: close\r\n"
                          "Trailer: X-Sum\r\nConnection: x-also\r\nX-Also: 3\r\nX-B: 4\r\n"
                          "Expect: 100-continue\r\nAccept-Encoding: gzip\r\n"
                          "Content-Length: 2\r\n\r\nxx";
    ASSERT_TRUE(llmbridge::detail::request_without(m, head_len(m), strip, "venue", "", {}, into));
    EXPECT_EQ(into, "POST /v1/chat/completions HTTP/1.1\r\nHost: venue\r\nContent-Length: 2\r\n"
                    "X-A: 1\r\nX-B: 4\r\n\r\nxx");
}

TEST(Rebuild, AnHttp10RequestLineGoesAsHttp11)
{
    const std::vector<std::string> strip;
    std::string into;
    const std::string m = "GET /v1/models HTTP/1.0\r\nX-A: 1\r\n\r\n";
    ASSERT_TRUE(llmbridge::detail::request_without(m, head_len(m), strip, "venue", "/base", {}, into));
    EXPECT_EQ(into, "GET /base/v1/models HTTP/1.1\r\nHost: venue\r\nX-A: 1\r\n\r\n");
}

// The Connection lines are read in the framing walk into a fixed table; a request with
// more than it holds is refused rather than forwarded with a listed header kept.
TEST(Rebuild, MoreConnectionLinesThanTheWalkHoldsAreRefused)
{
    const std::vector<std::string> strip;
    std::string into;
    std::string m = "POST /v1 HTTP/1.1\r\n";
    for (int i = 0; i < 5; ++i) m += "Connection: x-" + std::to_string(i) + "\r\n";
    m += "Content-Length: 0\r\n\r\n";
    EXPECT_FALSE(llmbridge::detail::request_without(m, head_len(m), strip, "venue", "", {}, into));
    EXPECT_TRUE(into.empty());
}

// A translated request with a model override was spliced (one parse) and then
// translated (a second parse). The body is parsed once now, and the override reaches
// the translator through RequestOut; the bytes are the same.
TEST(ParseOnce, AnOverriddenTranslatedRequestParsesTheBodyOnce)
{
    llmbridge::Upstream up{.ip = "127.0.0.1", .port = 1, .dialect = llmbridge::UpstreamDialect::Anthropic};
    up.host_hdr = "venue";
    std::string body = R"({"model":"gpt-4o","max_tokens":64,"messages":[)";
    for (int i = 0; i < 66; ++i)
        body += std::string(i ? "," : "") + R"({"role":"user","content":")" + std::string(2000, 'w') + "\"}";
    body += "]}";
    ASSERT_GT(body.size(), 128u * 1024);
    const std::string head = "POST /v1/chat/completions HTTP/1.1\r\nAuthorization: Bearer k\r\n\r\n";
    llmbridge::HeadFacts f;
    const std::vector<std::string> strip;
    ASSERT_TRUE(llmbridge::net::http::walk_headers(std::string_view(head).substr(0, head.size() - 4),
                                                   llmbridge::detail::HeadReader{head, strip, f}));
    std::string out, scratch;
    const char* why = "";
    const uint64_t before = llmbridge::provider::json::detail::parse_count();
    ASSERT_TRUE(llmbridge::detail::build_translated_request(up, up.dialect, body, head, f, out, scratch,
                                                            why, "claude-haiku-4-5"));
    EXPECT_EQ(llmbridge::provider::json::detail::parse_count() - before, 1u);
    const std::string expect = llmbridge::provider::openai_to_anthropic_request(
        llmbridge::provider::rewrite_model(body, "claude-haiku-4-5"));
    EXPECT_EQ(out.substr(out.size() - expect.size()), expect);
    EXPECT_NE(out.find("\r\nx-api-key: k\r\n"), std::string::npos);
}

static std::string big_openai(size_t turns)
{
    std::string b = R"({"model":"claude-sonnet-4-5","max_tokens":64,"stream":true,)"
                    R"("stream_options":{"include_usage":true},"messages":[)"
                    R"({"role":"system","content":"be brief"})";
    for (size_t i = 0; i < turns; ++i)
    {
        b += R"(,{"role":"user","content":")" + std::string(1000, 'u') + "\"}";
        b += R"(,{"role":"assistant","content":")" + std::string(1000, 'a') + "\"}";
    }
    return b + "]}";
}

TEST(Rebuild, AnthropicBufferFormMatchesValueForm)
{
    std::string buf;
    const std::string small = big_openai(20);
    bool want_v = false, want_b = false;
    ASSERT_TRUE(llmbridge::provider::openai_to_anthropic_request(small, buf, &want_b));
    EXPECT_EQ(buf, llmbridge::provider::openai_to_anthropic_request(small, &want_v));
    EXPECT_EQ(want_v, want_b);
    const char* before = buf.data();
    const size_t cap = buf.capacity();
    const std::string bigger = big_openai(21);
    ASSERT_TRUE(llmbridge::provider::openai_to_anthropic_request(bigger, buf));
    EXPECT_EQ(buf, llmbridge::provider::openai_to_anthropic_request(bigger));
    EXPECT_EQ(buf.data(), before);
    EXPECT_EQ(buf.capacity(), cap);
    EXPECT_FALSE(llmbridge::provider::openai_to_anthropic_request("not json", buf));
    EXPECT_TRUE(buf.empty());
}

TEST(Rebuild, BedrockBufferFormMatchesValueForm)
{
    std::string buf, model_b, model_v;
    const std::string body = big_openai(5);
    ASSERT_TRUE(llmbridge::provider::openai_to_bedrock_request(body, model_b, buf));
    EXPECT_EQ(buf, llmbridge::provider::openai_to_bedrock_request(body, model_v));
    EXPECT_EQ(model_b, model_v);
    EXPECT_EQ(model_b, "claude-sonnet-4-5");
}

TEST(Rebuild, OverridesBufferFormMatchesValueForm)
{
    std::string buf;
    const std::string body = big_openai(20);
    std::string_view had_b, had_v;
    ASSERT_TRUE(llmbridge::provider::apply_overrides(body, "claude-haiku-4-5", "auto", &had_b, buf));
    EXPECT_EQ(buf, llmbridge::provider::apply_overrides(body, "claude-haiku-4-5", "auto", &had_v));
    EXPECT_EQ(had_b, had_v);
    const char* before = buf.data();
    ASSERT_TRUE(llmbridge::provider::apply_overrides(big_openai(21), "claude-haiku-4-5", "", nullptr, buf));
    EXPECT_EQ(buf.data(), before);
    ASSERT_TRUE(llmbridge::provider::apply_overrides(body, "", "", nullptr, buf));
    EXPECT_EQ(buf, body);
    EXPECT_FALSE(llmbridge::provider::apply_overrides(body, "bad\"model", "", nullptr, buf));
}

TEST(Rebuild, HttpRequestFramingKeepsItsBuffer)
{
    std::string buf;
    llmbridge::detail::build_http_request("POST /v1/messages HTTP/1.1", "{}", "h",
                                           "x-api-key: k\r\n", buf);
    EXPECT_EQ(buf, "POST /v1/messages HTTP/1.1\r\nHost: h\r\nx-api-key: k\r\n"
                   "Content-Type: application/json\r\nConnection: keep-alive\r\n"
                   "Content-Length: 2\r\n\r\n{}");
    const char* before = buf.data();
    llmbridge::detail::build_http_request("POST /v1/messages HTTP/1.1", "{\"a\":1}", "h",
                                           "x-api-key: k\r\n", buf);
    EXPECT_EQ(buf.data(), before);
}

// The knob's whole point is that the pages are mapped before a request arrives:
// with it off, the reserved scratch is address space and nothing more.
TEST(Rebuild, PrefaultMapsTheScratchPages)
{
    const size_t want = 4u << 20;
    llmbridge::Gateway off(0, "127.0.0.1", 1);
    // An empty string points into its own object, whose inline buffer may straddle a
    // page boundary wherever the allocator placed it: two pages, never the 4 MiB.
    EXPECT_LE(off.prefault_resident_bytes_for_test(), 2 * 4096u);
    llmbridge::Gateway on(0, "127.0.0.1", 1);
    on.set_prefault_bytes(want);
    EXPECT_GE(on.prefault_resident_bytes_for_test(), want);
}

// A retired buffer keeps its pages for the next connection, and the counter says
// when a build had to grow the scratch. Both are the rotation the tape exposed.
TEST(Rebuild, ARetiredBufferIsHandedToTheNextConnection)
{
    llmbridge::Gateway gw(0, "127.0.0.1", 1);
    llmbridge::UpstreamConn dying;
    dying.out.stage().assign(1u << 20, 'k'); // a request, credential included, still in it
#ifdef LLMBRIDGE_HAVE_TLS
    dying.tls_out.stage().assign(1u << 20, 'c'); // and its ciphertext
    const char* cpages = dying.tls_out.bytes().data();
#endif
    const char* pages = dying.out.bytes().data();
    gw.retire_wbuf(&dying);
    EXPECT_EQ(gw.warm_bufs_for_test(), 1u);
    EXPECT_TRUE(dying.out.bytes().empty());
    llmbridge::UpstreamConn born;
    gw.adopt_warm(&born);
    EXPECT_EQ(gw.warm_bufs_for_test(), 0u);
    EXPECT_EQ(born.out.bytes().data(), pages);
#ifdef LLMBRIDGE_HAVE_TLS
    EXPECT_TRUE(dying.tls_out.bytes().empty());
    EXPECT_EQ(born.tls_out.bytes().data(), cpages);
    EXPECT_TRUE(born.tls_out.bytes().empty());
    EXPECT_EQ(std::count(born.tls_out.bytes().data(), born.tls_out.bytes().data() + (1u << 20), 'c'), 0)
        << "the ciphertext of a credential must not survive either";
#endif
    EXPECT_GE(born.out.capacity(), 1u << 20);
    EXPECT_TRUE(born.out.bytes().empty());
    EXPECT_EQ(std::count(born.out.bytes().data(), born.out.bytes().data() + (1u << 20), 'k'), 0)
        << "the credential must not survive into the next connection";
    EXPECT_EQ(gw.stats().warm_reuses, 1u);
    // Too small to matter, and a full list, are left alone. A client's buffer cannot be
    // offered at all: retire_wbuf takes an UpstreamConn.
    llmbridge::UpstreamConn small;
    small.out.stage().assign(1024, 'x');
    gw.retire_wbuf(&small);
    EXPECT_EQ(gw.warm_bufs_for_test(), 0u);
    for (size_t i = 0; i < llmbridge::Gateway::kWarmBufs + 2; ++i)
    {
        llmbridge::UpstreamConn d;
        d.out.stage().assign(1u << 17, 'x');
        gw.retire_wbuf(&d);
    }
    EXPECT_EQ(gw.warm_bufs_for_test(), llmbridge::Gateway::kWarmBufs) << "the list is capped";
}

TEST(Rebuild, AColdBuildIsCounted)
{
    llmbridge::Gateway gw(0, "127.0.0.1", 1);
    gw.note_build(1u << 20);
    EXPECT_EQ(gw.stats().cold_builds, 1u);
    gw.set_prefault_bytes(4u << 20);
    gw.prefault_resident_bytes_for_test();
    gw.note_build(1u << 20);
    EXPECT_EQ(gw.stats().cold_builds, 1u) << "a prefaulted scratch holds it without growing";
}
