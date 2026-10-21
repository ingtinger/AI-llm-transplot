// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// auth_headers_for's one-pass scan replaced six find_header() walks per request
// (request.cpp's scan_auth_headers), measured at ~40 us p99 on a real request.
// Raised on a public thread as the scary class of optimization: a later cleanup
// could silently reorder the walk, or drop the pool, without anything failing.
// The correctness half of the guard: order and duplicates must not change the
// result, and one golden fixture pins what the result is. The percentile harness
// lives separately, in request_perf_test.cpp, deliberately not run from here: a
// tight timing tolerance inside the PR-blocking suite is exactly the "noisy job
// blocks every PR" shape a reviewer warned against. What does belong here is the
// loose sanity bound at the end, an order of magnitude over measured, which only a
// pathological regression can trip.

#include "request.hpp"
#include "response.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace
{
    using llmbridge::UpstreamDialect;
    using llmbridge::HeadFacts;
    using llmbridge::detail::auth_headers_for;
    using llmbridge::detail::build_translated_request;
    using llmbridge::detail::HeadReader;
    using llmbridge::detail::named_key;
    using llmbridge::detail::NamedKey;
    using llmbridge::detail::Refusal;
    using llmbridge::detail::request_without;
    using llmbridge::detail::translate_failure;
    namespace refuse = llmbridge::refuse;

    constexpr std::string_view kNamed = "x-kottos-key-anthropic";

    /// Every key a client might send, the named one among them in another case.
    const std::string kManyKeys = "Authorization: Bearer sk-AUTH-WRONG\r\n"
                                  "x-api-key: sk-XAPI-WRONG\r\n"
                                  "x-goog-api-key: sk-GOOG-WRONG\r\n"
                                  "api-key: sk-AZURE-WRONG\r\n"
                                  "X-Kottos-Key-Anthropic: sk-RIGHT\r\n"
                                  "x-kottos-key-openai: sk-OTHER-DOMAIN-WRONG\r\n";

    std::string lowered(std::string s)
    {
        for (char& c : s) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        return s;
    }

    /// Header lines of a rebuilt head that carry any credential, by name.
    int credential_lines(const std::string& head)
    {
        int n = 0;
        for (size_t at = 0; (at = head.find("\r\n", at)) != std::string::npos; at += 2)
        {
            const std::string line = lowered(head.substr(at + 2, 32));
            for (const char* name : {"authorization:", "x-api-key:", "x-goog-api-key:", "api-key:"})
                n += line.rfind(name, 0) == 0;
        }
        return n;
    }

    /// `n` nameable family lines besides any the caller adds.
    std::string family_lines(int n)
    {
        std::string s;
        for (int i = 0; i < n; ++i) s += "x-kottos-key-k" + std::to_string(i) + ": v" + std::to_string(i) + "\r\n";
        return s;
    }

    std::string forward_request(const std::string& hdrs)
    {
        const std::string body = R"({"model":"m","messages":[]})";
        return "POST /v1/chat/completions HTTP/1.1\r\nHost: client\r\n" + hdrs +
               "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    }
    size_t head_len(const std::string& m) { return m.find("\r\n\r\n") + 4; }
} // namespace

// The reason logged and shown for a refused translation names the actual defect.
TEST(TranslateFailure, NamesARepeatedKeyAndNotWhitespaceAroundArguments)
{
    EXPECT_STREQ(translate_failure(R"({"model":"m","messages":[{"role":"user","content":"a",)"
                                   R"("content":"b"}]})"),
                 llmbridge::refuse::kRepeatedKey);
    EXPECT_STREQ(translate_failure(R"({"model":"m","messages":[)"), llmbridge::refuse::kNotJson);
    // Valid arguments with leading whitespace are one object; whatever failed, it was
    // not them.
    EXPECT_STREQ(translate_failure(
                     R"({"model":"m","messages":[{"role":"assistant","content":null,)"
                     R"("tool_calls":[{"id":"a","type":"function","function":{"name":"f",)"
                     R"("arguments":" {\"a\":1}"}}]}]})"),
                 llmbridge::refuse::kShape);
}

