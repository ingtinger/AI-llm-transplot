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
    struct Connection;
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
        /// Pausing a stream's upstream reads while its client drains. Without it a
        /// backend bounds a stream by its buffer cap instead.
        bool stream_backpressure = false;
    };

    /// A backend never calls back into shared code from inside one of these: a send
    /// that completes at once is a return value, so no handler runs re-entered.
    class IoOps
    {
      public:
        virtual Sent send(Connection& c) noexcept = 0;
        /// Open a fresh upstream's socket and start connecting; false: nothing is open.
        virtual bool connect(Connection& u) noexcept = 0;
        virtual void set_read(Connection& c, bool on) noexcept = 0;
        /// `c`, and a client's upstream with it; freed once no event can reach it.
        virtual void close(Connection& c) noexcept = 0;

        // The handlers each backend still writes itself, until they are shared.
        virtual void forward(Connection& client) noexcept = 0;
        virtual void reframe(Connection& client) noexcept = 0; // idle again: frame what it pipelined
        virtual void finish(Connection& client) noexcept = 0;
        virtual void stream_pump(Connection& u) noexcept = 0;
        virtual void stream_flush(Connection& client) noexcept = 0;

      protected:
        ~IoOps() = default;
    };
} // namespace llmbridge::detail
