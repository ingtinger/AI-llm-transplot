// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The epoll backend. Level-triggered (no EPOLLET): every readable handler drains the
// socket into rbuf, so EPOLLIN does not re-fire on unread bytes. Write interest is
// armed only when a write comes up short.

#include "io/epoll_io.hpp"

#include "core/conn.hpp"
#include "core/limits.hpp"
#include "core/registry.hpp"
#include "engine.hpp"
#include "net/socket_util.hpp"

#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace llmbridge::detail
{
    EpollIo::EpollIo(Engine& e) noexcept : _e(e)
    {
        _epfd = ::epoll_create1(EPOLL_CLOEXEC);
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = this; // the listener's identity in an event
        if (_epfd >= 0) ::epoll_ctl(_epfd, EPOLL_CTL_ADD, _e._listen_fd, &ev);
    }

    EpollIo::~EpollIo()
    {
        if (_epfd >= 0) ::close(_epfd);
    }

    void EpollIo::pause_accept() noexcept
    {
        epoll_event ev{};
        ev.data.ptr = this;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, _e._listen_fd, &ev);
        _e._accept_resume_ns = now_ns() + kAcceptBackoffNs;
        ++_e._stats.accept_backoffs;
        LB_WARN("CAP out of file descriptors; accepting paused for 100 ms, total=",
                _e._stats.accept_backoffs);
    }

    void EpollIo::resume_accept() noexcept
    {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.ptr = this;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, _e._listen_fd, &ev);
        _e._accept_resume_ns = 0;
    }

    // From both flags: arming a write used to set EPOLLIN too, silently undoing a pause.
    void EpollIo::sync_interest(Connection* c) noexcept
    {
        epoll_event ev{};
        ev.events = (c->read_paused ? 0u : static_cast<uint32_t>(EPOLLIN)) |
                    (c->write_armed ? static_cast<uint32_t>(EPOLLOUT) : 0u);
        ev.data.ptr = c;
        ::epoll_ctl(_epfd, EPOLL_CTL_MOD, c->fd, &ev);
    }

    void EpollIo::arm_write(Connection* c) noexcept
    {
        if (c->write_armed) return;
        c->write_armed = true;
        sync_interest(c);
    }

    void EpollIo::disarm_write(Connection* c) noexcept
    {
        if (!c->write_armed) return;
        c->write_armed = false;
        sync_interest(c);
    }

    // EPOLLHUP and EPOLLERR still fire with EPOLLIN off, so a close is never missed.
    void EpollIo::set_read(Connection& c, bool on) noexcept
    {
        if (c.read_paused == !on) return;
        c.read_paused = !on;
        sync_interest(&c);
    }

    bool EpollIo::drain_read(Connection* c) noexcept
    {
        char tmp[16384];
        size_t pulled = 0;
        for (;;)
        {
            const ssize_t n = ::read(c->fd, tmp, sizeof(tmp));
            if (n > 0)
            {
                if (c->ts_first_byte == 0) c->ts_first_byte = now_ns();
                c->rbuf.append(tmp, static_cast<size_t>(n));
                pulled += static_cast<size_t>(n);
                // What is left re-notifies; stopping lets back-pressure engage between events.
                if (pulled >= kMaxReadPerEvent) return true;
                if (static_cast<size_t>(n) < sizeof(tmp)) return true;
                continue;
            }
            if (n == 0) return false;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            return false;
        }
    }

    bool EpollIo::pump_write(Connection* c, bool* done) noexcept
    {
        *done = false;
#ifdef LLMBRIDGE_HAVE_TLS
        if (!_e.tls_invariant_ok(c)) return false; // never plaintext on a TLS conn
        // Every write reaches the socket here, so inbound TLS covers replies and
        // streams alike.
        if (c->tls)
        {
            if (c->tls->handshake_done()) _e.tls_push_out(c);
            bool flushed = false;
            if (!tls_flush(c, &flushed)) return false;
            *done = flushed && _e.tls_out_flushed(c);
            return true;
        }
#endif
        size_t budget = _e._ep_write_cap ? _e._ep_write_cap : SIZE_MAX;
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
        // Sent bytes stay in `out`: a request stays available for a stale-connection resend.
        *done = true;
        return true;
    }