TEST(AuthHeaders, OrderDoesNotChangeTheResult)
{
    const std::vector<std::string> strip;
    const std::vector<std::string> lines = {
        "Authorization: Bearer sk-live-abc\r\n",
        "anthropic-version: 2024-01-01\r\n",
        "Host: client\r\n",
    };
    std::vector<size_t> idx = {0, 1, 2};
    std::string baseline;
    bool have_baseline = false;
    do
    {
        std::string headers;
        for (const size_t i : idx) headers += lines[i];
        std::string out;
        ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
        if (!have_baseline) { baseline = out; have_baseline = true; }
        else EXPECT_EQ(out, baseline) << "header order must not change the resolved credential";
    } while (std::next_permutation(idx.begin(), idx.end()));
}

TEST(AuthHeaders, FirstDuplicateWins)
{
    const std::vector<std::string> strip;
    const std::string headers =
        "x-api-key: first\r\n"
        "x-api-key: second\r\n"
        "anthropic-version: 2024-01-01\r\n";
    std::string out;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
    EXPECT_NE(out.find("x-api-key: first"), std::string::npos);
    EXPECT_EQ(out.find("second"), std::string::npos) << "the second x-api-key must never surface";
}

TEST(AuthHeaders, DuplicateAcrossCaseStillResolvesFirst)
{
    // Header names are matched case-insensitively; a duplicate spelled differently
    // is still the same header to the merged walk, not a second candidate.
    const std::vector<std::string> strip;
    const std::string headers =
        "X-API-KEY: first\r\n"
        "x-api-key: second\r\n"
        "anthropic-version: 2024-01-01\r\n";
    std::string out;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
    EXPECT_NE(out.find("x-api-key: first"), std::string::npos);
    EXPECT_EQ(out.find("second"), std::string::npos);
}

TEST(AuthHeaders, StrippedHeaderIsInvisible)
{
    const std::vector<std::string> strip = {"authorization:"};
    const std::string headers =
        "Authorization: Bearer tenant-token\r\n"
        "anthropic-version: 2024-01-01\r\n";
    std::string out;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
    EXPECT_EQ(out.find("x-api-key"), std::string::npos) << "a stripped Authorization yields no credential";
}

TEST(AuthHeaders, ControlByteInValueIsRefused)
{
    const std::vector<std::string> strip;
    const std::string headers = "x-api-key: sk\rX-Smuggled: yes\r\n";
    std::string out;
    EXPECT_FALSE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
}

// The golden fixture. The permutation test above proves every ordering agrees
// with every other; it never says what they agree on, so a cleanup that changed
// the emitted format for all of them at once (a lowercased name, a dropped CRLF,
// the two lines swapped) would pass it. This pins the exact bytes for the one
// request that exercises everything at once: a duplicated x-api-key in mixed
// case, a duplicated anthropic-version, a bearer that must lose to the api key,
// and unrelated headers interleaved throughout. Any semantic change to the walk
// fails this, and the failure names the byte that moved.
TEST(AuthHeaders, GoldenSerializedResult)
{
    const std::vector<std::string> strip;
    const std::string headers =
        "Host: client\r\n"
        "x-api-key: first-key\r\n"
        "Content-Type: application/json\r\n"
        "X-API-KEY: second-key\r\n"
        "anthropic-version: 2024-01-01\r\n"
        "Authorization: Bearer sk-bearer\r\n"
        "anthropic-version: 2025-01-01\r\n";
    std::string out;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
    EXPECT_EQ(out, "x-api-key: first-key\r\nanthropic-version: 2024-01-01\r\n");
}

