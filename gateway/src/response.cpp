// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "response.hpp"

#include <string>

#include "gateway/metrics.hpp" // wall_ns
#include "net/http_write.hpp"
#include "scan.hpp"

namespace llmbridge::detail
{
    namespace
    {
        /// Error type and client-facing message for a status. Generic per status, never
        /// the rule that refused: the client gets the status, the reason goes to the log.
        /// Any other 4xx or 5xx keeps its own code, typed by its class.
        struct ErrorShape
        {
            int code;
            const char* type;
            const char* msg;
        };
        constexpr ErrorShape kErrorShapes[] = {
            {400, "invalid_request_error", "malformed request"},
            {401, "authentication_error", "unauthorized"},
            {402, "billing_error", "payment required"},
            {403, "permission_error", "forbidden"},
            {404, "invalid_request_error", "not found"},
            {413, "invalid_request_error", "request too large"},
            {415, "invalid_request_error", "compressed request body is not supported"},
            {429, "rate_limit_error", "rate limit exceeded"},
            {500, "api_error", "internal error"},
            {502, "upstream_error", "bad gateway: upstream failure"},
            {503, "service_error", "service unavailable"},
            {504, "timeout_error", "upstream timed out"},
        };

        ErrorShape error_shape(int code) noexcept
        {
            for (const ErrorShape& s : kErrorShapes)
                if (s.code == code) return s;
            if (code < 500) return {code, "invalid_request_error", "request refused"};
            return {code, "api_error", "server error"};
        }
    } // namespace

    // The opt-in timing headers (--timing-headers). LATENCY.md section 3 is normative:
    // what each one spans, why connect is apart from the gateway's compute, and why a
    // stream reports ttfb instead of t4. Durations and one timestamp, never content.
    void append_timing_headers(std::string& out, int64_t t0, int64_t gateway_us,
                               int64_t connect_us, int64_t upwrite_us,
                               int64_t upstream_us, const char* upstream_key, uint64_t seq,
                               int64_t client_upload_us)
    {
        const auto add = [&out](const char* k, int64_t v) {
            if (v < 0) return; // a stamp we never took; omit instead of lie
            out.append(k);
            out.append(": ");
            out.append(std::to_string(v));
            out.append("\r\n");
        };
        add("x-llmbridge-t0", wall_ns(t0));
        add("x-llmbridge-seq", static_cast<int64_t>(seq));
        add("x-llmbridge-gateway-us", gateway_us);
        add("x-llmbridge-connect-us", connect_us); // the connect(TLS) histogram's span
        add("x-llmbridge-upwrite-us", upwrite_us);
        // The client's own upload, first byte to last; not part of `added`.
        add("x-llmbridge-client-upload-us", client_upload_us);
        add(upstream_key, upstream_us);
    }

    // Token counts, non-streaming only: a stream's counts are end-of-stream facts, and
    // a streaming client reads them from the final chunk (`include_usage`).
    void append_usage_headers(std::string& out, const BodyUsage& u)
    {
        if (u.in >= 0)
        {
            out.append("x-llmbridge-tokens-in: ");
            out.append(std::to_string(u.in));
            out.append("\r\n");
        }
        if (u.out >= 0)
        {
            out.append("x-llmbridge-tokens-out: ");
            out.append(std::to_string(u.out));
            out.append("\r\n");
        }
    }

    // `detail` is shown only for a 4xx, the caller's own mistake; a 5xx's reason stays
    // in the log. Every detail is a literal in this tree or a policy message that
    // passed deny_message_ok, so nothing client-supplied is echoed back.
    void append_error(std::string& out, int code, const char* detail)
    {
        const ErrorShape s = error_shape(code);
        std::string body;
        provider::openai::write_error(body, (detail && code < 500) ? detail : s.msg, s.type);
        out.reserve(out.size() + body.size() + 128);
        net::http::append_status_line(out, code);
        out.append("Content-Type: application/json\r\nConnection: close\r\n");
        net::http::append_content_length(out, body.size());
        out.append("\r\n");
        out.append(body);
    }

    // A provider's own failure (429, 529, a 400 for context length) keeps its status,
    // so a client can decide to back off, and the headers that say how long.
    void append_relayed_reply(std::string& out, int status, std::string_view upstream_head,
                              std::string_view body, bool own_body)
    {
        net::http::append_status_line(out, status);
        if (!own_body) out.append("Content-Type: application/json\r\n");
        if (!net::http::append_relayed(out, upstream_head, own_body) && own_body)
            out.append("Content-Type: application/json\r\n");
        out.append("Connection: keep-alive\r\n");
        net::http::append_content_length(out, body.size());
        out.append("\r\n");
        out.append(body);
    }

} // namespace llmbridge::detail
