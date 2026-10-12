// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// A request body forwarded to its venue as it arrives instead of after its last byte:
// GATEWAY-INTERNALS.md 12.

#include "engine.hpp"

#include <algorithm>

#include "core/io.hpp"
#include "core/limits.hpp"
#include "net/socket_util.hpp"
#include "request.hpp"
#include "response.hpp"

namespace llmbridge::detail
{

    bool Engine::body_streams(const ClientConn* c, const net::http::Message& m) const noexcept
    {
        // A policy reads the body before it routes, so its requests wait for all of it.
        if (!_stream_body || _policy || m.body_len < _stream_body_min || m.encoded ||
            c->rbuf.size() >= m.total_len)
            return false;
        if (!net::http::request_line_ok(c->rbuf)) return false; // the framing path refuses it
        const Dialect client = client_dialect_of(std::string_view(c->rbuf.data(), m.header_len));
        if (client == Dialect::OpenAI && !_stream_openai_body) return false;
        // A byte-forward only: a translation reads the whole body.
        const TranslationPlan plan = resolve_translation(client, _upstreams[0].dialect, false);
        return plan.ok && !plan.translate;
    }

    std::string_view Engine::body_so_far(const ClientConn* c) const noexcept
    {
        const size_t end = std::min(c->rbuf.size(), c->msg.total_len);
        if (end <= c->msg.header_len) return {};
        return std::string_view(c->rbuf.data() + c->msg.header_len, end - c->msg.header_len);
    }

    // New body bytes: scanned first, then whatever is known good goes upstream. The body
    // stays in rbuf, so a stale-connection retry can resend it. The time this takes after
    // t3 is ours (body_work_ns); before it, connect or upwrite holds it. The last byte's
    // arrival ends the client's upload.
    void Engine::pump_body(ClientConn* c) noexcept
    {
        const int64_t t = now_ns();
        const bool after_t3 = c->req.f.ts_up_sent != 0;
        const std::string_view body = body_so_far(c);
        const bool last = body.size() == c->msg.body_len;
        size_t safe = body.size();
        if (c->req.f.body_scanned)
        {
            if (c->req.scan->feed(body, last) == provider::BodyScan::Verdict::Refused) return refuse_body(c);
            safe = c->req.scan->safe();
        }
        if (last)
        {
            c->req.f.body_streaming = false;
            c->req.f.ts_body_end = t;
            c->req.f.client_upload_ns = span_since(c->ts_first_byte, t);
            // Setup holds the upload, as a held body's does, so subtracting it still works.
            if (!c->req.f.client_conn_reused) c->req.f.client_conn_setup_ns = span_since(c->ts_accepted, t);
            c->ts_first_byte = c->rbuf.size() > c->msg.total_len ? t : 0; // as open_request
            if (_sink) (void)capture_model(c); // the facts only: every byte already passed
        }
        UpstreamConn* u = c->up;
        if (u && safe > c->req.f.body_sent)
        {
            u->out.stage().append(body.substr(c->req.f.body_sent, safe - c->req.f.body_sent));
            c->req.f.body_sent = static_cast<uint32_t>(safe);
            c->req.f.ts_progress = t;
            // Unconnected, the connect's completion sends it.
            if (u->connected && !send_upstream(u)) return;
        }
        if (after_t3) c->req.f.body_work_ns += now_ns() - t;
    }

    // The body failed its scan after part of it went: stop, and reset the upstream so
    // the provider holds a body short of its Content-Length and never runs it.
    void Engine::refuse_body(ClientConn* c) noexcept
    {
        ++_stats.streamed_body_refusals;
        const char* why = c->req.scan->why();
        LB_WARN(ReqId{c->req.f.req_seq}, " streamed body refused (", why, ") after ",
                static_cast<uint64_t>(c->req.f.body_sent), " bytes went upstream; resetting it");
        c->req.f.body_streaming = false;
        if (UpstreamConn* u = unpair(c))
        {
            u->peer_eof = true; // never pooled
            if (u->fd >= 0) net::set_reset_on_close(u->fd);
            _ops->close(*u);
        }
        end_body_stream(c, nullptr);
        const bool keys = std::string_view(why).find("key") != std::string_view::npos;
        error_respond(c, 400, why, keys ? kAmbiguousKeysMessage : kNotJsonMessage);
    }

    // A response byte arrived, or the request is over: the copy in rbuf is no longer a
    // resend's, and body bytes still to come are dropped as they arrive.
    void Engine::end_body_stream(ClientConn* c, UpstreamConn* u) noexcept
    {
        if (c->req.f.body_streaming && u)
        {
            // The response beat the request out: cut short, the connection is never pooled.
            c->req.f.body_streaming = false;
            u->peer_eof = true;
            ++_stats.upstream_unsent;
            LB_WARN("response before the whole body went; the upstream will not be pooled ", *u);
        }
        const size_t held = std::min(c->rbuf.size(), c->msg.total_len);
        c->req.discard += c->msg.total_len - held;
        c->rbuf.erase(0, held);
        c->req.f.body_in_rbuf = false;
    }

    void Engine::drop_discard(ClientConn* c) noexcept
    {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(c->req.discard, c->rbuf.size()));
        c->rbuf.erase(0, n);
        c->req.discard -= n;
        if (c->req.discard) return;
        c->ts_first_byte = c->rbuf.empty() ? 0 : now_ns(); // the next request's arrival
        if (!c->req.close_on_drain) return;
        c->req.close_on_drain = false;
        _ops->close(*c);
    }

} // namespace llmbridge::detail