// A sanity bound, not a benchmark. Measured on the reference host at ~135 ns
// p50 (request_perf_test.cpp); this passes anything under 2,000 ns, an order of
// magnitude of headroom, and averages over 200,000 calls so a context switch or
// two lands as noise instead of a spike. What it catches is the pathological
// shape: the six-walk version this scan replaced measured ~40 us at p99 on a
// live request, twenty times past this line. A tight tolerance would flake on a
// shared runner; this one cannot without something being genuinely wrong.
// Skipped under the sanitizers, which slow every path uniformly.
// The AzureOpenAI SDK sends `api-key`; it was not read at all.
TEST(AuthHeaders, AnAzureApiKeyIsACredential)
{
    const std::vector<std::string> strip;
    std::string out;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Azure, "api-key: AZKEY\r\n", strip, out));
    EXPECT_EQ(out, "api-key: AZKEY\r\n");
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Azure,
                                 "Authorization: Bearer B\r\napi-key: AZKEY\r\n", strip, out));
    EXPECT_EQ(out, "api-key: AZKEY\r\n") << "Azure's own header wins";
    EXPECT_FALSE(auth_headers_for(UpstreamDialect::Azure, "api-key: a\x01" "b\r\n", strip, out))
        << "validated with the other credentials";
    EXPECT_FALSE(auth_headers_for(UpstreamDialect::Anthropic, "api-key: a\x01" "b\r\n", strip, out))
        << "validated whether or not the venue reads it";
    EXPECT_FALSE(auth_headers_for(UpstreamDialect::Azure, "Host: x\r\n", strip, out))
        << "no credential at all is refused";
}

// RFC 9110 11.1: the scheme is case-insensitive.
TEST(AuthHeaders, TheBearerSchemeIsCaseInsensitive)
{
    const std::vector<std::string> strip;
    std::string out;
    for (const char* auth : {"Bearer k", "bearer k", "BEARER k", "bEaReR k", "Bearer   k"})
    {
        ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic,
                                     std::string("Authorization: ") + auth + "\r\n", strip, out));
        EXPECT_EQ(out, "x-api-key: k\r\nanthropic-version: 2023-06-01\r\n") << auth;
    }
    for (const char* auth : {"Bearerk", "Basic k", "Bearer"})
    {
        ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic,
                                     std::string("Authorization: ") + auth + "\r\n", strip, out));
        EXPECT_EQ(out, "anthropic-version: 2023-06-01\r\n") << auth;
    }
}

// A policy may refuse with any 4xx or 5xx; one the table did not list became a 502.
TEST(ErrorReply, AnyStatusKeepsItsOwnCode)
{
    for (const int code : {405, 409, 422, 451, 500, 501, 599})
    {
        std::string out;
        llmbridge::detail::append_error(out, code, "why");
        EXPECT_EQ(llmbridge::detail::status_of(out), code) << out;
        EXPECT_EQ(out.find("Bad Gateway"), std::string::npos) << out;
        EXPECT_NE(out.find(code < 500 ? "invalid_request_error" : "api_error"), std::string::npos) << out;
        EXPECT_EQ(out.find("why") != std::string::npos, code < 500) << out;
    }
    std::string out;
    llmbridge::detail::append_error(out, 422);
    EXPECT_EQ(out.substr(0, out.find("\r\n")), "HTTP/1.1 422 Unprocessable Content");
    out.clear();
    llmbridge::detail::append_error(out, 502);
    EXPECT_EQ(out.substr(0, out.find("\r\n")), "HTTP/1.1 502 Bad Gateway");
    EXPECT_NE(out.find("upstream_error"), std::string::npos) << out;
}

#if defined(__has_feature)
    #define LLMBRIDGE_TEST_UNDER_CLANG_SANITIZER \
        (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
#else
    #define LLMBRIDGE_TEST_UNDER_CLANG_SANITIZER 0
#endif
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !LLMBRIDGE_TEST_UNDER_CLANG_SANITIZER
TEST(AuthHeaders, StaysWithinAnOrderOfMagnitudeOfOneWalk)
{
    const std::vector<std::string> strip;
    const std::string headers =
        "Host: client\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer sk-live-0123456789abcdef\r\n"
        "anthropic-version: 2023-06-01\r\n"
        "X-Request-Id: abc-123\r\n"
        "Accept: application/json\r\n";
    std::string out;
    constexpr int kIters = 200'000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i)
        ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out));
    const auto t1 = std::chrono::steady_clock::now();
    const double ns_per_call = std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
    EXPECT_LT(ns_per_call, 2000.0) << "ns/call=" << ns_per_call;
}

