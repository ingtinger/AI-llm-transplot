// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <string_view>

#include "gateway/gateway.hpp" // UpstreamDialect
#include "provider/dialect.hpp"

/// @file
/// The translation to run is a function of the caller's dialect and the venue's, read
/// from the venue table: DESIGN.md "Dialect resolution".
namespace llmbridge
{
    /// A body wire format: the payload shape only. A venue's auth scheme and URL layout
    /// are separate axes, in its VenueSpec.
    using Dialect = provider::Wire;
    using provider::client_wire;

    /// The body dialect a venue speaks.
    [[nodiscard]] inline Dialect venue_body_dialect(UpstreamDialect venue) noexcept
    {
        return provider::spec(venue).wire;
    }

    /// The translation for one (client, venue) pair. `ok == false` refuses the request:
    /// no translator exists for the pair. A same-dialect pair forwards, `translate` false.
    struct TranslationPlan
    {
        bool ok = false;
        bool translate = false;
        UpstreamDialect venue = UpstreamDialect::OpenAI; ///< the shape to translate into
        const char* why = "";
    };

    /// `stream`: the client asked for one, which a venue that cannot stream refuses.
    [[nodiscard]] inline TranslationPlan resolve_translation(Dialect client,
                                                             UpstreamDialect venue,
                                                             bool stream) noexcept
    {
        const provider::VenueSpec& s = provider::spec(venue);
        // Same dialect on both sides: forward the bytes, unless the venue's transport
        // (SigV4, a rewritten URL) needs the request rebuilt.
        if (client == s.wire && s.same_wire_forwards) return {true, false, venue, ""};
        if (client != Dialect::OpenAI)
            return {false, false, venue, s.same_wire_forwards ? "no translator for this client dialect to this venue"
                                                              : "venue needs an OpenAI-dialect client"};
        if (stream && !s.streams) return {false, false, venue, "venue cannot stream"};
        return {true, true, venue, ""};
    }
} // namespace llmbridge
