// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The epoll half of Gateway: every ep_ method and run_epoll(). Its twin is
// gateway_uring.cpp; the shared methods are in gateway.cpp. DESIGN.md "Naming
// conventions" says why the two are kept apart and never merged.

#include "gateway/gateway.hpp"

#include "core/io.hpp"
#include "core/limits.hpp"
#include "core/registry.hpp"
#include "net/secure.hpp"
#include "net/socket_util.hpp"
#include "request.hpp"
#include "response.hpp"
#include "scan.hpp"
#include "stream.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string_view>
#include <sys/epoll.h>

namespace llmbridge
{
    using namespace detail;

    // epoll is level-triggered here (no EPOLLET): every readable handler first
    // drains the socket into rbuf, so EPOLLIN won't re-fire on unread bytes, and
    // EPOLLIN stays armed for the connection's whole life. Write interest is
    // toggled via EPOLL_CTL_MOD on top of that always-on EPOLLIN.
    void Gateway::ep_add_read(Connection* c) noexcept
    {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_ADD, c->fd, &ev);
    }

    void Gateway::ep_pause_accept() noexcept
    {
        epoll_event ev{};
        ev.data.ptr = _listen_conn;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, _listen_fd, &ev);
        _accept_resume_ns = now_ns() + kAcceptBackoffNs;
        ++_stats.accept_backoffs;
        LB_WARN("CAP out of file descriptors; accepting paused for 100 ms, total=",
                _stats.accept_backoffs);
    }

    void Gateway::ep_resume_accept() noexcept
    {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = _listen_conn;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, _listen_fd, &ev);
        _accept_resume_ns = 0;
    }

    // The one writer of a connection's interest mask, from both flags: arming a write
    // used to set EPOLLIN too, silently undoing a backpressure pause.
    void Gateway::ep_sync_interest(Connection* c) noexcept
    {
        epoll_event ev{};
        ev.events = (c->read_paused ? 0u : static_cast<uint32_t>(EPOLLIN)) |
                    (c->write_armed ? static_cast<uint32_t>(EPOLLOUT) : 0u);
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }

    void Gateway::ep_arm_write(Connection* c) noexcept
    {
        if (c->write_armed) return;
        c->write_armed = true;
        ep_sync_interest(c);
    }

    void Gateway::ep_disarm_write(Connection* c) noexcept
    {
        if (!c->write_armed) return;
        c->write_armed = false;
        ep_sync_interest(c);
    }

    // EPOLLHUP and EPOLLERR still fire with EPOLLIN off, so a close is never missed.
    void Gateway::ep_set_read(Connection* c, bool on) noexcept
    {
        if (c->read_paused == !on) return;
        c->read_paused = !on;
        ep_sync_interest(c);
    }

    bool Gateway::ep_drain_read(Connection* c) noexcept
    {
        char tmp[16384];
        size_t pulled = 0;
        for (;;)
        {
            ssize_t n = ::read(c->fd, tmp, sizeof(tmp));
            if (n > 0)
            {
                if (c->ts_first_byte == 0) c->ts_first_byte = now_ns();
                c->rbuf.append(tmp, static_cast<size_t>(n));
                pulled += static_cast<size_t>(n);
                // Level-triggered: whatever is left re-notifies. Stopping here is what
                // lets back-pressure engage between events instead of after an
                // unbounded burst. See kEpMaxReadPerEvent.
                if (pulled >= kEpMaxReadPerEvent) return true;
                if (static_cast<size_t>(n) < sizeof(tmp)) return true;
                continue;
            }
            if (n == 0) return false;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            return false;
        }
    }

    bool Gateway::ep_pump_write(Connection* c, bool* done) noexcept
    {
        *done = false;
#ifdef LLMBRIDGE_HAVE_TLS
        if (!tls_invariant_ok(c)) return false; // never plaintext on a TLS conn
        // Branching here and not at the call sites is deliberate: respond,
        // ep_on_client_writable and the SSE stream flush all reach the socket
        // through this one function, so inbound TLS covers streaming for free
        // instead of needing a fourth copy of the logic.
        if (c->tls)
        {
            if (c->tls->handshake_done()) tls_push_out(c);
            bool flushed = false;
            if (!ep_tls_flush(c, &flushed)) return false;
            *done = flushed && tls_out_flushed(c);
            return true;
        }
#endif
        size_t budget = _ep_write_cap ? _ep_write_cap : SIZE_MAX;
        while (!c->out.wire().empty())
        {
            if (budget == 0) return true; // the test seam's EAGAIN
            const std::string_view w = c->out.wire();
            const ssize_t n = ::write(c->fd, w.data(), std::min(w.size(), budget));
            if (n > 0)
            {
                c->out.sent(static_cast<size_t>(n));
                c->sent_bytes += static_cast<uint64_t>(n);
                budget -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        // The bytes stay in `out` once sent: a request stays available for a
        // stale-connection resend until the response is read.
        *done = true;
        return true;
    }

#ifdef LLMBRIDGE_HAVE_TLS

    bool Gateway::ep_tls_flush(Connection* u, bool* done) noexcept
    {
        *done = false;
        tls_pump_out(u);
        size_t budget = _ep_write_cap ? _ep_write_cap : SIZE_MAX;
        while (!u->tls_out.wire().empty())
        {
            const std::string_view w = u->tls_out.wire();
            const ssize_t n = budget ? ::write(u->fd, w.data(), std::min(w.size(), budget)) : -1;
            if (n > 0)
            {
                u->tls_out.sent(static_cast<size_t>(n));
                u->sent_bytes += static_cast<uint64_t>(n);
                budget -= static_cast<size_t>(n);
                continue;
            }
            if (budget == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
            {
                ep_arm_write(u);
                return true;
            }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        u->tls_out.clear();
        *done = true;
        return true;
    }

    bool Gateway::ep_tls_drain_read(Connection* u) noexcept
    {
        char tmp[16384];
        size_t pulled = 0;
        for (;;)
        {
            const ssize_t n = ::read(u->fd, tmp, sizeof tmp);
            if (n > 0)
            {
                if (!tls_feed(u, tmp, static_cast<size_t>(n))) return false;
                pulled += static_cast<size_t>(n);
                if (pulled >= kEpMaxReadPerEvent) break; // see ep_drain_read
                if (static_cast<size_t>(n) < sizeof tmp) break;
                continue;
            }
            if (n == 0) return false; // EOF
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }
        // Feeding may have produced output (handshake flights, the request itself
        // once the handshake completed). Put it on the wire before returning to
        // the parse logic, and stamp the request-sent time on the transition.
        const bool was_flushed = u->peer && tls_out_flushed(u);
        bool done = false;
        if (!ep_tls_flush(u, &done)) return false;
        if (u->peer && !was_flushed && tls_out_flushed(u))
            u->peer->req.f.ts_up_sent = now_ns();
        return true;
    }

#endif // LLMBRIDGE_HAVE_TLS

    // close_* defer the free to the end of the epoll batch: an earlier event in
    // the same batch can close a conn that a later event still references via
    // data.ptr (e.g. a peer aborted while its own fd is also ready), so freeing
    // inline would dangle that pointer. Events for doomed conns are skipped in
    // run().
    void Gateway::ep_close_client(Connection* c) noexcept
    {
        if (c->doomed) return;
        // Every close unpairs; an upstream mid-response is of no use to anyone else.
        if (Connection* u = unpair(c)) ep_close_upstream(u);
        _clients->remove(c);
        LB_DEBUG("close ", *c, " clients=", _clients->size());
        if (c->fd >= 0) { ::close(c->fd); c->fd = -1; }
        c->doomed = true;
        _doomed->add(c);
    }

    void Gateway::ep_close_upstream(Connection* u) noexcept
    {
        if (u->doomed) return;
        unpair(u);
        _pool->remove(*u);
        _upconns->remove(u);
        // After the erase, so `pool=` excludes this connection.
        LB_DEBUG("upstream close ", *u, " pool=", pooled_upstream_count());
        if (u->fd >= 0) { ::close(u->fd); u->fd = -1; }
        u->doomed = true;
        _doomed->add(u);
    }

    void Gateway::ep_on_accept() noexcept
    {
        for (;;)
        {
            int fd = ::accept4(_listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR) continue;
                // The pending connection stays queued, so a level-triggered listener
                // would report it again at once: back off instead of spinning.
                if (errno == EMFILE || errno == ENFILE) ep_pause_accept();
                return;
            }
            net::set_nodelay(fd);
            net::set_nosigpipe(fd);
            if (_client_sndbuf > 0)
                ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &_client_sndbuf, sizeof(_client_sndbuf));
            Connection* c = new Connection();
            c->fd = fd;
            c->is_client = true;
            c->id = _next_client_id++;
            c->ts_accepted = now_ns();
        c->ts_client_activity = c->ts_accepted;
            c->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
            if (_tls.client_tls && !tls_attach_client(c))
            {
                // Fail closed. Answering in plaintext because the Session would
                // not attach is how a credential ends up on the wire in the clear.
                ::close(fd);
                delete c;
                ++_stats.errors;
                continue;
            }
#endif
            _clients->add(c);
            LB_DEBUG("accept ", *c, " clients=", _clients->size());
            ep_add_read(c);
        }
    }

    void Gateway::ep_on_client_readable(Connection* c) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        // Inbound TLS: ciphertext off the socket, plaintext into rbuf. Everything
        // after this, framing included, is unaware of TLS.
        const bool ok = c->tls ? ep_tls_drain_read(c) : ep_drain_read(c);
#else
        const bool ok = ep_drain_read(c);
#endif
        if (!ok) return client_gone(c);
        on_client_bytes(c);
    }

    void Gateway::ep_on_upstream_writable(Connection* u) noexcept
    {
        const uint64_t sent = u->sent_bytes;
        if (!u->connected)
        {
            int err = net::connect_result(u->fd);
            if (err != 0)
            {
                note_connect_failure(u->upstream_slot, std::strerror(err));
                Connection* client = unpair(u);
                ep_close_upstream(u);
                if (client) fail_request(client, 502, "upstream connect refused");
                else ++_stats.errors;
                return;
            }
            u->connected = true;
            // t2 for a plaintext upstream: the socket can carry the request now.
            // A TLS upstream is not wire-ready yet; tls_feed() stamps t2 when the
            // handshake completes.
            if (!upstream_is_tls(u))
            {
                u->wire_ready = true;
                if (u->peer) u->peer->req.f.ts_wire_ready = now_ns();
            }
#ifdef LLMBRIDGE_HAVE_TLS
            if (u->tls && !u->tls->handshake_done())
                u->tls->start_handshake(); // ClientHello lands in the write BIO
#endif
        }
#ifdef LLMBRIDGE_HAVE_TLS
        if (u->tls)
        {
            const bool was_flushed = u->peer && tls_out_flushed(u);
            bool tdone = false;
            if (!ep_tls_flush(u, &tdone)) return upstream_lost(u, UpFail::Write);
            if (u->peer && u->sent_bytes != sent) u->peer->req.f.ts_progress = now_ns();
            if (!tdone) return; // EPOLLOUT re-armed by ep_tls_flush
            ep_disarm_write(u); // ciphertext drained; handshake replies arrive via read
            if (u->peer && !was_flushed && tls_out_flushed(u))
                u->peer->req.f.ts_up_sent = now_ns();
            return;
        }
#endif
        bool done = false;
        if (!ep_pump_write(u, &done)) return upstream_lost(u, UpFail::Write);
        if (u->peer && u->sent_bytes != sent) u->peer->req.f.ts_progress = now_ns();
        if (!done) { ep_arm_write(u); return; }
        ep_disarm_write(u);
        if (u->peer) u->peer->req.f.ts_up_sent = now_ns(); // end of request-path work
    }

    void Gateway::ep_on_upstream_readable(Connection* u) noexcept
    {
        const bool read_ok =
#ifdef LLMBRIDGE_HAVE_TLS
            u->tls ? ep_tls_drain_read(u) :
#endif
                     ep_drain_read(u);
        note_response_bytes(u);
        if (read_ok) return on_upstream_bytes(u);
#ifdef LLMBRIDGE_HAVE_TLS
        on_upstream_eof(u, u->tls && u->tls->want() == net::tls::Want::Error);
#else
        on_upstream_eof(u, false);
#endif
    }

    void Gateway::ep_on_client_writable(Connection* c) noexcept
    {
        if (c->req.f.streaming) return stream_flush(c);
        respond(c);
    }

    // The seam the shared lifecycle drives: today's epoll primitives, one call each.
    class Gateway::EpollIo final : public IoOps
    {
      public:
        explicit EpollIo(Gateway& g) noexcept : _g(g) {}
        Sent send(Connection& c) noexcept override
        {
            bool done = false;
            if (!_g.ep_pump_write(&c, &done)) return Sent::Failed;
            if (done)
            {
                _g.ep_disarm_write(&c);
                return Sent::Done;
            }
            _g.ep_arm_write(&c); // socket full; finish on writability
            return Sent::Queued;
        }
        bool connect(Connection& u) noexcept override
        {
            const int fd = net::make_client_socket();
            if (fd < 0) return false;
            if (::connect(fd, reinterpret_cast<const sockaddr*>(&u.up_addr), sizeof(u.up_addr)) < 0 &&
                errno != EINPROGRESS)
            {
                ::close(fd);
                return false;
            }
            u.fd = fd;
            _g.ep_add_read(&u);
            _g.ep_arm_write(&u); // learn when the non-blocking connect completes
            return true;
        }
        void set_read(Connection& c, bool on) noexcept override { _g.ep_set_read(&c, on); }
        void close(Connection& c) noexcept override
        {
            if (c.is_client) _g.ep_close_client(&c);
            else _g.ep_close_upstream(&c);
        }

      private:
        Gateway& _g;
    };

    int Gateway::run_epoll()
    {
        _t_start = now_ns();
        _active_backend = 1; // also the io_uring fallback path
        EpollIo io(*this);
        attach(&io, IoCaps{.stream_backpressure = true});
        epoll_event events[kEpMaxEvents];

        while (!_stop)
        {
            // kPollTickMs timeout so request_stop() is observed within a tick.
            int n = ::epoll_wait(_epfd, events, kEpMaxEvents, kPollTickMs);
            if (n < 0) { if (errno == EINTR) continue; break; }
            for (int i = 0; i < n; ++i)
            {
                Connection* c = static_cast<Connection*>(events[i].data.ptr);
                if (c == _listen_conn) { ep_on_accept(); continue; }
                if (c->doomed) continue; // freed earlier this batch
                // Unlike kqueue, epoll coalesces a fd's readiness into one entry,
                // so a single event can carry both EPOLLIN and EPOLLOUT. Error/
                // hangup conditions fold into the readable path (ep_drain_read then
                // reports the EOF/error). Re-check doomed between the two halves.
                const uint32_t e = events[i].events;
                const bool readable = e & (EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP);
                const bool writable = e & EPOLLOUT;
                if (c->is_client)
                {
                    if (readable) ep_on_client_readable(c);
                    if (writable && !c->doomed) ep_on_client_writable(c);
                }
                else
                {
                    if (writable) ep_on_upstream_writable(c);
                    if (readable && !c->doomed) ep_on_upstream_readable(c);
                }
            }
            sweep_idle(); // heartbeat, pool reaping and every timeout
            if (_accept_resume_ns && now_ns() >= _accept_resume_ns) ep_resume_accept();
            _doomed->clear([this](Connection* d) {
                retire_wbuf(d);
                delete d;
            });
        }
        attach(nullptr, IoCaps{});
        return 0;
    }
} // namespace llmbridge
