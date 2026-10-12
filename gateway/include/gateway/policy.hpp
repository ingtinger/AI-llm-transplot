// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "net/http.hpp"

/// May this request proceed? llmbridge authenticates and meters nobody: whoever needs it
/// supplies a Policy. A stock build installs none. Design: DESIGN.md "The policy seam".
namespace llmbridge
{
    /// Metadata only, no route to the body. `head` carries the client's keys (Authorization,
    /// x-kottos-key-*): never log a RequestFacts or keep a view; use net::http::find_header.
    struct RequestFacts
    {
        std::string_view head;  ///< request line + headers, through the CRLFCRLF
        size_t body_bytes = 0;  ///< Content-Length as framed. Bytes, not tokens
        std::string_view model; ///< top-level plain-string `model`, else empty
        uint64_t prefix_hash = 0;
        uint64_t seq = 0; ///< the gateway's request sequencer
        /// Top-level `"stream": true` and `stream_options.include_usage`; anything else is false.
        bool stream = false;
        bool include_usage = false;
    };

    inline constexpr size_t kMaxDenyMessage = 256;

    /// The rule for Decision::message, public so a policy can static_assert its own. It lands
    /// unescaped in a JSON string: 1 to kMaxDenyMessage bytes of printable ASCII, no `"` or `\`.
    [[nodiscard]] constexpr bool deny_message_ok(const char* m) noexcept
    {
        if (!m) return false;
        size_t n = 0;
        for (; m[n] != '\0'; ++n)
        {
            const auto c = static_cast<unsigned char>(m[n]);
            if (n == kMaxDenyMessage || c < 0x20 || c > 0x7E || c == '"' || c == '\\')
                return false;
        }
        return n > 0;
    }

    /// Headers holding provider keys: no name with this prefix, in any case, goes upstream.
    inline constexpr std::string_view kKeyHeaderPrefix = "x-kottos-key-";
    inline constexpr size_t kMaxKeyHeaderName = 64;

    /// The rule for Decision::credential_header and Retry::credential_header, public so a
    /// policy can static_assert its own: the prefix in any case, then at least one of
    /// [A-Za-z0-9._-], kMaxKeyHeaderName bytes in all.
    [[nodiscard]] constexpr bool key_header_ok(std::string_view name) noexcept
    {
        if (name.size() <= kKeyHeaderPrefix.size() || name.size() > kMaxKeyHeaderName) return false;
        for (size_t i = 0; i < name.size(); ++i)
        {
            const char c = net::http::detail::lc(name[i]);
            const bool ok = i < kKeyHeaderPrefix.size()
                                ? c == kKeyHeaderPrefix[i]
                                : (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                                      c == '_' || c == '.';
            if (!ok) return false;
        }
        return true;
    }

    struct Decision
    {
        /// `= false` so `Decision d;` refuses too; `Decision{}` zeroes it either way.
        bool allow = false;
        /// Must be 400-599; the gateway substitutes 403 for anything else and warns.
        int deny_status = 401;
        /// Index into the gateway's upstream table; out of range (the -1 default too) means
        /// the first one. Ignored when `allow` is false.
        int upstream_index = -1;

        /// Rewrite the request's `model` before sending; empty leaves the client's. Valid
        /// until decide() returns; the gateway copies it, and a longer one than 255 bytes
        /// gets the request a 500 instead of a cut name.
        std::string_view model{};

        /// Set the request's `service_tier`, at most 32 bytes; empty leaves the body alone.
        std::string_view service_tier{};

        /// Logged, never sent; must outlive the call and carry no credential material.
        const char* reason = "policy denied";
        const char* message = nullptr;
        uint64_t tag = 0;

        /// The client header holding the key for the venue picked here, key_header_ok,
        /// such as `x-kottos-key-anthropic`; empty sends the client's own credentials. Its
        /// value goes as the venue's own key header and is the only credential sent: absent,
        /// empty, repeated or unsafe is a 400, never another header. Copied, like `model`.
        std::string_view credential_header{};
    };

    /// A venue did not answer (refused connect, failed write, EOF before the response, idle
    /// timeout) and the client has seen nothing: the policy may fail over. An unparseable
    /// answer never reaches here, since retrying would mask an incompatibility.
    struct FailureFacts
    {
        int upstream_index = -1; ///< the venue that just failed
        int status = 502;        ///< what the client gets if nothing is retried
        const char* reason = ""; ///< the gateway's short literal for the failure
        int attempt = 0;         ///< venues already tried for this request, 0 on the first
        uint64_t tag = 0;        ///< Decision::tag of this request, verbatim; 0 if unset
    };

    /// Value-initialised means give up and let the client see the error.
    struct Retry
    {
        bool retry = false;
        int upstream_index = -1; ///< must be in range, and not the one that just failed
        /// The new venue's overrides, as Decision's; empty sends the client's own, never
        /// the failed venue's. Valid until on_failure returns.
        std::string_view model{};
        std::string_view service_tier{};
    };

    /// Non-owning, fixed at Gateway construction, called on its loop thread once per framed
    /// request. A policy shared across workers must be thread-safe itself.
    class Policy
    {
      public:
        virtual ~Policy() = default;

        /// noexcept and allocation-free on the allow path: every request pays for it.
        virtual Decision decide(const RequestFacts& facts) noexcept = 0;

        /// After a venue failed with nothing sent to the client. The default never retries:
        /// llmbridge measures no health, so ordering, ejection and cooldown are the policy's.
        virtual Retry on_failure(const FailureFacts&) noexcept { return {}; }

        virtual bool wants_prefix_hash() const noexcept { return false; }

      protected:
        Policy() = default;
        Policy(const Policy&) = default;
        Policy& operator=(const Policy&) = default;
    };
} // namespace llmbridge