#ifdef LLMBRIDGE_HAVE_TLS

    bool EpollIo::tls_flush(Connection* u, bool* done) noexcept
    {
        *done = false;
        _e.tls_pump_out(u);
        size_t budget = _e._ep_write_cap ? _e._ep_write_cap : SIZE_MAX;
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
                arm_write(u);
                return true;
            }
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        u->tls_out.clear();
        *done = true;
        return true;
    }

    bool EpollIo::tls_drain_read(Connection* u) noexcept
    {
        char tmp[16384];
        size_t pulled = 0;
        for (;;)
        {
            const ssize_t n = ::read(u->fd, tmp, sizeof tmp);
            if (n > 0)
            {
                if (!_e.tls_feed(u, tmp, static_cast<size_t>(n))) return false;
                pulled += static_cast<size_t>(n);
                if (pulled >= kMaxReadPerEvent) break; // see drain_read
                if (static_cast<size_t>(n) < sizeof tmp) break;
                continue;
            }
            if (n == 0) return false; // EOF
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            return false;
        }
        // Feeding may have produced output (handshake flights, or the request once the
        // handshake completed): put it on the wire, and stamp t3 on the transition.
        const bool was_flushed = u->peer && _e.tls_out_flushed(u);
        bool done = false;
        if (!tls_flush(u, &done)) return false;
        if (u->peer && !was_flushed && _e.tls_out_flushed(u))
            u->peer->req.f.ts_up_sent = now_ns();
        return true;
    }

#endif // LLMBRIDGE_HAVE_TLS

    // The fd closes now; the memory at the end of the batch, because a later event in
    // the same batch may still carry this connection in data.ptr (skipped as doomed).
    void EpollIo::close(Connection& c) noexcept
    {
        if (c.doomed) return;
        Connection* u = _e.forget(&c);
        if (c.fd >= 0)
        {
            ::close(c.fd);
            c.fd = -1;
        }
        if (u) close(*u); // an upstream mid-response is of no use to anyone else
    }

    // One EPOLL_CTL_ADD, read and write interest together: the connect's completion is
    // the first writable event.
    bool EpollIo::connect(Connection& u) noexcept
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
        u.write_armed = true;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.ptr = &u;
        ::epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &ev);
        return true;
    }

    Sent EpollIo::send(Connection& c) noexcept
    {
        bool done = false;
        if (!pump_write(&c, &done)) return Sent::Failed;
        if (done)
        {
            disarm_write(&c);
            return Sent::Done;
        }
        arm_write(&c); // socket full; finish on writability
        return Sent::Queued;
    }

    void EpollIo::on_accept() noexcept
    {
        for (;;)
        {
            const int fd = ::accept4(_e._listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR) continue;
                // The connection stays queued and a level-triggered listener reports it
                // again at once: back off instead of spinning.
                if (errno == EMFILE || errno == ENFILE) pause_accept();
                return;
            }
            net::set_nodelay(fd);
            net::set_nosigpipe(fd);
            if (_e._client_sndbuf > 0)
                ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &_e._client_sndbuf, sizeof(_e._client_sndbuf));
            Connection* c = new Connection();
            c->fd = fd;
            c->is_client = true;
            c->id = _e._next_client_id++;
            c->ts_accepted = now_ns();
            c->ts_client_activity = c->ts_accepted;
            c->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
            if (_e._tls.client_tls && !_e.tls_attach_client(c))
            {
                // Fail closed: a credential must never be answered in plaintext.
                ::close(fd);
                delete c;
                ++_e._stats.errors;
                continue;
            }
#endif
            _e._clients->add(c);
            LB_DEBUG("accept ", *c, " clients=", _e._clients->size());
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.ptr = c;
            ::epoll_ctl(_epfd, EPOLL_CTL_ADD, fd, &ev);
        }
    }

    void EpollIo::on_client_readable(Connection* c) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        const bool ok = c->tls ? tls_drain_read(c) : drain_read(c);
#else
        const bool ok = drain_read(c);
