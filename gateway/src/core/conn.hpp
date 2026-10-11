// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// The two kinds of connection, what they share, and their log rendering. Which buffer
// holds what, who may free a connection on each backend, and how the two TLS legs fit:
// GATEWAY-INTERNALS.md.

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/outbuf.hpp"
#include "core/req.hpp"
#include "gateway/metrics.hpp"
#include "gateway/venue.hpp"
#include "net/http.hpp"
#include "net/log.hpp"
#include "net/tls.hpp" // self-guarded by LLMBRIDGE_HAVE_TLS
#include "provider/openai.hpp"
#include "provider/sse.hpp"

namespace llmbridge
{
    enum class ConnKind : uint8_t
    {
        Client,
        Upstream,
    };

    /// What a socket needs on either leg. Never deleted through this type: Engine::free_conn
    /// knows the kind. Aligned so a backend can tag a pointer with an op and the kind.
    struct alignas(16) ConnBase
    {
        ConnBase(const ConnBase&) = delete;
        ConnBase& operator=(const ConnBase&) = delete;
        /// Live instances, so a test can assert every connection is freed.
        inline static std::atomic<long> s_live{0};

        /// Log identity, process-unique across both kinds.
        uint64_t log_inst = net::log::next_instance();
        /// Bytes the socket took over the connection's life, on either transport.
        uint64_t sent_bytes = 0;
        std::string rbuf; ///< plaintext received: a request on a client, a response upstream
        /// Plaintext owed to the peer: on the socket, or for TLS fed to the Session.
        OutBuf out;
#ifdef LLMBRIDGE_HAVE_TLS
        /// Null = plaintext. Kept across pool cycles: a pooled reuse pays no handshake.
        std::unique_ptr<net::tls::Session> tls;
        OutBuf tls_out; ///< ciphertext awaiting the socket (10b)
#endif
        // The small fields last, so a derived type packs its own into the tail padding.
        int fd = -1;
        /// Index in the Registry that holds it: live clients, live upstreams or doomed.
        uint32_t slot = UINT32_MAX;
        int inflight = 0; ///< io_uring: SQEs still referencing this conn (5b)
        const ConnKind kind;
        /// Closed, not yet freed: GATEWAY-INTERNALS.md section 7.
        bool doomed = false;
        bool write_armed = false; ///< epoll: EPOLLOUT registered
        /// Reads paused, written only by a backend's set_read. GATEWAY-INTERNALS.md 6c.
        bool read_paused = false;
        bool recv_armed = false; ///< io_uring: a multishot recv is live on this fd
        /// What the socket reads: the ciphertext on TLS, else the plaintext. Only an
        /// io_uring send pins it. GATEWAY-INTERNALS.md 5b.
        OutBuf& wire() noexcept
        {
#ifdef LLMBRIDGE_HAVE_TLS
            if (tls) return tls_out;
#endif
            return out;
        }
        [[nodiscard]] bool send_inflight() const noexcept
        {
#ifdef LLMBRIDGE_HAVE_TLS
            if (tls) return tls_out.pinned();
#endif
            return out.pinned();
        }

      protected:
        explicit ConnBase(ConnKind k) noexcept : kind(k) { s_live.fetch_add(1, std::memory_order_relaxed); }
        ~ConnBase() { s_live.fetch_sub(1, std::memory_order_relaxed); }
    };

    struct UpstreamConn;

    /// A customer's connection to us, and the request in flight on it.
    struct ClientConn final : ConnBase
    {
        ClientConn() noexcept : ConnBase(ConnKind::Client) {}

        uint64_t id = 0; ///< stable, for the log
        /// The venue serving the request in flight, for its dialect; -1 for none.
        int upstream_slot = -1;
        bool close_after_resp = false; ///< an error reply, or a truncated stream: close once flushed
        bool ever_framed = false;
        int64_t ts_accepted = 0;
        int64_t ts_first_byte = 0; ///< the first byte of the request being buffered
        /// When this connection last completed a request, for the idle deadline; the
        /// setup deadline covers one that never framed anything.
        int64_t ts_client_activity = 0;
        /// Total length of the request being buffered, from its head, or 0.
        size_t client_frame_want = 0;
        UpstreamConn* up = nullptr; ///< pair()/unpair() only
        net::http::Message msg{};
        RequestCtx req; ///< reset by req.begin() at framing
        /// The SSE translator, reset per stream; by value, so a stream allocates none.
        provider::AnthropicToOpenAiSse sse_xlate;
        /// A translated reply body, before it is framed into `out`; capacity kept.
        std::string xlate_scratch{};
    };

