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

    // Backpressure: pause/resume reading a connection. Used to stop pulling
    // upstream SSE bytes while the client's write buffer is draining, so a slow
    // client can't make us buffer an unbounded stream. (EPOLLHUP/EPOLLERR still
    // fire while paused, so an upstream close is never missed.)
    void Gateway::ep_set_read(Connection* c, bool on) noexcept
    {
        if (c->read_paused == !on) return;
        c->read_paused = !on;
        ep_sync_interest(c);
    }

    void Gateway::ep_pause_read(Connection* c) noexcept
    {
        if (c->read_paused) return;
        ep_set_read(c, false);
        ++_stats.stream_pauses; // observability: proves backpressure actually engaged
        // Both guards above are edge-triggered (`read_paused` early-returns), so this
        // is one line per episode, not per event. WARN like every other cap: at the
        // default INFO floor a DEBUG line is compiled out, which would make the epoll
        // half of this behaviour invisible in production while the io_uring half
        // (the 8 MiB drop) stayed visible. Same event, two backends, one log.
        LB_WARN("CAP backpressure, pausing upstream reads ", *c,
                " (the client is slower than the provider)");
    }

    void Gateway::ep_resume_read(Connection* c) noexcept
    {
        if (!c->read_paused) return;
        ep_set_read(c, true);
        // Same level as the pause: a log that opens an episode and never closes it
        // reads as still stuck.
        LB_WARN("CAP backpressure cleared, resuming upstream reads ", *c);
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
                if (client) { if (!upstream_failed(client, 502, "upstream connect refused")) error_respond(client, 502, "upstream connect refused"); }
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
            if (!ep_tls_flush(u, &tdone))
            {
                if (u->peer) { if (!retry_upstream(u) && !upstream_failed(u->peer, 502, "upstream write failed")) error_respond(u->peer, 502, "upstream write failed, retry exhausted"); }
                else ep_close_upstream(u);
                return;
            }
            if (u->peer && u->sent_bytes != sent) u->peer->req.f.ts_progress = now_ns();
            if (!tdone) return; // EPOLLOUT re-armed by ep_tls_flush
            ep_disarm_write(u); // ciphertext drained; handshake replies arrive via read
            if (u->peer && !was_flushed && tls_out_flushed(u))
                u->peer->req.f.ts_up_sent = now_ns();
            return;
        }
