// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// What the shared request lifecycle asks of an event-loop backend, and nothing more.

#include <cstdint>

namespace llmbridge
{
    struct ConnBase;
    struct ClientConn;
    struct UpstreamConn;
}

namespace llmbridge::detail
{
    /// Done: every owed byte is on the wire. Queued: a later event finishes it, or the
    /// backend already tore the connection down. Failed: the caller tears down.
    enum class Sent : uint8_t
    {
        Done,
        Queued,
        Failed,
    };

    struct IoCaps
    {
        /// Pauses a stream's upstream for a slow client; else kStreamBufCap bounds it.
        bool stream_backpressure = false;
    };

    /// A backend never calls back into shared code from inside one of these: a send
    /// that completes at once is a return value, so no handler runs re-entered.
    class IoOps
    {
      public:
        virtual Sent send(ConnBase& c) noexcept = 0;
        /// Open a fresh upstream's socket and start connecting; false: nothing is open.
        virtual bool connect(UpstreamConn& u) noexcept = 0;
        virtual void set_read(ConnBase& c, bool on) noexcept = 0;
        /// `c`, and a client's upstream with it; freed once no event can reach it.
        virtual void close(ConnBase& c) noexcept = 0;

      protected:
        ~IoOps() = default;
    };
} // namespace llmbridge::detail