// The same bound with a key named: the named path costs about what the unnamed one does.
// It cannot see a second walk; TheNamedKeyIsReadFromTheFramingWalkAlone does.
TEST(KeyHeader, ANamedKeyStaysWithinTheSameBound)
{
    const std::vector<std::string> strip;
    const std::string headers = "Host: client\r\nContent-Type: application/json\r\n" + kManyKeys +
                                "X-Request-Id: abc-123\r\nAccept: application/json\r\n";
    std::string out;
    constexpr int kIters = 200'000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i)
        ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, headers, strip, out, kNamed));
    const auto t1 = std::chrono::steady_clock::now();
    const double ns_per_call = std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters;
    EXPECT_LT(ns_per_call, 2000.0) << "ns/call=" << ns_per_call;
}
#endif

// ── The x-kottos-key- family ─────────────────────────────────────────────────
// A client may send one provider key per x-kottos-key-<domain> header. None of them
// is ever forwarded: the strip list matches exact names, and a client picks the suffix.

// A plain x-kottos-key is not in the family (no dash), nor is x-kottos-keyring, and the
// client's own credential is forwarded as before.
TEST(KeySwap, TheFamilyIsStrippedWhenNothingIsNamed)
{
    const std::vector<std::string> strip;
    const std::string m = forward_request("Authorization: Bearer sk-CLIENT\r\n"
                                          "x-kottos-key-anthropic: sk-F1\r\nX-KOTTOS-KEY-OPENAI: sk-F2\r\n"
                                          "x-kottos-key-: sk-F3\r\nx-kottos-keyring: kept\r\n"
                                          "x-kottos-key: tenant\r\n");
    std::string into;
    ASSERT_TRUE(request_without(m, head_len(m), strip, "venue", "", {}, into));
    EXPECT_EQ(into.find("sk-F"), std::string::npos) << into;
    EXPECT_NE(into.find("\r\nAuthorization: Bearer sk-CLIENT\r\n"), std::string::npos) << into;
    EXPECT_NE(into.find("\r\nx-kottos-keyring: kept\r\n"), std::string::npos) << into;
    EXPECT_NE(into.find("\r\nx-kottos-key: tenant\r\n"), std::string::npos) << into;
}

// The walk keeps only lines a policy could name, and the strip does not depend on it: a
// family line no policy could name (an empty suffix, other bytes, past 64) goes too.
TEST(KeySwap, AFamilyLineNoPolicyCouldNameIsStrippedToo)
{
    const std::vector<std::string> strip;
    const std::string m = forward_request("Authorization: Bearer sk-CLIENT\r\nx-kottos-key-: sk-F1\r\n"
                                          "x-kottos-key-a+b: sk-F2\r\nX-Kottos-Key-" + std::string(52, 'a') +
                                          ": sk-F3\r\n");
    std::string into;
    ASSERT_TRUE(request_without(m, head_len(m), strip, "venue", "", {}, into));
    EXPECT_EQ(into.find("sk-F"), std::string::npos) << into;
    EXPECT_NE(into.find("\r\nAuthorization: Bearer sk-CLIENT\r\n"), std::string::npos) << into;
}

// The golden fixture for a request without the family: the rebuild is the bytes it was
// before key headers existed.
TEST(KeySwap, WithoutTheFamilyTheRebuildIsUnchanged)
{
    const std::vector<std::string> strip;
    const std::string body = R"({"model":"m","messages":[]})";
    const std::string m = "POST /v1/chat/completions HTTP/1.1\r\nHost: client\r\n"
                          "Authorization: Bearer sk\r\nX-A: 1\r\nContent-Length: " +
                          std::to_string(body.size()) + "\r\n\r\n" + body;
    std::string into;
    ASSERT_TRUE(request_without(m, head_len(m), strip, "venue", "", {}, into));
    EXPECT_EQ(into, "POST /v1/chat/completions HTTP/1.1\r\nHost: venue\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\nAuthorization: Bearer sk\r\nX-A: 1\r\n\r\n" +
                        body);
}

// ── A key header a policy named ──────────────────────────────────────────────
// The policy names the family header holding the key for the venue it picked. That
// value, and only that value, reaches the venue, in the venue's own header; nothing
// stands in when it is missing.

