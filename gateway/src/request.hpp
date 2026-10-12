// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Everything between a framed client request and the bytes for a venue: dialect
// resolution, the rebuild of a byte-forwarded request, the translated request with
// its credential headers, and the response translated back. The credential path
// is in here, in one file.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gateway/dialect.hpp"
#include "core/conn.hpp"
#include "engine.hpp"
#include "provider/translate.hpp"

namespace llmbridge::detail
{
    /// The request line for a log line: no credential, unlike every header after it;
    /// cut at 128 bytes so a hostile URI cannot dominate the log, and at the first CR.
    inline std::string_view request_line(std::string_view buf) noexcept
    {
        constexpr size_t kMax = 128;
        const size_t eol = buf.find('\r');
        size_t n = eol == std::string_view::npos ? buf.size() : eol;
        if (n > kMax) n = kMax;
        return buf.substr(0, n);
    }

    /// Status code off the front of a response we built; 0 if it is not one.
    inline int status_of(std::string_view resp) noexcept
    {
        if (resp.size() < 12 || resp.compare(0, 5, "HTTP/") != 0) return 0;
        const char a = resp[9], b = resp[10], c = resp[11];
        if (a < '0' || a > '9' || b < '0' || b > '9' || c < '0' || c > '9') return 0;
        return (a - '0') * 100 + (b - '0') * 10 + (c - '0');
    }

    /// The venue was sent HEAD, so its response has no body. Only a byte-forward keeps
    /// the client's method; a translated request is always a POST.
    [[nodiscard]] inline bool sent_head(const ClientConn& c) noexcept
    {
        return c.msg.head && !c.req.f.translate_body;
    }

    [[nodiscard]] inline int64_t span_since(int64_t start, int64_t end) noexcept
    {
        return (start > 0 && end >= start) ? end - start : 0;
    }

    constexpr std::string_view kContinue = "HTTP/1.1 100 Continue\r\n\r\n";

    /// Fills a HeadFacts from each header line it is shown, offsets from `head`'s start;
    /// handed to net::http::parse_request so framing and the facts are one walk.
    struct HeadReader
    {
        std::string_view head;
        const std::vector<std::string>& strip;
        HeadFacts& f;
        bool operator()(const net::http::HeaderLine& h) noexcept;
    };

    /// The client's dialect from the request line of `head`: client_wire on its method
    /// and full target.
    [[nodiscard]] Dialect client_dialect_of(std::string_view head) noexcept;
    /// application/json or a +json type, whatever its parameters and case.
    [[nodiscard]] bool json_media_type(std::string_view content_type) noexcept;
    /// Everything a policy or the sink reads from the body; the gateway calls this
    /// once per request.
    [[nodiscard]] provider::TopLevelFacts body_facts(std::string_view head,
                                                     std::string_view body) noexcept;
    TranslationPlan resolve_dialect(const ClientConn* c, const Upstream& venue) noexcept;
    const char* translate_failure(std::string_view body) noexcept;
    const char* dialect_name(UpstreamDialect m) noexcept;
    void append_http(std::string& out, std::string_view start_line, std::string_view body,
                     std::string_view extra); // appended: `out` is a stage, capacity kept
    void build_http_request(std::string_view start_line, std::string_view body,
                            std::string_view host, std::string_view extra, std::string& into);
    /// The byte-forwarded request into `into`, capacity kept: `msg`'s head, then `body`,
    /// all or the first part of one `declared` bytes long. Null, or why to refuse it,
    /// with `into` empty.
    const char* build_forward(std::string_view msg, size_t header_len, const HeadFacts& f,
                              const std::vector<std::string>& strip, std::string_view host_hdr,
                              std::string_view base_path, std::string_view body,
                              std::string& into, size_t declared);
    /// build_forward over a message whose facts are read here; false refuses it.
    bool request_without(std::string_view msg, size_t header_len,
                         const std::vector<std::string>& strip,
                         std::string_view host_hdr, std::string_view base_path,
                         std::string_view body_override, std::string& into);
    /// `out` receives the framed request and `body_scratch` the translated body; both
    /// keep their capacity. `head` is the client's, `f` its facts. On false, `why` is
    /// "translate", "credential", "bedrock credential" or "no credential".
    bool build_translated_request(const Upstream& up, UpstreamDialect mode,
                                  std::string_view body, std::string_view head,
                                  const HeadFacts& f, std::string& out,
                                  std::string& body_scratch, const char*& why,
                                  std::string_view model_override = {},
                                  bool* wants_stream_usage = nullptr);
    /// The credential lines a translated request carries, from one header block. A test
    /// seam over the facts the framing walk reads.
    bool auth_headers_for(UpstreamDialect mode, std::string_view client_headers,
                          const std::vector<std::string>& strip, std::string& out);
    bool xlate_resp(UpstreamDialect mode, std::string_view body, std::string& out,
                    provider::openai::Usage& u);
    std::string host_header_for(const Upstream& u);
    std::string aws_region_for(const Upstream& u);
    std::string peer_of(int fd) noexcept;
} // namespace llmbridge::detail
