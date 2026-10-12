// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Bytes between a socket and a connection's plaintext buffers: the TLS Session on
// either leg, and the sent-request test and stamp.

#include "engine.hpp"

#include "core/limits.hpp"
#include "request.hpp"

#include <cstdint>
#include <memory>

namespace llmbridge::detail
{

    // Compiles to `false` in a build without TLS support.
    bool Engine::upstream_is_tls(const UpstreamConn* u) const noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        return u->tls != nullptr;
#else
        (void)u;
        return false;
#endif
    }

#ifdef LLMBRIDGE_HAVE_TLS

    namespace
    {
        constexpr uint64_t kUnmeasured = UINT64_MAX; // a first send not yet encrypted
    }

    // ── TLS plumbing ────────────────────────────────────────────────────────
    // The invariant (see gateway.hpp): rbuf and `out` are plaintext, always. TLS lives
    // strictly between the socket and those buffers. On a TLS connection `out` counts
    // plaintext fed into the Session (its bytes stay for a resend), and `tls_out`
    // holds the ciphertext on its way to the socket.
    bool Engine::tls_required(const ClientConn*) const noexcept { return _tls.client_tls; }

    bool Engine::tls_required(const UpstreamConn* u) const noexcept { return upstream_of(u).tls; }

    bool Engine::tls_invariant_ok(ConnBase* c) noexcept
    {
        if (c->tls || !visit(*c, [this](const auto& k) { return tls_required(&k); })) return true;

        // Must be encrypted and has no Session: refuse the write, the caller tears it
        // down. Unreachable today and kept on purpose: GATEWAY-INTERNALS.md 10d.
        ++_stats.errors;
        return false;
    }

    bool Engine::tls_attach_upstream(UpstreamConn* u) noexcept
    {
        // The venue's own hostname, never _tls.sni_host. init_client drives both SNI
        // and SSL_set1_host from this, so a global value would verify every venue's
        // certificate against the first venue's name: a valid-cert-wrong-server hole
        // across the table. u->venue is set at every caller before this runs.
        u->tls = std::make_unique<net::tls::Session>();
        if (!u->tls->init_client(_tls_upstream_ctx, upstream_of(u).sni_host))
        {
            u->tls.reset();
            return false;
        }
        return true;
    }

    bool Engine::tls_attach_client(ClientConn* c) noexcept
    {
        c->tls = std::make_unique<net::tls::Session>();
        if (!c->tls->init_server(_tls_client_ctx))
        {
            c->tls.reset();
            return false;
        }
        // Accept state is already set by init_server. The handshake advances on the
        // first readable event: the client speaks first with a ClientHello, so there
        // is nothing to emit here and nothing to wait for.
        return true;
    }

    void Engine::tls_pump_out(ConnBase* u) noexcept
    {
        // stage() is safe while a send SQE reads tls_out: it is then the back half.
        // Reserved once for what this pass moves. Growing by append cost a 4 MB
        // request 1.6 ms of reallocation on a connection's first use, more than the
        // encryption itself.
        std::string& out = u->tls_out.stage();
        const size_t need = out.size() + u->tls->pending_output_bytes() + 256;
        if (out.capacity() < need) out.reserve(need + need / 8);
        uint8_t buf[65536];
        size_t n;
        while ((n = u->tls->pull_ciphertext({buf, sizeof buf})) > 0)
            out.append(reinterpret_cast<const char*>(buf), n);
        // Unsent only, the backlog, not the bytes streamed.
        const uint64_t unsent = u->tls_out.unsent() + u->tls->pending_output_bytes();
        if (unsent > _stats.tls_buffered_peak) _stats.tls_buffered_peak = unsent;
    }

    void Engine::tls_push_out(ConnBase* u, size_t limit) noexcept
    {
        // Feed as much plaintext as the Session accepts, up to `limit`; its ciphertext
        // lands straight in tls_out's stage, behind any flight already staged: order is
        // the wire's. Reserved for all of `out`, so a limited push is not a second grow.
        tls_pump_out(u);
        std::string& sink = u->tls_out.stage();
        const size_t todo = u->out.wire().size();
        // Headroom, as for the plaintext scratch: an exact reserve on a context that
        // grows every turn is a fresh allocation, and fresh pages, per request.
        const size_t need = sink.size() + todo + todo / 512 + 256;
        if (sink.capacity() < need)
        {
            ++_stats.tls_out_grows;
            sink.reserve(need + need / 8);
        }
        u->tls->set_sink(&sink);
        while (limit && !u->out.wire().empty())
        {
            const std::string_view w = u->out.wire().substr(0, limit);
            const size_t n = u->tls->write_plaintext({reinterpret_cast<const uint8_t*>(w.data()), w.size()});
            if (n == 0) break; // handshake not done, or session back-pressured
            u->out.sent(n);
            limit -= n;
        }
        u->tls->set_sink(nullptr);
    }

    // Encrypted alone, so no part staged behind it shares its last record, then counted.
    void Engine::tls_measure_first_send(UpstreamConn* u) noexcept
    {
        ReqState& f = u->client->req.f;
        tls_push_out(u, f.first_send_len);
        f.first_send_end = u->sent_bytes + u->tls_out.unsent();
    }

    bool Engine::tls_out_flushed(const ConnBase* u) const noexcept
    {
        return u->out.idle() && u->tls_out.idle() && !u->tls->has_pending_output();
    }

    template <class C>
    Engine::TlsIn Engine::tls_ingest(C* u, const char* p, size_t n, bool quiet_handshake) noexcept
    {
        const bool hs_was_done = u->tls->handshake_done();
        if (n > 0 &&
            u->tls->feed_ciphertext({reinterpret_cast<const uint8_t*>(p), n}) != n)
        {
            LB_WARN("TLS input refused ", *u, " handshake_done=", hs_was_done,
                    " err=", u->tls->last_error());
            return TlsIn::Failed;
        }
        if (u->tls->want() == net::tls::Want::Error)
        {
            // The error is an OpenSSL reason plus a file path, no key material. A
            // client handshake failure is almost always a scanner, so it is DEBUG and
            // counted; anything else is actionable and WARN. GATEWAY-INTERNALS.md 10d.
            if (quiet_handshake && !hs_was_done)
            {
                ++_stats.client_tls_handshake_failures;
                LB_DEBUG("TLS failed ", *u, " during=handshake err=", u->tls->last_error());
            }
            else
                LB_WARN("TLS failed ", *u, " during=", hs_was_done ? "session" : "handshake",
                        " err=", u->tls->last_error());
            return TlsIn::Failed;
        }

        // Plaintext into rbuf; framing and the SSE pump take over unchanged.
        uint8_t buf[16384];
        size_t r;
        while ((r = u->tls->read_plaintext({buf, sizeof buf})) > 0)
        {
            stamp_first_byte(*u);
            u->rbuf.append(reinterpret_cast<const char*>(buf), r);
        }
        if (u->tls->want() == net::tls::Want::Error)
        {
            LB_WARN("TLS read failed ", *u, " err=", u->tls->last_error());
            return TlsIn::Failed;
        }
        return !hs_was_done && u->tls->handshake_done() ? TlsIn::Handshook : TlsIn::Fed;
    }

    bool Engine::tls_feed(ClientConn* c, const char* p, size_t n) noexcept
    {
        const TlsIn r = tls_ingest(c, p, n, /*quiet_handshake=*/true);
        if (r == TlsIn::Handshook)
        {
            // The inbound handshake finishes before t0 (t0 is the framed request),
            // so it is invisible to every other number this file reports. Recorded
            // here or it cannot be seen at all. Warm-up gated like the others. A
            // client speaks first, so nothing is pending.
            const int64_t now = now_ns();
            LB_DEBUG("TLS established ", *c);
            if (c->ts_accepted > 0 && now - _t_start >= _warmup_ns)
                _stats.accept_tls.record(static_cast<uint64_t>(now - c->ts_accepted));
        }
        return r != TlsIn::Failed;
    }

    bool Engine::tls_feed(UpstreamConn* u, const char* p, size_t n) noexcept
    {
        const TlsIn r = tls_ingest(u, p, n, /*quiet_handshake=*/false);
        if (r == TlsIn::Handshook)
        {
            // t2 belongs here for TLS, not at TCP connect. The wire cannot carry
            // the request until the handshake is done, so stamping t2 earlier put
            // the entire handshake inside upwrite-us (t3-t2) and left connect-us
            // reporting the TCP leg alone. See the attribution test in
            // gateway/tests/gateway_tls_test.cpp. The request has waited in `out`.
            u->wire_ready = true;
            if (u->client) u->client->req.f.ts_wire_ready = now_ns();
            // The first send goes first, measured; parts that came meanwhile follow it.
            if (u->client && u->client->req.f.first_send_end == kUnmeasured) tls_measure_first_send(u);
            if (!u->out.idle()) tls_push_out(u);
        }
        return r != TlsIn::Failed;
    }

