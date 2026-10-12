// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Per-call cost of the protocol work on the request path, outside the event loop:
// a realistic provider response head, a translated response, one translated SSE
// event, and a large agent request. The bench mocks use a 4-line head and tiny
// bodies, which hide these costs; this is the baseline the refactor is held to.
// Not run by ctest (timings jitter on shared runners). Run it on a quiet machine
// from a Release build and compare percentiles against a previous run.

#include "large_bodies.hpp"
#include "net/http.hpp"
#include "provider/body_scan.hpp"
#include "provider/dialect.hpp"
#include "provider/json.hpp"
#include "provider/sse.hpp"
#include "provider/translate.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// Allocations, counted for the large-body section; one increment per new. TSan's
// runtime defines operator new itself, so under TSan nothing is counted.
#if defined(__SANITIZE_THREAD__)
    #define LLMBRIDGE_BENCH_UNDER_TSAN 1
#elif defined(__has_feature)
    #if __has_feature(thread_sanitizer)
        #define LLMBRIDGE_BENCH_UNDER_TSAN 1
    #endif
#endif
#ifndef LLMBRIDGE_BENCH_UNDER_TSAN
    #define LLMBRIDGE_BENCH_UNDER_TSAN 0
#endif
static size_t g_news = 0;
#if !LLMBRIDGE_BENCH_UNDER_TSAN
[[gnu::noinline]] static void* counted(std::size_t n)
{
    ++g_news;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
[[gnu::noinline]] static void release(void* p) noexcept { std::free(p); }
void* operator new(std::size_t n) { return counted(n); }
void* operator new[](std::size_t n) { return counted(n); }
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
#endif

namespace
{
    namespace http = llmbridge::net::http;
    namespace provider = llmbridge::provider;

    // Batches amortise the ~16 ns clock read; enough batches keep the tail visible.
    template <class F>
    void measure(const char* name, int batch, int samples, F&& call)
    {
        for (int i = 0; i < batch * 100; ++i) call();
        std::vector<double> ns(static_cast<size_t>(samples));
        for (auto& v : ns)
        {
            const auto t0 = std::chrono::steady_clock::now();
            for (int j = 0; j < batch; ++j) call();
            const auto t1 = std::chrono::steady_clock::now();
            v = std::chrono::duration<double, std::nano>(t1 - t0).count() / batch;
        }
        std::sort(ns.begin(), ns.end());
        const auto at = [&](double p) { return ns[static_cast<size_t>(p / 100 * (ns.size() - 1))]; };
        std::printf("%-38s p50 %10.1f ns   p95 %10.1f ns   p99 %10.1f ns\n", name, at(50), at(95), at(99));
    }

    // Shaped like a real Anthropic response head: 26 lines, ~1 KB, rate-limit block.
    std::string anthropic_head()
    {
        std::string h = "HTTP/1.1 200 OK\r\nDate: Thu, 09 Oct 2026 12:00:00 GMT\r\n"
                        "Content-Type: application/json\r\nContent-Length: 412\r\n"
                        "Connection: keep-alive\r\n";
        for (const char* k : {"requests", "input-tokens", "output-tokens", "tokens"})
            for (const char* f : {"limit", "remaining", "reset"})
                h += std::string("anthropic-ratelimit-") + k + "-" + f +
                     (f[0] == 'r' && f[2] == 's' ? ": 2026-10-09T12:00:30Z\r\n" : ": 400000\r\n");
        h += "request-id: req_011CTESTREQUESTIDENTIFIER0001\r\n"
             "anthropic-organization-id: 00000000-0000-0000-0000-000000000000\r\n"
             "via: 1.1 google\r\ncf-cache-status: DYNAMIC\r\n"
             "X-Robots-Tag: none\r\nServer: cloudflare\r\n"
             "CF-RAY: 8c0123456789abcd-IAD\r\n\r\n";
        return h;
    }

    constexpr std::string_view kAnthropicReply =
        R"({"id":"msg_011CTESTMESSAGEID","type":"message","role":"assistant",)"
        R"("model":"claude-opus-5-5","content":[{"type":"text","text":"The capital of France )"
        R"(is Paris. It has been the capital since the 10th century and is the country's )"
        R"(largest city."}],"stop_reason":"end_turn","stop_sequence":null,)"
        R"("usage":{"input_tokens":25,"cache_creation_input_tokens":0,)"
        R"("cache_read_input_tokens":0,"output_tokens":31}})";

    constexpr std::string_view kSseStart =
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\","
        "\"model\":\"claude-opus-5-5\",\"usage\":{\"input_tokens\":25,\"output_tokens\":1}}}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n";
    constexpr std::string_view kSseDelta =
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\" capital\"}}\n\n";

    // An agent turn: system prompt, tools, and a long history, about 130 KB.
    std::string agent_request()
    {
        std::string b = R"({"model":"claude-opus-5-5","max_tokens":4096,"messages":[)"
                        R"({"role":"system","content":"You are a coding agent."})";
        const std::string text(1000, 'a');
        for (int i = 0; b.size() < 130'000; ++i)
            b += std::string(R"(,{"role":")") + (i % 2 ? "assistant" : "user") +
                 R"(","content":")" + text + R"("})";
        b += "]}";
        return b;
    }

    // The same size with what an agent loop carries: 20 tool schemas, and a history of
    // tool calls whose arguments are escaped JSON, each answered by a tool result.
    std::string tool_agent_request()
    {
        std::string b = R"({"model":"claude-opus-5-5","max_tokens":4096,"tools":[)";
        for (int t = 0; t < 20; ++t)
            b += std::string(t ? "," : "") + R"({"type":"function","function":{"name":"tool_)" +
                 std::to_string(t) + R"(","parameters":{"type":"object","properties":{"path":)" +
                 R"({"type":"string","description":")" + std::string(400, 'd') +
                 R"("},"n":{"type":"integer"}},"required":["path"]}}})";
        b += R"(],"messages":[{"role":"system","content":"You are a coding agent."})";
        for (int i = 0; b.size() < 130'000; ++i)
        {
            const std::string id = "call_" + std::to_string(i);
            b += R"(,{"role":"assistant","content":null,"tool_calls":[{"id":")" + id +
                 R"(","type":"function","function":{"name":"tool_1","arguments":)" +
                 R"("{\"path\":\"src/a/b/file.cpp\",\"text\":\")" + std::string(300, 'x') +
                 R"(\"}"}}]},{"role":"tool","tool_call_id":")" + id + R"(","content":")" +
                 std::string(200, 'r') + R"("})";
        }
        b += "]}";
        return b;
    }
    // Wall time per call in milliseconds, p50 and p99 over `samples` calls.
    template <class F>
    void measure_ms(const std::string& name, int samples, F&& call)
    {
        call();
        std::vector<double> ms(static_cast<size_t>(samples));
        for (auto& v : ms)
        {
            const auto t0 = std::chrono::steady_clock::now();
            call();
            v = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        std::sort(ms.begin(), ms.end());
        std::printf("%-44s p50 %9.3f ms   p99 %9.3f ms\n", name.c_str(), ms[ms.size() / 2],
                    ms[static_cast<size_t>(0.99 * static_cast<double>(ms.size() - 1))]);
    }

    size_t count_nodes(const provider::json::Value& v)
    {
        size_t n = 1;
        if (v.is_array())
            for (const auto& e : v.arr) n += count_nodes(e);
        else if (v.is_object())
            for (const auto& m : v.obj) n += count_nodes(m.value);
        return n;
    }

    // 1, 4 and 10 MB bodies in the realistic shapes: framing, the top-level scan, the
    // streaming scan, translation with and without a model override, and what each
    // costs in allocations and JSON values.
    void large_bodies()
    {
        using namespace llmbridge::bench;
        std::string out;
        for (const size_t mb : {1, 4, 10})
            for (const Shape shape : {Shape::Transcript, Shape::Schemas, Shape::Images})
            {
                const std::string body = llmbridge::bench::body(shape, mb << 20);
                const std::string tag = std::string(shape_name(shape)) + " " + std::to_string(mb) + "MB";
                const int n = mb == 10 ? 11 : 21;
                const std::string msg = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Type: "
                                        "application/json\r\nContent-Length: " +
                                        std::to_string(body.size()) + "\r\n\r\n" + body;
                measure_ms("L frame " + tag, n, [&] {
                    http::Message m;
                    if (http::parse_request(msg, m) != http::FrameStatus::Complete) std::abort();
                });
                measure_ms("L top_level_facts " + tag, n, [&] {
                    if (provider::top_level_facts(body).keys != provider::KeyCheck::Ok) std::abort();
                });
                provider::BodyScan scan;
                measure_ms("L BodyScan (64 KB reads) " + tag, n, [&] {
                    scan.reset();
                    for (size_t have = 0; have < body.size();)
                    {
                        have = std::min(body.size(), have + (64u << 10));
                        if (scan.feed(std::string_view(body).substr(0, have), have == body.size()) ==
                            provider::BodyScan::Verdict::Refused)
                            std::abort();
                    }
                });
                bool ok = false;
                {
                    const provider::json::Value v = provider::json::parse(body, ok);
                    if (!ok) std::abort();
                    std::printf("L nodes %-36s %zu JSON values (kMaxNodes %zu), %zu bytes\n", tag.c_str(),
                                count_nodes(v), provider::json::kMaxNodes, body.size());
                }
                if (shape == Shape::Images) continue; // refused by the translator: vision
                measure_ms("L translate " + tag, n, [&] {
                    if (!provider::openai_to_anthropic_request(body, out)) std::abort();
                });
                measure_ms("L translate+override, one parse " + tag, n, [&] {
                    const provider::json::Value v = provider::parse_request_body(body, ok); // as the gateway does
                    provider::RequestOut o{out, body, "claude-haiku-4-5"};
                    if (!ok || !provider::spec(provider::Venue::Anthropic).request(v, o)) std::abort();
                });
                measure_ms("L translate+override, splice " + tag, n, [&] {
                    if (!provider::openai_to_anthropic_request(provider::rewrite_model(body, "claude-haiku-4-5"), out))
                        std::abort();
                });
                for (const size_t retain : {size_t{4} << 20, size_t{16} << 20})
                {
                    if (LLMBRIDGE_BENCH_UNDER_TSAN) break;
                    provider::json::set_retain_bytes(retain);
                    for (int w = 0; w < 3; ++w) (void)provider::openai_to_anthropic_request(body, out);
                    const size_t before = g_news;
                    (void)provider::openai_to_anthropic_request(body, out);
                    std::printf("L allocs translate %-26s %zu (retain %zu MB)\n", tag.c_str(), g_news - before,
                                retain >> 20);
                }
                provider::json::set_retain_bytes(size_t{4} << 20);
            }
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--large") == 0)
    {
        large_bodies();
        return 0;
    }
    const std::string head = anthropic_head();
    measure("parse_response_head (~1 KB head)", 64, 100'000, [&] {
        http::ResponseHead h;
        if (http::parse_response_head(head, h) != http::FrameStatus::Complete) std::abort();
    });

    measure("anthropic_to_openai_response", 16, 50'000, [&] {
        if (provider::anthropic_to_openai_response(kAnthropicReply).empty()) std::abort();
    });

    provider::AnthropicToOpenAiSse sse(1'760'000'000);
    std::string out;
    if (!sse.feed(kSseStart, out)) std::abort();
    measure("AnthropicToOpenAiSse::feed (1 event)", 64, 100'000, [&] {
        out.clear();
        if (!sse.feed(kSseDelta, out)) std::abort();
    });

    const std::string agent = agent_request();
    std::string translated;
    measure("openai_to_anthropic_request (130 KB)", 1, 2'000, [&] {
        if (!provider::openai_to_anthropic_request(agent, translated)) std::abort();
    });
    const std::string tools = tool_agent_request();
    measure("openai_to_anthropic_request (tools)", 1, 2'000, [&] {
        if (!provider::openai_to_anthropic_request(tools, translated)) std::abort();
    });
    std::printf("(agent body %zu bytes, tool agent %zu; head %zu bytes)\n", agent.size(),
                tools.size(), head.size());
    return 0;
}
