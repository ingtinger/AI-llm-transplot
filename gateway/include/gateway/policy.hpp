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
        /// False when decide() runs on the head of a body still arriving (decides_on_head):
        /// `model` is a top-level `model` that has closed in the bytes so far, else empty;
        /// `stream` and `include_usage` are false; `prefix_hash` is over the same bytes as
        /// a held body's, the gateway waiting for them. confirm() gets the whole body's.
        bool body_whole = true;
        /// At confirm() only: the body is streaming to the venue with its top-level `model`,
        /// the plain string Decision::substitute_for (`model` above), already replaced by
        /// Decision::substitute_model. The decision in force has been carried out: returning
        /// it keeps the request, with no redo. False for a parked body, rewritten after.
        bool model_substituted = false;
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

    inline constexpr size_t kMaxSubstituteModel = 255;
    inline constexpr size_t kMaxSubstituteFor = 64;

    /// The rule for Decision::substitute_model (at most kMaxSubstituteModel bytes) and
    /// Decision::substitute_for (kMaxSubstituteFor), public so a policy can static_assert its
    /// own: the gateway writes either between quotes in a JSON body unescaped, and declares
    /// a streamed body's length from their sizes, so each is 1 or more bytes of printable
    /// ASCII with no `"` or `\`.
    [[nodiscard]] constexpr bool substitute_ok(std::string_view m, size_t max) noexcept
    {
        if (m.empty() || m.size() > max) return false;
        for (const char ch : m)
        {
            const auto c = static_cast<unsigned char>(ch);
            if (c < 0x20 || c > 0x7E || c == '"' || c == '\\') return false;
        }
        return true;
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
        /// Index into the gateway's upstream table; out of range (the -1 default too) means the
        /// first one, but past the end with credential_header set is a 500. Ignored unless `allow`.
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

        /// A decision on the head only: allowed, but nothing goes upstream before the
        /// body's last byte. Ignored on a whole body and from confirm().
        bool hold = false;

        /// A conditional model substitution, the one a head decision can make without the
        /// model: when the body's top-level `model` is the plain JSON string `substitute_for`
        /// (its raw bytes, no escape), it goes as `substitute_model` instead; any other model,
        /// or none, goes as the client sent it. Both or neither, each substitute_ok (else a
        /// 500); `model` set wins over it. On a body streamed as it arrives the gateway
        /// splices it into the stream, so the body need not park: GATEWAY-INTERNALS.md 12d.
        /// Copied, like `model`. kottos sets `"auto"` and the model it routes "auto" to.
        std::string_view substitute_model{};
        std::string_view substitute_for{};
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
        /// As Decision::credential_header, for the venue named here. Left empty after any
        /// attempt that named one, the decision or a retry, the request is not retried.
        std::string_view credential_header{};
        /// As Decision's, for the new venue; empty substitutes nothing. On a body still
        /// streaming it is spliced only when the head decision named one too (the gateway
        /// then tracked the model from the first byte); else the body parks, as for `model`.
        std::string_view substitute_model{};
        std::string_view substitute_for{};
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

        /// Opt in to deciding on the head of a large body, read once at construction.
        /// decide() then runs before the body is whole (RequestFacts::body_whole false),
        /// and the body may go to the venue as it arrives, its last byte withheld until
        /// confirm(). Off, every policy request is decided on its whole body, as before.
        virtual bool decides_on_head() const noexcept { return false; }

        /// Once per request decided on its head, with the whole body's facts, before its
        /// last byte leaves; never for a body decided whole, nor once the venue answered
        /// first. `early` is the decision in force: its venue (after any failover, never
        /// -1), overrides, key header and tag. Return it to keep: the last byte goes. A
        /// refusal (`allow` false, checked as decide()'s) resets a venue holding part of
        /// the body, short of its Content-Length, and the client gets its status and
        /// message. Any other allowed decision redoes the request: the venue is reset and
        /// the whole body goes as a held one under the new venue, model, service_tier,
        /// credential_header and tag. A change of tag alone is a keep, the tag updated, and so
        /// is a decision that sends the venue the same model the one in force does, as a
        /// substitution or an override (`whole.model_substituted` says the splice is done).
        /// The request was counted at decide(): do not count it again here.
        virtual Decision confirm(const RequestFacts& whole, const Decision& early) noexcept
        {
            (void)whole;
            return early;
        }

      protected:
        Policy() = default;
        Policy(const Policy&) = default;
        Policy& operator=(const Policy&) = default;
    };
} // namespace llmbridge
