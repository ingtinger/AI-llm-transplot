// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// The bytes a client is answered with when the gateway itself builds the reply:
// error envelopes, a relayed provider status, the SSE head, and the opt-in
// timing and usage headers.

#include <cstdint>
#include <string>
#include <string_view>

#include "provider/openai.hpp"

namespace llmbridge
{
    struct ReqState;
    struct TimingSplit;
}

namespace llmbridge::detail
{
    // Client-facing SSE response head. The stream is close-delimited: the body
    // ends when we close the socket, so no client-side chunk framing is needed.
    constexpr std::string_view kSseHead =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";

    /// The same reply framed so the connection survives it.
    constexpr std::string_view kSseHeadChunked =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";

    void append_timing_headers(std::string& out, const ReqState& f, const TimingSplit& sp,
                               const char* upstream_key);
    void append_usage_headers(std::string& out, const provider::openai::Usage& u);
    /// The builders append to `out`, a connection's stage, whose capacity is kept.
    void append_error(std::string& out, int code, const char* detail = nullptr);

    /// The 400 for a body whose top-level keys we could read differently from the
    /// provider. A literal: nothing from the body is echoed.
    inline constexpr const char* kAmbiguousKeysMessage =
        "request body repeats a top-level key or escapes one";
    /// The 400 for a forwarded body that turned out not to be JSON.
    inline constexpr const char* kNotJsonMessage = "request body is not valid JSON";
    /// `status` and `body` with Content-Length, keeping the upstream headers that
    /// net::http::relayed() allows; `own_body`: the upstream's body, with its type.
    void append_relayed_reply(std::string& out, int status, std::string_view upstream_head,
                              std::string_view body, bool own_body);
} // namespace llmbridge::detail
