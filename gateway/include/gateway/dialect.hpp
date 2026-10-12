// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <string_view>

#include "gateway/gateway.hpp" // UpstreamDialect

/// @file
/// The translation to run is a function of two dialects, not one: what the caller
/// speaks and what the venue speaks. The venue's dialect alone answered only the
/// second while assuming the first was OpenAI, which mistranslated any
/// Anthropic-speaking client. These pure functions resolve the pair, and the plan
/// they return separates "does a translator run" from "into which shape", because a
/// same-dialect pair byte-forwards and no dialect value can say that.
/// See DESIGN.md "Dialect resolution".
namespace llmbridge
{
    /// A body wire format. This is the payload shape only; a venue's auth scheme and
    /// URL layout are separate axes that `UpstreamDialect`'s Bedrock/Azure still carry.
    enum class Dialect
    {
        OpenAI,
        Anthropic,
        Gemini,
        Cohere,
    };

    namespace detail
    {
        constexpr bool ends_with(std::string_view s, std::string_view tail) noexcept
        {
            return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
        }
    } // namespace detail

    /// What the caller speaks, from its method and full request target: exact routes on
    /// the path, the query stripped. Anything else is OpenAI, the historical assumption,
    /// so an unknown endpoint behaves as it did before dialects existed.
    [[nodiscard]] constexpr Dialect client_wire(std::string_view method, std::string_view target) noexcept
    {
        const std::string_view path = target.substr(0, target.find('?'));
        if (detail::ends_with(path, "/chat/completions")) return Dialect::OpenAI; // the common case
        // Anthropic's batches take GET and DELETE as well; everything else here is a POST.
        if (path.find("/v1/messages/batches") != std::string_view::npos) return Dialect::Anthropic;
        if (method != "POST") return Dialect::OpenAI;
        if (detail::ends_with(path, "/v1/messages") || detail::ends_with(path, "/v1/messages/count_tokens"))
            return Dialect::Anthropic;
        if (detail::ends_with(path, ":generateContent") || detail::ends_with(path, ":streamGenerateContent"))
            return Dialect::Gemini;
        if (detail::ends_with(path, "/v2/chat")) return Dialect::Cohere;
        return Dialect::OpenAI;
    }

    /// The body dialect a venue speaks. Bedrock carries an Anthropic body under SigV4;
    /// Azure carries an OpenAI body under a rewritten URL; None is an OpenAI-compatible
    /// venue.
    [[nodiscard]] inline Dialect venue_body_dialect(UpstreamDialect venue) noexcept
    {
        switch (venue)
        {
            case UpstreamDialect::Anthropic:
            case UpstreamDialect::Bedrock:
                return Dialect::Anthropic;
            case UpstreamDialect::Gemini:
                return Dialect::Gemini;
            case UpstreamDialect::Cohere:
                return Dialect::Cohere;
            case UpstreamDialect::OpenAI:
            case UpstreamDialect::Azure:
                return Dialect::OpenAI;
        }
        return Dialect::OpenAI;
    }

    /// The translation to run for one (client, venue) pair. `ok == false` means no
    /// translator exists for the pair and the request must be refused, never sent: the
    /// caller speaks a dialect the venue's path was not built to receive.
    struct TranslationPlan
    {
        bool ok = false;
        /// Whether a translator runs at all.
        bool translate = false;
        /// The venue shape to translate into, meaningful only when `translate`.
        UpstreamDialect venue = UpstreamDialect::OpenAI;
        const char* why = "";
    };

    [[nodiscard]] inline TranslationPlan resolve_translation(Dialect client,
                                                             UpstreamDialect venue,
                                                             bool stream) noexcept
    {
        // Bedrock streams from /model/{id}/invoke-with-response-stream, which answers
        // in AWS event-stream framing, not SSE, and none of that is built.
        if (venue == UpstreamDialect::Bedrock && stream)
            return {false, false, venue, "bedrock venue cannot stream"};
        // Bedrock and Azure run SigV4 or a URL rewrite that a same-dialect byte-forward
        // would drop, and both are built for an OpenAI caller only. Leave them as they
        // are; refuse any other caller so no unsigned or mis-targeted bytes are sent.
        if (venue == UpstreamDialect::Bedrock || venue == UpstreamDialect::Azure)
        {
            if (client == Dialect::OpenAI) return {true, true, venue, ""};
            return {false, false, venue, "venue needs an OpenAI-dialect client"};
        }
        // Same dialect on both sides: forward the bytes. This is the case the old
        // single field could not express honestly.
        if (client == venue_body_dialect(venue)) return {true, false, venue, ""};
        if (client == Dialect::OpenAI) return {true, true, venue, ""};
        return {false, false, venue, "no translator for this client dialect to this venue"};
    }
} // namespace llmbridge