TEST(KeyHeader, EachDialectGetsTheNamedKeyInItsOwnHeaderAndNothingElse)
{
    const std::vector<std::string> strip;
    const std::pair<UpstreamDialect, std::string_view> cases[] = {
        {UpstreamDialect::Anthropic, "x-api-key: sk-RIGHT\r\nanthropic-version: 2023-06-01\r\n"},
        {UpstreamDialect::Gemini, "x-goog-api-key: sk-RIGHT\r\n"},
        {UpstreamDialect::Cohere, "Authorization: Bearer sk-RIGHT\r\n"},
        {UpstreamDialect::Azure, "api-key: sk-RIGHT\r\n"},
    };
    for (const auto& [mode, want] : cases)
    {
        std::string out;
        Refusal r;
        ASSERT_TRUE(auth_headers_for(mode, kManyKeys, strip, out, kNamed, &r)) << r.why;
        EXPECT_EQ(out, want);
    }
}

TEST(KeyHeader, TheClientsAnthropicVersionStillWins)
{
    const std::vector<std::string> strip;
    std::string out;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, kManyKeys + "anthropic-version: 2024-10-22\r\n",
                                 strip, out, kNamed));
    EXPECT_EQ(out, "x-api-key: sk-RIGHT\r\nanthropic-version: 2024-10-22\r\n");
}

// The leak this guards: Authorization holds a key for another provider, and a missing
// named header must never let it through as this venue's key.
TEST(KeyHeader, AnAbsentOrEmptyHeaderIsRefusedAndNothingStandsIn)
{
    const std::vector<std::string> strip;
    for (const std::string hdrs : {"Authorization: Bearer sk-WRONG\r\nx-api-key: sk-WRONG2\r\n",
                                   "Authorization: Bearer sk-WRONG\r\nx-kottos-key-anthropic:   \r\n",
                                   "x-kottos-key-anthropic:\r\n"})
    {
        for (const UpstreamDialect mode : {UpstreamDialect::Anthropic, UpstreamDialect::Gemini,
                                           UpstreamDialect::Cohere, UpstreamDialect::Azure})
        {
            std::string out = "stale";
            Refusal r;
            EXPECT_FALSE(auth_headers_for(mode, hdrs, strip, out, kNamed, &r)) << hdrs;
            EXPECT_STREQ(r.detail, refuse::kKeyHeaderMissing) << hdrs;
            EXPECT_STREQ(r.why, llmbridge::detail::kWhyKeyMissing) << hdrs;
            EXPECT_TRUE(out.empty()) << out;
        }
    }
}

TEST(KeyHeader, ARepeatedHeaderIsRefusedWhicheverCopyIsFirst)
{
    const std::vector<std::string> strip;
    for (const std::string hdrs : {"x-kottos-key-anthropic: a\r\nX-KOTTOS-KEY-ANTHROPIC: b\r\n",
                                   "x-kottos-key-anthropic: a\r\nx-kottos-key-anthropic: a\r\n",
                                   "x-kottos-key-anthropic:\r\nx-kottos-key-anthropic: b\r\n"})
    {
        std::string out;
        Refusal r;
        EXPECT_FALSE(auth_headers_for(UpstreamDialect::Anthropic, hdrs, strip, out, kNamed, &r));
        EXPECT_STREQ(r.detail, refuse::kKeyHeaderRepeated) << hdrs;
        EXPECT_TRUE(out.empty()) << out;
    }
}

TEST(KeyHeader, UnsafeBytesAreRefused)
{
    const std::vector<std::string> strip;
    for (const std::string& value : {std::string("sk\x01x"), std::string("sk\x7fx"), std::string("sk\xC3x"),
                                     std::string(9000, 'k')})
    {
        std::string out;
        Refusal r;
        EXPECT_FALSE(auth_headers_for(UpstreamDialect::Anthropic, "x-kottos-key-anthropic: " + value + "\r\n",
                                      strip, out, kNamed, &r));
        EXPECT_STREQ(r.detail, refuse::kCredential);
        EXPECT_TRUE(out.empty()) << out;
    }
    // A bare CR splits the head differently for another parser: the framing walk refuses it.
    std::string out;
    Refusal r;
    EXPECT_FALSE(auth_headers_for(UpstreamDialect::Anthropic, "x-kottos-key-anthropic: sk\rX-Injected: 1\r\n",
                                  strip, out, kNamed, &r));
    EXPECT_STREQ(r.why, "request framing");
    EXPECT_TRUE(out.empty()) << out;
}