#endif
        if (!ok) return _e.client_gone(c);
        _e.on_client_bytes(c);
    }

    void EpollIo::on_client_writable(Connection* c) noexcept
    {
        if (c->req.f.streaming) return _e.stream_flush(c);
        _e.respond(c);
    }

    void EpollIo::on_upstream_writable(Connection* u) noexcept
    {
        const uint64_t sent = u->sent_bytes;
        if (!u->connected)
        {
            const int err = net::connect_result(u->fd);
            if (err != 0)
            {
                _e.note_connect_failure(u->upstream_slot, std::strerror(err));
                Connection* client = unpair(u);
                close(*u);
                if (client) _e.fail_request(client, 502, "upstream connect refused");
                else ++_e._stats.errors;
                return;
            }
            u->connected = true;
            // t2 for a plaintext upstream; tls_feed() stamps it when a handshake completes.
            if (!_e.upstream_is_tls(u))
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
            const bool was_flushed = u->peer && _e.tls_out_flushed(u);
            bool tdone = false;
            if (!tls_flush(u, &tdone)) return _e.upstream_lost(u, Engine::UpFail::Write);
            if (u->peer && u->sent_bytes != sent) u->peer->req.f.ts_progress = now_ns();
            if (!tdone) return; // EPOLLOUT re-armed by tls_flush
            disarm_write(u);    // handshake replies arrive via read
            if (u->peer && !was_flushed && _e.tls_out_flushed(u))
                u->peer->req.f.ts_up_sent = now_ns();
            return;
        }
#endif
        bool done = false;
        if (!pump_write(u, &done)) return _e.upstream_lost(u, Engine::UpFail::Write);
        if (u->peer && u->sent_bytes != sent) u->peer->req.f.ts_progress = now_ns();
        if (!done)
        {
            arm_write(u);
            return;
        }
        disarm_write(u);
        if (u->peer) u->peer->req.f.ts_up_sent = now_ns(); // end of request-path work
    }

    void EpollIo::on_upstream_readable(Connection* u) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        const bool read_ok = u->tls ? tls_drain_read(u) : drain_read(u);
#else
        const bool read_ok = drain_read(u);
#endif
        note_response_bytes(u);
        if (read_ok) return _e.on_upstream_bytes(u);
#ifdef LLMBRIDGE_HAVE_TLS
        _e.on_upstream_eof(u, u->tls && u->tls->want() == net::tls::Want::Error);
#else
        _e.on_upstream_eof(u, false);
#endif
    }

    int EpollIo::run()
    {
        if (_epfd < 0)
        {
            LB_ERROR("epoll_create1 failed (", errno, "); this worker takes no work");
            return 1;
        }
        _e._t_start = now_ns();
        _e._active_backend = 1;
        _e.attach(this, IoCaps{.stream_backpressure = true});
        epoll_event events[kMaxEvents];
        while (!_e._stop)
        {
            // A tick, so request_stop() is observed promptly.
            const int n = ::epoll_wait(_epfd, events, kMaxEvents, kPollTickMs);
            if (n < 0)
            {
                if (errno == EINTR) continue;
                break;
            }
            for (int i = 0; i < n; ++i)
            {
                if (events[i].data.ptr == this)
                {
                    on_accept();
                    continue;
                }
                Connection* c = static_cast<Connection*>(events[i].data.ptr);
                if (c->doomed) continue; // closed earlier this batch
                // One entry can carry both directions. Errors and hangups read as
                // readable, and drain_read reports them; doomed is re-checked between.
                const uint32_t e = events[i].events;
                const bool readable = e & (EPOLLIN | EPOLLHUP | EPOLLERR | EPOLLRDHUP);
                const bool writable = e & EPOLLOUT;
                if (c->is_client)
                {
                    if (readable) on_client_readable(c);
                    if (writable && !c->doomed) on_client_writable(c);
                }
                else
                {
                    if (writable) on_upstream_writable(c);
                    if (readable && !c->doomed) on_upstream_readable(c);
                }
            }
            _e.sweep_idle();
            if (_e._accept_resume_ns && now_ns() >= _e._accept_resume_ns) resume_accept();
            _e._doomed->clear([this](Connection* d) {
                _e.retire_wbuf(d);
                delete d;
            });
        }
        _e.attach(nullptr, IoCaps{});
        return 0;
    }
} // namespace llmbridge::detail