    /// Our connection to a venue: opened, pooled and reused across clients.
    struct UpstreamConn final : ConnBase
    {
        UpstreamConn() noexcept : ConnBase(ConnKind::Upstream) {}

        int venue = -1; ///< upstream table index: its pool, its address, its TLS name
        bool connected = false; ///< the non-blocking connect finished
        bool wire_ready = false;
        /// Reused from the pool, so a failure before any response may retry once.
        bool from_pool = false;
        bool retried = false;
        bool pooled = false;   ///< UpstreamPool's membership; nothing else writes it
        bool peer_eof = false; ///< the venue sent EOF, so this connection is never pooled
        sockaddr_in up_addr{};
        int64_t ts_opened = 0; ///< socket creation; the connect deadline counts from here
        /// When it entered the idle pool (reaped after _pool_idle_ns), and when this
        /// request took it, which bounds the retry.
        int64_t ts_pooled = 0;
        int64_t ts_pool_taken = 0;
        net::http::ResponseFramer framer; ///< the response in flight, its head parsed once
        ClientConn* client = nullptr;     ///< pair()/unpair() only
        UpstreamConn* pool_newer = nullptr; ///< UpstreamPool's links
        UpstreamConn* pool_older = nullptr;
    };
    static_assert(sizeof(UpstreamConn) < 512, "a pooled upstream is held 8,192 times over");

    /// One call on the concrete kind, the only place that switches on it.
    template <class F> decltype(auto) visit(ConnBase& c, F&& f)
    {
        if (c.kind == ConnKind::Client) return f(static_cast<ClientConn&>(c));
        return f(static_cast<UpstreamConn&>(c));
    }
    template <class F> decltype(auto) visit(const ConnBase& c, F&& f)
    {
        if (c.kind == ConnKind::Client) return f(static_cast<const ClientConn&>(c));
        return f(static_cast<const UpstreamConn&>(c));
    }

    /// Starts a request's upload clock, from its first byte; an upstream keeps none.
    inline void stamp_first_byte(ClientConn& c) noexcept
    {
        if (c.ts_first_byte == 0) c.ts_first_byte = now_ns();
    }
    inline void stamp_first_byte(UpstreamConn&) noexcept {}

    /// The client whose request a connection serves, if any.
    inline ClientConn* client_of(ClientConn& c) noexcept { return &c; }
    inline ClientConn* client_of(UpstreamConn& u) noexcept { return u.client; }

    /// The only writers of `up` and `client`, both directions at once, so a link is
    /// never one-sided.
    inline void pair(ClientConn* c, UpstreamConn* u) noexcept
    {
        c->up = u;
        u->client = c;
    }
    /// Either leg; returns the other, now unlinked, or null.
    inline UpstreamConn* unpair(ClientConn* c) noexcept
    {
        UpstreamConn* u = c->up;
        if (u) u->client = nullptr;
        c->up = nullptr;
        return u;
    }
    inline ClientConn* unpair(UpstreamConn* u) noexcept
    {
        ClientConn* c = u->client;
        if (c) c->up = nullptr;
        u->client = nullptr;
        return c;
    }

    /// A response byte reached `u`'s rbuf: the venue may have acted, so no retry now.
    inline void note_response_bytes(UpstreamConn* u) noexcept
    {
        if (u->client && !u->rbuf.empty() && u->client->req.f.phase == Phase::Dispatched)
            u->client->req.f.phase = Phase::Responding;
    }

    /// Renders `ClientConnection#42(fd=17,cid=2)` for `LB_INFO("closed ", *c)`. Never prints
    /// rbuf or out: they hold the customer's request, credential included.
    inline void log_put(net::log::Line& l, const ClientConn& c)
    {
        l.put(net::log::Id{"ClientConnection", c.log_inst});
        l.put("(fd=");
        l.put(static_cast<int64_t>(c.fd));
        if (c.id)
        {
            l.put(",cid=");
            l.put(c.id);
        }
        l.put(')');
    }
    inline void log_put(net::log::Line& l, const UpstreamConn& u)
    {
        l.put(net::log::Id{"UpstreamConnection", u.log_inst});
        l.put("(fd=");
        l.put(static_cast<int64_t>(u.fd));
        l.put(')');
    }
    inline void log_put(net::log::Line& l, const ConnBase& c)
    {
        visit(c, [&l](const auto& k) { log_put(l, k); });
    }

    /// A request is not an object with a lifetime, so it is named by the sequencer:
    /// `Request#123`, the same number the timing header carries.
    struct ReqId
    {
        uint64_t seq;
    };
    inline void log_put(net::log::Line& l, ReqId r)
    {
        l.put(net::log::Id{"Request", r.seq});
    }
} // namespace llmbridge