TEST(KeyHeader, ASignedVenueCannotTakeOne)
{
    const std::vector<std::string> strip;
    std::string out;
    Refusal r;
    EXPECT_FALSE(auth_headers_for(UpstreamDialect::Bedrock, kManyKeys, strip, out, kNamed, &r));
    EXPECT_STREQ(r.detail, refuse::kKeyHeaderUnusable);
    EXPECT_TRUE(out.empty());
}

// SigV4 signs with the key pair in Authorization; beside a named key it is never signed,
// even where the request builder is reached without the check before it.
TEST(KeyHeader, ASignedVenueIsNeverSignedBesideANamedKey)
{
    llmbridge::Upstream up{.ip = "127.0.0.1", .port = 1, .sni_host = "bedrock-runtime.us-east-1.amazonaws.com",
                           .dialect = UpstreamDialect::Bedrock};
    up.host_hdr = up.sni_host;
    up.aws_region = "us-east-1";
    const std::string head = "POST /v1/chat/completions HTTP/1.1\r\nAuthorization: Bearer AKIDEXAMPLE:SECRET\r\n\r\n";
    const std::vector<std::string> strip;
    HeadFacts f;
    ASSERT_TRUE(llmbridge::net::http::walk_headers(std::string_view(head).substr(0, head.size() - 4),
                                                   HeadReader{head, strip, f}));
    const std::string body = R"({"model":"m","max_tokens":8,"messages":[{"role":"user","content":"hi"}]})";
    std::string out, scratch;
    const char* why = "";
#ifdef LLMBRIDGE_HAVE_TLS
    ASSERT_TRUE(build_translated_request(up, up.dialect, body, head, f, out, scratch, why)) << why;
    EXPECT_NE(out.find("AWS4-HMAC-SHA256"), std::string::npos) << "the control is signed";
#endif
    const NamedKey key{"x-api-key: ", "sk-NAMED"};
    EXPECT_FALSE(build_translated_request(up, up.dialect, body, head, f, out, scratch, why, {}, nullptr, key));
    EXPECT_STREQ(why, "credential");
    EXPECT_TRUE(out.empty()) << out;
}

// Unnamed, a family header is never a credential: only the four names above are read.
TEST(KeyHeader, TheFamilyIsNeverReadAsACredentialWhenNothingIsNamed)
{
    const std::vector<std::string> strip;
    const std::string hdrs = "x-kottos-key-anthropic: sk-FAMILY\r\nX-Kottos-Key-Openai: sk-FAMILY2\r\n";
    std::string out;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, hdrs, strip, out));
    EXPECT_EQ(out, "anthropic-version: 2023-06-01\r\n");
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Gemini, hdrs, strip, out));
    EXPECT_EQ(out, "");
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Cohere, hdrs, strip, out));
    EXPECT_EQ(out, "");
    EXPECT_FALSE(auth_headers_for(UpstreamDialect::Azure, hdrs, strip, out));
}

TEST(KeyHeader, TheNameMatchesExactlyNotAsAPrefix)
{
    const std::vector<std::string> strip;
    std::string out;
    Refusal r;
    EXPECT_FALSE(auth_headers_for(UpstreamDialect::Anthropic, "x-kottos-key-ab: sk-LONGER\r\n", strip, out,
                                  "x-kottos-key-a", &r));
    EXPECT_STREQ(r.detail, refuse::kKeyHeaderMissing);
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic,
                                 "x-kottos-key-ab: sk-LONGER\r\nx-kottos-key-a: sk-RIGHT\r\n", strip, out,
                                 "x-kottos-key-a"));
    EXPECT_EQ(out, "x-api-key: sk-RIGHT\r\nanthropic-version: 2023-06-01\r\n");
}

// The strip list hides a header from the credential read, but the family never leaves
// anyway, so a strip entry for the named header does not hide it.
TEST(KeyHeader, AStrippedKeyHeaderIsStillRead)
{
    const std::vector<std::string> strip = {"x-kottos-key-anthropic"};
    std::string out;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, kManyKeys, strip, out, kNamed));
    EXPECT_EQ(out, "x-api-key: sk-RIGHT\r\nanthropic-version: 2023-06-01\r\n");
    const std::string m = forward_request(kManyKeys);
    std::string into;
    ASSERT_TRUE(request_without(m, head_len(m), strip, "venue", "", {}, into, kNamed, UpstreamDialect::OpenAI));
    EXPECT_NE(into.find("\r\nAuthorization: Bearer sk-RIGHT\r\n"), std::string::npos) << into;
}