#endif // LLMBRIDGE_HAVE_TLS

    // The attempt's first send is what `u->out` holds now: the head and the body there at
    // dispatch. t3 waits for the wire bytes that carry it, never for parts staged behind
    // it (LATENCY.md 2b); TLS counts ciphertext, so the Session measures it as it encrypts.
    void Engine::mark_first_send(UpstreamConn* u) noexcept
    {
        ReqState& f = u->client->req.f;
        f.first_send_len = u->out.unsent();
        f.first_send_end = u->sent_bytes + f.first_send_len;
#ifdef LLMBRIDGE_HAVE_TLS
        if (!u->tls) return;
        f.first_send_end = kUnmeasured; // until the handshake is done (tls_feed)
        if (u->tls->handshake_done()) tls_measure_first_send(u);
#endif
    }

    // Every send that moved a client's bytes, on both backends: t3 once the first send is
    // in, and ts_body_sent each time all that is staged is, last at a streamed body's end.
    void Engine::note_sent(UpstreamConn* u, int64_t now) noexcept
    {
        ReqState& f = u->client->req.f;
        if (!f.ts_up_sent && u->sent_bytes >= f.first_send_end) f.ts_up_sent = now;
        if (upstream_request_sent(u)) f.ts_body_sent = now;
    }

    bool Engine::send_upstream(UpstreamConn* u) noexcept
    {
        const uint64_t before = u->sent_bytes;
        if (_ops->send(*u) == Sent::Failed)
        {
            upstream_lost(u, UpFail::Write);
            return false;
        }
        if (u->sent_bytes != before) note_sent(u, now_ns());
        return true;
    }

    bool Engine::upstream_request_sent(const UpstreamConn* u) const noexcept
    {
        if (u->send_inflight()) return false; // an SQE still reads the send buffer
        if (!u->out.idle()) return false;     // plaintext not fully handed over
#ifdef LLMBRIDGE_HAVE_TLS
        // On TLS, a sent `out` only means "fed to the Session". Ciphertext may still
        // be queued in tls_out or inside OpenSSL.
        if (u->tls && (!u->tls_out.idle() || u->tls->has_pending_output())) return false;
#endif
        return true;
    }
} // namespace llmbridge::detail