#endif
        bool done = false;
        if (!ep_pump_write(u, &done))
        {
            if (u->peer) { if (!retry_upstream(u) && !upstream_failed(u->peer, 502, "upstream write failed")) error_respond(u->peer, 502, "upstream write failed, retry exhausted"); }
            else ep_close_upstream(u);
            return;
        }
        if (u->peer && u->sent_bytes != sent) u->peer->req.f.ts_progress = now_ns();
        if (!done) { ep_arm_write(u); return; }
        ep_disarm_write(u);
        if (u->peer) u->peer->req.f.ts_up_sent = now_ns(); // end of request-path work
    }

    void Gateway::ep_on_upstream_readable(Connection* u) noexcept
    {
        Connection* client = u->peer;
        const bool read_ok =
#ifdef LLMBRIDGE_HAVE_TLS
            u->tls ? ep_tls_drain_read(u) :
#endif
                     ep_drain_read(u);
        note_response_bytes(u);
        if (!read_ok)
        {
            u->peer_eof = true; // never pooled now, whatever finishes the exchange
#ifdef LLMBRIDGE_HAVE_TLS
            // TLS parity with the io_uring path: a fatal session error (bad record,
            // MAC failure) mid-stream must abort the client, never finalize the
            // stream as clean: a corrupted stream that ends in a well-formed
            // [DONE] would hide the corruption from the client entirely. Only a
            // real transport EOF may end a close-delimited stream normally.
            if (u->tls && u->tls->want() == net::tls::Want::Error && client && client->req.f.streaming)
            {
                abort_pair(client);
                return;
            }
#endif
            if (client && client->req.f.streaming) { stream_on_upstream_eof(u); return; }
            if (client == nullptr) ep_close_upstream(u); // idle pooled conn dropped (eviction)
            else if (!retry_upstream(u) && !upstream_failed(client, 502, "upstream EOF")) error_respond(client, 502, "upstream EOF, retry exhausted");
            return;
        }
        // Stray bytes on an idle pooled upstream. They are the tail of an exchange
        // that is over, and acquire_upstream does not clear rbuf, so leaving them
        // hands one client's bytes to the next as the head of its response. The
        // io_uring twin already closes here; this side kept the connection.
        if (client == nullptr) { ep_close_upstream(u); return; }
        client->req.f.ts_progress = now_ns(); // upstream made progress

        // Mid-stream: pump the newly-arrived body bytes and return.
        if (client->req.f.streaming) { ep_stream_pump(u); return; }

        // First response bytes: for the Anthropic translate path, peek the head to
        // decide whole-body vs streaming (text/event-stream). Other modes and
        // non-streaming responses fall through to the whole-body path unchanged.
        // Anthropic (translated) or none (byte-forward). Gemini and Cohere are
        // non-streaming here, so a stream from one of those would be forwarded in a
        // dialect the client cannot read; they keep falling through to the whole-body
        // path until their translators exist.
        if (client->req.f.effective_dialect == UpstreamDialect::Anthropic ||
            client->req.f.effective_dialect == UpstreamDialect::Azure ||
            !client->req.f.translate_body)
        {
            net::http::ResponseHead h;
            const auto hs = net::http::parse_response_head(u->rbuf, h, sent_head(*client));
            if (hs == net::http::FrameStatus::NeedMore) return;
            if (hs == net::http::FrameStatus::Error) { error_respond(client, 502, "upstream response head framing"); return; }
            // Only a 200 carries a real event stream. A provider error (429 rate
            // limit, 529 overloaded, 400 context length, 401 auth) must reach the
            // client with its status (relayed below once the body is framed)
            // never laundered into a 200 stream.
            if (h.event_stream && h.status == 200 && h.body != net::http::Body::None)
            {
                // t4 for a stream: the provider's response head is now complete.
                // Not the first token, and not the first data chunk. A provider
                // May send 200 + content-type as soon as it accepts the stream,
                // well before its first generated token. Anthropic does not: measured
                // 2026-08-06, its head trails its first token by ~1 ms, so for that
                // provider this tracks TTFT closely. That is a per-provider fact, not
                // a protocol guarantee; see LATENCY.md §3 before assuming it holds.
                // The non-streaming path stamps this after framing, which this branch
                // returns before reaching, so stamp it here or it stays 0 and the
                // TTFB timing header reports garbage.
                client->req.f.ts_up_recvd = now_ns();
                note_upstream_rtt(client, u);
                note_quota(client, h);
                note_venue_req_id(client, std::string_view(u->rbuf.data(), h.header_len));
                begin_stream(u, h);
                return;
            }
        }

        // Stamp just before framing so the response HTTP parse is counted in the
        // response-path overhead, without charging inter-packet network wait.
        const int64_t t0 = now_ns();
        // parse_response, not parse: real providers return non-streaming bodies
        // chunked over HTTP/1.1, which parse_request() rejects by design (see http.hpp).
        // `r.body` aliases rbuf (Content-Length) or _resp_scratch (chunked); it is
        // dead before rbuf is erased or the upstream released, below.
        const auto r = net::http::parse_response(u->rbuf, u->rdec, sent_head(*client));
        if (r.failed()) { error_respond(client, 502, "upstream response framing"); return; }
        if (!r.complete()) return;
        const net::http::ResponseHead& h = r.head;
        const std::string_view body_buf = r.body;
        const size_t total_len = r.total_len;

        client->req.f.ts_up_recvd = t0; // end of upstream wait (stamped pre-framing)
        note_upstream_rtt(client, u);
        note_quota(client, h);
        note_venue_req_id(client, std::string_view(u->rbuf.data(), h.header_len));
        note_upstream_error(client, h, body_buf);
        note_served_tier(client, body_buf, /*tail=*/true);
        note_served_model(client, body_buf, /*streamed=*/false);

        if (client->req.f.translate_body)
        {
            const std::string_view body = body_buf;
            // Relay a provider failure with its own status + message (rate limit,
            // overloaded GPU, context length, auth); translating a non-200 body as
            // if it were a completion would fail and mask it as a generic 502.
            if (h.status != 0 && h.status != 200)
            {
                append_http_status(client->out.stage(), h.status, reason_for(h.status),
                                   provider::upstream_error_to_openai(body, "upstream_error"));
                client->req.f.phase = Phase::Replying;
                unpair(u);
                u->rbuf.erase(0, total_len);
                release_upstream(u, h.keep_alive, true);
                ++_stats.errors;
                respond(client);
                return;
            }
            if (!xlate_resp(client->req.f.effective_dialect, body, client->xlate_scratch, client->req.f.tok))
            {
                unpair(u);
                u->rbuf.erase(0, total_len); // framing was valid; the upstream conn is reusable
                release_upstream(u, h.keep_alive, true);
                error_respond(client, 502, "response translate");
                return;
            }
            std::string timing;
            if (_timing_headers)
            {
                // t5: the response is built here and written immediately after.
                const int64_t ts_resp_built = now_ns();
                const TimingSplit sp = timing_split(
                    client->req.f.ts_req_recvd, client->req.f.ts_req_built, client->req.f.ts_wire_ready,
                    client->req.f.ts_up_sent, client->req.f.ts_up_recvd, ts_resp_built);
                append_timing_headers(timing, client->req.f.ts_req_recvd, sp.compute_ns / 1000,
                                      sp.connect_ns / 1000, sp.upwrite_ns / 1000,
                                      sp.upstream_ns / 1000, "x-llmbridge-upstream-us",
                                      client->req.f.req_seq, client->req.f.client_upload_ns / 1000);
                append_usage_headers(timing, client->req.f.tok);
            }
            append_http(client->out.stage(), "HTTP/1.1 200 OK", client->xlate_scratch, timing);
        }
        else
        {
            // Passthrough: forward the upstream's own bytes. A chunked response is
            // re-framed with Content-Length instead of relayed verbatim; we have
            // already decoded it, and handing the client a chunked body we did not
            // re-verify would push our framing problem downstream.
            // The upstream's own status, not a hardcoded 200. Re-framing a chunked
            // response used to relabel every one of them as success, so a provider's
            // 429 reached the caller as a completion whose body happened to be a
            // rate-limit error, with Retry-After dropped. The client cannot back off
            // from a 200. Content-Length responses were relayed verbatim and were
            // never affected, which is why it survived: only chunkedness triggers it.
            if (h.body == net::http::Body::Chunked)
                append_http_status(client->out.stage(), h.status ? h.status : 200,
                                   reason_for(h.status ? h.status : 200), body_buf);
            else client->out.stage().append(u->rbuf.data(), total_len);
            // The counts, from the venue's own body. Only the translated branch above
            // scanned, so a byte-forward reported nothing: a sink saw -1 and a tape
            // recorded a request that cost zero tokens at a real price.
            client->req.f.tok = scan_usage(body_buf);
        }
        client->req.f.phase = Phase::Replying;

        // Response fully read -> upstream is free. Pool it only if it will stay
        // open (response keep-alive, and for passthrough the client didn't ask to
        // close); otherwise it's about to close, so drop it instead of reuse a
        // stale connection.
        const bool keep_alive =
            h.keep_alive && (client->req.f.translate_body || client->msg.keep_alive);
        unpair(u);
        // Drop the framed message; anything left is a second response and keeps the
        // connection out of the pool.
        u->rbuf.erase(0, total_len);
        release_upstream(u, keep_alive, true);
        respond(client);
    }

    void Gateway::ep_on_client_writable(Connection* c) noexcept
    {
        if (c->req.f.streaming) { ep_stream_flush(c); return; } // pump path has its own drain logic
        bool done = false;
        if (!ep_pump_write(c, &done)) return client_gone(c);
        if (!done) { ep_arm_write(c); return; }
        ep_finish_client(c);
    }

    void Gateway::ep_finish_client(Connection* c) noexcept
    {
        // Only a staged reply finishes. A drained interim 100 Continue, or a write event
        // with nothing owed, is no request: counting one wiped the real one's state.
        if (c->req.f.phase != Phase::Replying)
        {
            ep_disarm_write(c);
            return;
        }
        // Error replies (close_after_resp) are counted in _stats.errors, not the
        // latency histograms; their timing stamps are unset and would be garbage.
        if (!c->close_after_resp)
        {
            const int64_t ts_resp_sent = now_ns();
            if (ts_resp_sent - _t_start >= _warmup_ns)
            {
                // req_path is our request-side work only; the wait for connect +
                // TLS is its own histogram so it cannot inflate the added-latency
                // claim. overhead = req_path + resp_path, connect excluded.
                //
                // Derived from the same timing_split() the headers use, so
                // connect(TLS) here and x-llmbridge-connect-us there cannot come to
                // mean different things. t5 does not enter this grouping, so t4 is
                // passed in its place.
                const TimingSplit sp = timing_split(c->req.f.ts_req_recvd, c->req.f.ts_req_built,
                                                    c->req.f.ts_wire_ready, c->req.f.ts_up_sent,
                                                    c->req.f.ts_up_recvd, c->req.f.ts_up_recvd);
                const int64_t conn_ns = sp.connect_ns;
                const int64_t req_ns = sp.req_path_ns;
                const int64_t resp_ns = ts_resp_sent - c->req.f.ts_up_recvd;
                if (req_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(req_ns));
                if (conn_ns >= 0) _stats.connect.record(static_cast<uint64_t>(conn_ns));
                if (resp_ns >= 0) _stats.resp_path.record(static_cast<uint64_t>(resp_ns));
                if (req_ns >= 0 && resp_ns >= 0)
                    _stats.overhead.record(static_cast<uint64_t>(req_ns + resp_ns));
                ++_stats.requests;
            }
        }
        ep_disarm_write(c);
        if (_sink) sink_emit(c, status_of(c->out.bytes()), /*streamed=*/false);
        LB_DEBUG(ReqId{c->req.f.req_seq}, " status=", status_of(c->out.bytes()),
                 " bytes=", c->out.bytes().size(), " keep_alive=", c->msg.keep_alive, " on ", *c);
        c->out.clear();
        c->req.f.phase = Phase::Idle;
        const bool close_now = c->close_after_resp || !c->msg.keep_alive;
        c->msg = net::http::Message{};
        if (close_now) { ep_close_client(c); return; }
        on_client_bytes(c); // a pipelined next request is framed from rbuf, with no read
    }

    // Decode + translate the upstream body bytes now sitting in u->rbuf, appending
    // OpenAI SSE to the client's write buffer, then flush.
    void Gateway::ep_stream_pump(Connection* u) noexcept
    {
        Connection* client = u->peer;
        if (!client) { ep_close_upstream(u); return; } // lost peer mid-stream

        const StreamStep st = stream_step(client, u->rbuf, client->out.stage(), /*at_eof=*/false);
        if (st == StreamStep::Corrupt || st == StreamStep::Failed)
        {
            // Truncate honestly: flush what we already translated, then close
            // Without a terminal [DONE] so the client sees an aborted stream
            // instead of a fabricated clean finish.
            stream_truncate(client);
            ep_stream_flush(client);
            return;
        }
        ep_stream_flush(client);
    }

    // Write buffered SSE to the client. If it doesn't all go, finish on writability
    // and pause upstream reads (backpressure). On full flush, resume upstream, or
    // finalize if the stream has ended.
    void Gateway::ep_stream_flush(Connection* client) noexcept
    {
        const uint64_t sent = client->sent_bytes;
        bool done = false;
        if (!ep_pump_write(client, &done)) { abort_pair(client); return; } // client gone
        // A client draining a paused stream is progress, though the upstream is silent.
        if (client->sent_bytes != sent) client->req.f.ts_progress = now_ns();
        if (!done)
        {
            ep_arm_write(client);
            if (client->peer && !client->peer->doomed) ep_pause_read(client->peer);
            return;
        }
        client->out.clear();
        ep_disarm_write(client);
        if (client->req.f.stream_ended) { finalize_stream(client); return; }
        if (client->peer && !client->peer->doomed) ep_resume_read(client->peer);
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
            if (done) return Sent::Done;
            _g.ep_arm_write(&c); // socket full; finish on writability
            return Sent::Queued;
        }
        bool connect(Connection& u) noexcept override
        {
            const Upstream& up = _g.upstream_of(&u);
            const int fd = net::start_connect(up.ip.c_str(), up.port);
            if (fd < 0) return false;
            u.fd = fd;
            (void)net::resolve_ipv4(up.ip.c_str(), up.port, u.up_addr); // the ip may have rotated
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
        void finish(Connection& c) noexcept override { _g.ep_finish_client(&c); }
        void stream_pump(Connection& u) noexcept override { _g.ep_stream_pump(&u); }
        void stream_flush(Connection& c) noexcept override { _g.ep_stream_flush(&c); }

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