// The walk records at most kKeyHeaders family lines. Past that a named key is refused,
// since a repeat could be among the lines not kept; a request naming none is unaffected.
// Lines that can never be named are not counted.
TEST(KeyHeader, PastTheWalksTableANamedKeyIsRefused)
{
    static_assert(HeadFacts::kKeyHeaders == 8);
    const std::vector<std::string> strip;
    const std::string named = "x-kottos-key-anthropic: sk-RIGHT\r\nAuthorization: Bearer sk-AUTH\r\n";
    const std::string unnameable = "x-kottos-key-: v\r\nx-kottos-key-" + std::string(52, 'n') + ": v\r\n";
    std::string out;
    Refusal r;
    ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, family_lines(7) + unnameable + named, strip, out,
                                 kNamed, &r)) << r.why;
    EXPECT_EQ(out, "x-api-key: sk-RIGHT\r\nanthropic-version: 2023-06-01\r\n");
    for (const std::string& hdrs : {family_lines(8) + named, named + family_lines(8)})
    {
        EXPECT_FALSE(auth_headers_for(UpstreamDialect::Anthropic, hdrs, strip, out, kNamed, &r));
        EXPECT_STREQ(r.detail, refuse::kKeyHeaderTooMany);
        EXPECT_STREQ(r.why, llmbridge::detail::kWhyKeyTooMany);
        ASSERT_TRUE(auth_headers_for(UpstreamDialect::Anthropic, hdrs, strip, out)) << "nothing named";
        EXPECT_EQ(out, "x-api-key: sk-AUTH\r\nanthropic-version: 2023-06-01\r\n");
        const std::string m = forward_request(hdrs + family_lines(20));
        std::string into;
        EXPECT_FALSE(request_without(m, head_len(m), strip, "venue", "", {}, into, kNamed,
                                     UpstreamDialect::OpenAI, &r));
        EXPECT_STREQ(r.detail, refuse::kKeyHeaderTooMany);
        ASSERT_TRUE(request_without(m, head_len(m), strip, "venue", "", {}, into));
        EXPECT_EQ(lowered(into).find("x-kottos-key"), std::string::npos) << into;
    }
}

// The named key comes from the spans the framing walk recorded and nothing else: with
// every other byte of the head poisoned, it still resolves, so nothing walks the head again.
TEST(KeyHeader, TheNamedKeyIsReadFromTheFramingWalkAlone)
{
    const std::vector<std::string> strip;
    std::string head = "POST /v1/chat/completions HTTP/1.1\r\nHost: client\r\n"
                       "Content-Type: application/json\r\n" + kManyKeys + "X-Other: y\r\n\r\n";
    HeadFacts f;
    ASSERT_TRUE(llmbridge::net::http::walk_headers(std::string_view(head).substr(0, head.size() - 4),
                                                   HeadReader{head, strip, f}));
    std::vector<bool> keep(head.size(), false);
    const auto mark = [&keep](const HeadFacts::Span& s) {
        for (uint32_t i = 0; i < s.len; ++i) keep[s.at + i] = true;
    };
    for (const HeadFacts::Span* s : {&f.authorization, &f.x_api_key, &f.api_key, &f.x_goog_api_key,
                                     &f.anthropic_version, &f.content_type})
        mark(*s);
    for (uint8_t i = 0; i < f.key_headers; ++i)
    {
        mark(f.key_name[i]);
        mark(f.key_value[i]);
    }
    for (size_t i = 0; i < head.size(); ++i)
        if (!keep[i]) head[i] = '\x01';
    NamedKey key;
    const Refusal r = named_key(head, f, kNamed, UpstreamDialect::Anthropic, key);
    ASSERT_EQ(r.why, nullptr) << r.why;
    EXPECT_EQ(key.value, "sk-RIGHT");
    llmbridge::Upstream up{.ip = "127.0.0.1", .port = 1, .dialect = UpstreamDialect::Anthropic};
    up.host_hdr = "venue";
    std::string out, scratch;
    const char* why = "";
    ASSERT_TRUE(build_translated_request(up, up.dialect,
                                         R"({"model":"m","max_tokens":8,"messages":[{"role":"user","content":"hi"}]})",
                                         head, f, out, scratch, why, {}, nullptr, key)) << why;
    EXPECT_NE(out.find("\r\nx-api-key: sk-RIGHT\r\nanthropic-version: 2023-06-01\r\n"), std::string::npos) << out;
    EXPECT_EQ(out.find("WRONG"), std::string::npos) << out;
}

