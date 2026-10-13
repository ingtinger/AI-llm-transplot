// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// What the shared request lifecycle asks of an event-loop backend, and nothing more.

#include <cerrno>
#include <cstddef>
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

    /// How a backend bounds what a stream owes a slow client. GATEWAY-INTERNALS.md 6c.
    struct IoCaps
    {
        /// Pauses a stream's upstream for a slow client.
        bool stream_backpressure = false;
        /// Bytes a stream may owe before it pauses. 0: as soon as a send is left queued,
        /// which on epoll means the socket is full; an io_uring send is queued in flight.
        size_t stream_pause_above = 0;
        /// A stream owing more is dropped, paused or not; 0 for no cap.
        size_t stream_cap = 0;
    };

    /// Did a connect fail for want of something on this host (descriptors, memory, a full
    /// submission queue or epoll set)? No other venue would do better, and the venue is
    /// not to blame. Anything else is the venue's: no route, a refusal, and no free local
    /// port, since Linux picks one per destination and another venue may still have one.
    [[nodiscard]] constexpr bool connect_error_is_local(int err) noexcept
    {
        return err == EMFILE || err == ENFILE || err == ENOBUFS || err == ENOMEM || err == EAGAIN ||
               err == ENOSPC;
    }

    /// The 503's reason for a local connect error, naming what ran out: each is raised
    /// in a different place.
    [[nodiscard]] constexpr const char* local_connect_why(int err) noexcept
    {
        switch (err)
        {
        case EMFILE: return "no upstream (gateway out of resources: EMFILE, the process descriptor limit)";
        case ENFILE: return "no upstream (gateway out of resources: ENFILE, the system descriptor limit)";
        case ENOBUFS: return "no upstream (gateway out of resources: ENOBUFS, socket buffers)";
        case ENOMEM: return "no upstream (gateway out of resources: ENOMEM)";
        case ENOSPC: return "no upstream (gateway out of resources: ENOSPC, the epoll watch limit)";
        case EAGAIN: return "no upstream (gateway out of resources: EAGAIN, a full submission queue or routing cache)";
        default: return "no upstream (gateway out of resources)";
        }
    }

    /// A backend never calls back into shared code from inside one of these: a send
    /// that completes at once is a return value, so no handler runs re-entered.
    class IoOps
    {
      public:
        virtual Sent send(ConnBase& c) noexcept = 0;
        /// Open a fresh upstream's socket and start connecting: 0, or the errno that
        /// stopped it with nothing open (connect_error_is_local says whose it was).
        virtual int connect(UpstreamConn& u) noexcept = 0;
        virtual void set_read(ConnBase& c, bool on) noexcept = 0;
        /// `c`, and a client's upstream with it; freed once no event can reach it.
        virtual void close(ConnBase& c) noexcept = 0;

      protected:
        ~IoOps() = default;
    };
} // namespace llmbridge::detail
