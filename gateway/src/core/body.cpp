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

    bool Engine::body_streams(const Connection* c, const net::http::Message& m) const noexcept
    {
        // A policy reads the body before it routes, so its requests wait for all of it.
        if (_policy || m.body_len < _stream_body_min || m.encoded || c->rbuf.size() >= m.total_len)
            return false;
        if (!net::http::request_line_ok(c->rbuf)) return false; // the framing path refuses it
        const Dialect client = client_dialect_of(std::string_view(c->rbuf.data(), m.header_len));
        if (client == Dialect::OpenAI && !_stream_openai_body) return false;
        // A byte-forward only: a translation reads the whole body.
        const TranslationPlan plan = resolve_translation(client, _upstreams[0].dialect, false);
        return plan.ok && !plan.translate;
    }

    std::string_view Engine::body_so_far(const Connection* c) const noexcept
    {
        const size_t end = std::min(c->rbuf.size(), c->msg.total_len);
        if (end <= c->msg.header_len) return {};
        return std::string_view(c->rbuf.data() + c->msg.header_len, end - c->msg.header_len);
    }

    // New body bytes: scanned first, then whatever is known good goes upstream. The body
    // stays in rbuf, so a stale-connection retry can resend it.
    void Engine::pump_body(Connection* c) noexcept
    {
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
            if (_sink) (void)capture_model(c); // the facts only: every byte already passed
        }
        Connection* u = c->peer;
        if (!u || safe <= c->req.f.body_sent) return;
        u->out.stage().append(body.substr(c->req.f.body_sent, safe - c->req.f.body_sent));
        c->req.f.body_sent = static_cast<uint32_t>(safe);
        c->req.f.ts_progress = now_ns();
        if (!u->connected) return; // the connect's completion sends it
        switch (_ops->send(*u))
        {
            case Sent::Done: c->req.f.ts_up_sent = now_ns(); return;
            case Sent::Queued: return;
            case Sent::Failed: return upstream_lost(u, UpFail::Write);
        }
    }

    // The body failed its scan after part of it went: stop, and reset the upstream so
    // the provider holds a body short of its Content-Length and never runs it.
    void Engine::refuse_body(Connection* c) noexcept
    {
        ++_stats.streamed_body_refusals;
        const char* why = c->req.scan->why();
        LB_WARN(ReqId{c->req.f.req_seq}, " streamed body refused (", why, ") after ",
                static_cast<uint64_t>(c->req.f.body_sent), " bytes went upstream; resetting it");
        c->req.f.body_streaming = false;
        if (Connection* u = unpair(c))
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
    void Engine::end_body_stream(Connection* c, Connection* u) noexcept
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

    void Engine::drop_discard(Connection* c) noexcept
    {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(c->req.discard, c->rbuf.size()));
        c->rbuf.erase(0, n);
        c->req.discard -= n;
        if (c->req.discard || !c->req.close_on_drain) return;
        c->req.close_on_drain = false;
        _ops->close(*c);
    }

} // namespace llmbridge::detail