TEST(KeySwap, AByteForwardCarriesOnlyTheNamedKeyInTheVenuesHeader)
{
    const std::vector<std::string> strip;
    const std::pair<UpstreamDialect, std::string_view> cases[] = {
        {UpstreamDialect::OpenAI, "\r\nAuthorization: Bearer sk-RIGHT\r\n"},
        {UpstreamDialect::Anthropic, "\r\nx-api-key: sk-RIGHT\r\n"},
        {UpstreamDialect::Gemini, "\r\nx-goog-api-key: sk-RIGHT\r\n"},
        {UpstreamDialect::Cohere, "\r\nAuthorization: Bearer sk-RIGHT\r\n"},
    };
    const std::string m = forward_request(kManyKeys + "X-Keep: kept\r\n");
    for (const auto& [venue, want] : cases)
    {
        std::string into;
        Refusal r;
        ASSERT_TRUE(request_without(m, head_len(m), strip, "venue", "", {}, into, kNamed, venue, &r)) << r.why;
        const std::string head = into.substr(0, head_len(into));
        EXPECT_NE(head.find(want), std::string::npos) << head;
        EXPECT_EQ(credential_lines(head), 1) << head;
        EXPECT_EQ(into.find("WRONG"), std::string::npos) << into;
        EXPECT_EQ(lowered(head).find("x-kottos-key"), std::string::npos) << head;
        EXPECT_NE(head.find("\r\nX-Keep: kept\r\n"), std::string::npos) << head;
        EXPECT_EQ(into.substr(head_len(into)), m.substr(head_len(m))) << "the body is verbatim";
        EXPECT_NE(head.find("\r\nContent-Length: " + std::to_string(m.size() - head_len(m)) + "\r\n"),
                  std::string::npos)
            << head;
    }
}

TEST(KeySwap, EveryRefusalLeavesTheBufferEmpty)
{
    const std::vector<std::string> strip;
    const std::pair<std::string, const char*> cases[] = {
        {"Authorization: Bearer sk-WRONG\r\n", refuse::kKeyHeaderMissing},
        {"x-kottos-key-anthropic:  \r\n", refuse::kKeyHeaderMissing},
        {"x-kottos-key-anthropic: a\r\nx-Kottos-Key-Anthropic: b\r\n", refuse::kKeyHeaderRepeated},
        {"x-kottos-key-anthropic: sk\x01x\r\n", refuse::kCredential},
        {"x-kottos-key-anthropic: sk\xC3x\r\n", refuse::kCredential},
        {family_lines(8) + "x-kottos-key-anthropic: sk\r\n", refuse::kKeyHeaderTooMany},
    };
    for (const auto& [hdrs, want] : cases)
    {
        const std::string m = forward_request(hdrs);
        std::string into = "stale";
        Refusal r;
        EXPECT_FALSE(request_without(m, head_len(m), strip, "venue", "", {}, into, kNamed,
                                     UpstreamDialect::OpenAI, &r));
        EXPECT_STREQ(r.detail, want) << hdrs;
        EXPECT_TRUE(into.empty()) << into;
    }
    const std::string m = forward_request(kManyKeys);
    std::string into = "stale";
    Refusal r;
    EXPECT_FALSE(request_without(m, head_len(m), strip, "venue", "", {}, into, kNamed,
                                 UpstreamDialect::Bedrock, &r));
    EXPECT_STREQ(r.detail, refuse::kKeyHeaderUnusable);
    EXPECT_TRUE(into.empty());
}
