// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The policy seam on a freshly framed request, and replying to a client.

#include "engine.hpp"

#include "core/io.hpp"
#include "core/limits.hpp"
#include "core/registry.hpp"
#include "request.hpp"
#include "response.hpp"

namespace llmbridge::detail
{

    Decision Engine::policy_decision(ClientConn* c, const net::http::Message& m) noexcept
    {
        // Head, plus the model and two streaming flags and nothing else from the body.
        // This is the line where "metadata only, no prompt text". All three were read
        // by capture_model, which both backends run first when a policy exists.
        const RequestFacts facts{std::string_view(c->rbuf.data(), m.header_len), m.body_len,
                                 c->req.f.model.view(),
                                 c->req.f.prefix_hash, c->req.f.req_seq, c->req.f.asked_stream, c->req.f.asked_usage};

        Decision d = _policy->decide(facts);
        if (d.allow)
        {
            c->req.f.policy_tag = d.tag; // handed back verbatim in FailureFacts
            return d;
        }

        // A status the responder cannot render would emit a misleading reply, so
        // substitute and say so loudly. The refusal still stands: fail closed.
        if (d.deny_status < 400 || d.deny_status > 599)
        {
            LB_WARN(ReqId{c->req.f.req_seq}, " policy returned out-of-range status ", d.deny_status,
                    " (", d.reason, "); refusing with 403");
            d.deny_status = 403;
        }
        // The one policy string a client sees, spliced into JSON unescaped. Never
        // printed here: a message that failed the check is not safe in a log line.
        if (d.message && !deny_message_ok(d.message))
        {
            LB_WARN(ReqId{c->req.f.req_seq}, " policy message refused (", d.reason,
                    "); sending the generic one");
            d.message = nullptr;
        }
        // Never log `facts`: the head carries the client's Authorization.
        ++_stats.policy_denied;
        return d;
    }

    void Engine::on_client_bytes(ClientConn* c) noexcept
    {
        if (c->req.discard) drop_discard(c);
        if (c->doomed) return;
        if (c->req.f.body_streaming) pump_body(c);
        else if (c->req.f.phase == Phase::Idle && !c->rbuf.empty()) try_frame(c);
        if (!c->doomed) gate_client_reads(c);
    }

    // Reading continues while a request is in flight, so this is the only bound on what
    // a client can pipeline behind it. No legal request is larger, so a pause costs a
    // legitimate client nothing; it resumes once its requests drain the buffer.
    void Engine::gate_client_reads(ClientConn* c) noexcept
    {
        const bool over = c->req.f.phase != Phase::Idle && c->rbuf.size() > kMaxClientBuffered;
        if (over == c->read_paused) return;
        if (over)
        {
            ++_stats.client_read_pauses;
            LB_WARN("CAP client pipelined ", c->rbuf.size(), " bytes behind its request in flight ",
                    *c, "; reading paused");
        }
        _ops->set_read(*c, !over);
    }

    void Engine::client_gone(ClientConn* c) noexcept
    {
        // Mid-request (a reply or stream still owed included) is an abort; idle is normal.
        if (c->req.f.phase != Phase::Idle) abort_pair(c);
        else _ops->close(*c);
    }

    void Engine::send_continue(ClientConn* c) noexcept
    {
        // Phase stays Idle, so the send completing finishes nothing.
        c->out.stage().append(kContinue);
        if (_ops->send(*c) == Sent::Failed) _ops->close(*c);
    }

    // t0 is stamped before the parse that frames the request, so the parse counts in the
    // request path; a partial request discards it, so network wait never does.
    void Engine::try_frame(ClientConn* c) noexcept
    {
        // A request whose length is known is not re-parsed per read.
        if (c->client_frame_want && c->rbuf.size() < c->client_frame_want) return;
        const int64_t t0 = now_ns();
        net::http::Message m;
        // The one header walk: framing, credentials, Expect and Connection together.
        HeadFacts facts;
        const auto st = net::http::parse_request(c->rbuf, m, HeadReader{c->rbuf, _strip_headers, facts});
        if (st == net::http::FrameStatus::NeedMore)
        {
            if (m.total_len && !c->client_frame_want)
            {
                c->client_frame_want = m.total_len;
                c->rbuf.reserve(m.total_len);
                // No body byte yet: the client is waiting on us, not the network.
                if (c->rbuf.size() <= m.header_len && facts.expect == 2) send_continue(c);
                // A body that needs no rewrite goes upstream as it arrives.
                if (body_streams(c, m))
                {
                    c->client_frame_want = 0;
                    open_request(c, m, facts, t0);
                    if (_sink) sink_capture(c);
                    c->upstream_slot = 0;
                    c->req.f.body_streaming = c->req.f.body_in_rbuf = c->req.f.body_streamed = true;
                    // An OpenAI client's JSON is checked as it passes; a file upload is not JSON.
                    c->req.f.body_scanned = json_media_type(facts.content_type.in(c->rbuf)) &&
                                            client_dialect_of(c->rbuf) == Dialect::OpenAI;
                    if (c->req.f.body_scanned)
                    {
                        if (!c->req.scan) c->req.scan = std::make_unique<provider::BodyScan>();
                        c->req.scan->reset();
                    }
                    ++_stats.streamed_bodies;
                    return dispatch(c);
                }
            }
            return;
        }
        c->client_frame_want = 0;
        open_request(c, m, facts, t0);
        if (st == net::http::FrameStatus::Error) return error_respond(c, 400, "request framing");
        if (!net::http::request_line_ok(c->rbuf)) return error_respond(c, 400, "request line");
        // Nothing here inflates, and every reader downstream assumes JSON.
        if (c->msg.encoded) return error_respond(c, 415, "compressed request body");
        if (_sink) sink_capture(c);
        if (_sink || _policy)
            if (const char* why = capture_model(c)) return error_respond(c, 400, why, kAmbiguousKeysMessage);
        LB_DEBUG(ReqId{c->req.f.req_seq}, " ", request_line(c->rbuf), " on ", *c);
        // The policy seam: framed, but nothing translated, no credential mapped and no
        // upstream acquired, so a refusal reaches no provider. Venue 0 without one.
        c->upstream_slot = 0;
        if (_policy)
        {
            const Decision d = policy_decision(c, m);
            if (!d.allow) return error_respond(c, d.deny_status, d.reason, d.message);
            // Out of range is the default: a venue that has left the table must not send
            // the request somewhere arbitrary.
            if (d.upstream_index >= 0 && static_cast<size_t>(d.upstream_index) < _upstreams.size())
                c->upstream_slot = d.upstream_index;
            else if (d.upstream_index >= 0)
                LB_WARN(ReqId{c->req.f.req_seq}, " policy chose upstream ",
                        static_cast<int64_t>(d.upstream_index), " of ",
                        static_cast<int64_t>(_upstreams.size()), "; using 0");
            // Copied: the views die when decide() returns. Refused rather than cut, which
            // would route to whatever model the prefix names.
            if (!c->req.f.model_override.set(d.model) || !c->req.f.tier_override.set(d.service_tier))
                return error_respond(c, 500, "policy override longer than the gateway holds");
            // A name outside the family would be echoed by a byte-forward: refused, never guessed.
            if (!d.credential_header.empty() && !key_header_ok(d.credential_header))
                return error_respond(c, 400, kWhyKeyUnusable, refuse::kKeyHeaderUnusable);
            c->req.f.key_header.set(d.credential_header);
        }
        dispatch(c);
    }

    void Engine::open_request(ClientConn* c, const net::http::Message& m, const HeadFacts& f,
                              int64_t t0) noexcept
    {
        // The one per-request reset, before any reply: even a 400 carries its own
        // sequence number and none of the previous request's state.
        c->req.begin(g_seq.fetch_add(1, std::memory_order_relaxed));
        c->upstream_slot = -1;
        c->req.f.ts_req_recvd = t0;
        c->msg = m;
        c->req.f.head = f;
        c->req.f.client_upload_ns = span_since(c->ts_first_byte, t0);
        c->req.f.client_conn_reused = c->ever_framed;
        c->req.f.client_conn_setup_ns = c->req.f.client_conn_reused ? 0 : span_since(c->ts_accepted, t0);
        // Whole, its last byte is in, and a pipelining client's next request already here
        // begins now. A streamed body's arrival runs on to its last byte (pump_body).
        if (c->rbuf.size() < m.total_len) return;
        c->req.f.ts_body_end = t0;
        c->ts_first_byte = c->rbuf.size() > m.total_len ? t0 : 0;
    }

    void Engine::dispatch(ClientConn* c) noexcept
    {
        // Resolved per attempt: a failover may land on a venue of another dialect. The
        // client dialect is read off the request line, back in rbuf for a failover.
        const Upstream& up = upstream_of(c);
        {
            const TranslationPlan plan = resolve_dialect(c, up);
            if (!plan.ok) return error_respond(c, 400, plan.why);
            c->req.f.translate_body = plan.translate;
            c->req.f.effective_dialect = plan.venue;
        }
        // A key the policy named is this venue's only credential, or nothing is sent.
        NamedKey key;
        if (!c->req.f.key_header.empty())
            if (const Refusal r = named_key(std::string_view(c->rbuf.data(), c->msg.header_len), c->req.f.head,
                                            c->req.f.key_header.view(), up.dialect, key))
                return error_respond(c, 400, r.why, r.detail);
        // Built before an upstream is acquired, so a bad body cannot leak a pooled one.
        note_build(c->msg.total_len);
        const std::string_view body = body_so_far(c);
        if (c->req.f.translate_body)
        {
            const std::string_view head(c->rbuf.data(), c->msg.header_len);
            const char* why = "";
            if (!build_translated_request(up, c->req.f.effective_dialect, body, head, c->req.f.head,
                                          _rebuild, _xlate, why, c->req.f.model_override.view(),
                                          &c->req.f.wants_usage, key))
            {
                // A credential is never echoed: it would land in a JSON body unescaped.
                if (why[0] == 't') return error_respond(c, 400, "translate", translate_failure(body));
                if (why[0] == 'n') return error_respond(c, 400, "no credential", refuse::kNoCredential);
                return error_respond(c, 400, "malformed credential",
                                     why[0] == 'b' ? refuse::kBedrockCredential : refuse::kCredential);
            }
        }
        else
        {
            // Byte-forward still rebuilds: Host must name the venue. The model is
            // spliced, so every other byte the client sent survives.
            std::string_view rewritten;
            if (!c->req.f.model_override.empty() || !c->req.f.tier_override.empty())
            {
                std::string_view had;
                if (!provider::apply_overrides(body, c->req.f.model_override.view(),
                                               c->req.f.tier_override.view(), &had, _xlate))
                    return error_respond(c, 400, "cannot apply route overrides");
                rewritten = _xlate;
                c->req.f.asked_tier.set(had); // `had` points into rbuf, reused before the sink
            }
            // A streamed body goes as far as it is known good, under the length declared.
            std::string_view fwd = rewritten.empty() ? body : rewritten;
            if (c->req.f.body_streaming)
            {
                if (c->req.f.body_scanned &&
                    c->req.scan->feed(body, body.size() == c->msg.body_len) == provider::BodyScan::Verdict::Refused)
                    return refuse_body(c);
                fwd = body.substr(0, c->req.f.body_scanned ? c->req.scan->safe() : body.size());
                c->req.f.body_sent = static_cast<uint32_t>(fwd.size());
            }
            if (const char* bad = build_forward(std::string_view(c->rbuf.data(), c->msg.header_len),
                                                c->msg.header_len, c->req.f.head, _strip_headers,
                                                up.host_hdr, up.base_path, fwd, _rebuild,
                                                rewritten.empty() ? c->msg.body_len : rewritten.size(), key))
                return error_respond(c, 400, bad);
        }

        c->req.f.phase = Phase::Dispatched; // from here until a response byte, failover may redo it
        UpstreamConn* u = acquire_upstream(c->upstream_slot);
        if (!u)
        {
            secure_clear(_rebuild); // a credential must not wait in the scratch
            // A failover attempt runs on a copy put back in rbuf; drop it, or the next
            // failover inserts a second one.
            if (!c->req.saved.empty()) c->rbuf.erase(0, c->msg.total_len);
            if (!upstream_failed(c, 502, "no upstream (connect failed)"))
                error_respond(c, 502, "no upstream (connect failed)");
            return;
        }

        LB_DEBUG(ReqId{c->req.f.req_seq}, " upstream ", *u, " pooled=", u->from_pool,
                 " dest=", upstream_of(u).ip, ":", upstream_of(u).port);
        // The swap hands the request over and takes the upstream's scrubbed, allocated
        // buffer back as the next scratch.
        u->out.stage().swap(_rebuild);
        // The original bytes, kept when a failover could use them: the rebuilt request
        // is in this venue's dialect.
        if (_policy && _upstreams.size() > 1 && c->req.saved.empty())
        {
            const size_t extra = c->rbuf.size() - c->msg.total_len;
            c->req.saved.swap(c->rbuf);
            c->rbuf.assign(c->req.saved, c->msg.total_len, extra);
            c->req.saved.resize(c->msg.total_len);
        }
        else if (!c->req.f.body_in_rbuf) c->rbuf.erase(0, c->msg.total_len);
        pair(c, u);
        c->ever_framed = true;            // past the setup deadline for good
        c->ts_client_activity = now_ns(); // and the idle clock restarts here
        c->req.f.ts_req_built = now_ns();
        c->req.f.ts_progress = c->req.f.ts_req_built;
        // Per attempt: a failover's venue is not the last one's. Pooled means no handshake.
        c->req.f.ts_wire_ready = u->connected ? c->req.f.ts_req_built : 0;
        c->req.f.ts_up_sent = c->req.f.ts_body_sent = 0;
        c->req.f.upstream_pooled = u->connected;
        c->req.f.upstream_ip = u->up_addr.sin_addr.s_addr;
        mark_first_send(u);

        if (!u->connected) return; // the connect's completion sends it
        send_upstream(u);
    }

    // The staged reply is on the wire: account for it and take the next request.
    void Engine::finish(ClientConn* c) noexcept
    {
        // Only a staged reply finishes. A drained interim 100 Continue, or a write event
        // with nothing owed, is no request: counting one wiped the real one's state.
        if (c->req.f.phase != Phase::Replying) return;
        // An error reply is counted in errors; its stamps are unset, so not in a histogram.
        if (!c->close_after_resp)
        {
            const int64_t ts_resp_sent = now_ns(); // t6
            if (ts_resp_sent - _t_start >= _warmup_ns)
            {
                // The split the timing headers use, so the two cannot disagree; t5 does
                // not enter it, so t4 stands in.
                const TimingSplit sp = split_of(c->req.f, c->req.f.ts_up_recvd);
                const int64_t resp_ns = ts_resp_sent - c->req.f.ts_up_recvd;
                if (sp.req_path_ns >= 0) _stats.req_path.record(static_cast<uint64_t>(sp.req_path_ns));
                if (sp.connect_ns >= 0) _stats.connect.record(static_cast<uint64_t>(sp.connect_ns));
                if (resp_ns >= 0) _stats.resp_path.record(static_cast<uint64_t>(resp_ns));
                if (sp.req_path_ns >= 0 && resp_ns >= 0)
                    _stats.overhead.record(static_cast<uint64_t>(sp.req_path_ns + resp_ns));
                ++_stats.requests;
            }
        }
        if (_sink) sink_emit(c, status_of(c->out.bytes()), /*streamed=*/false);
        LB_DEBUG(ReqId{c->req.f.req_seq}, " status=", status_of(c->out.bytes()),
                 " bytes=", c->out.bytes().size(), " tokens_in=", c->req.f.tok.in,
                 " tokens_out=", c->req.f.tok.out, " keep_alive=", c->msg.keep_alive, " on ", *c);
        c->out.clear();
        c->req.f.phase = Phase::Idle;
        const bool close_now = c->close_after_resp || !c->msg.keep_alive;
        c->msg = net::http::Message{};
        if (close_now && c->req.discard) c->req.close_on_drain = true; // after the body it still sends
        else if (close_now) return _ops->close(*c);
        on_client_bytes(c); // a pipelined next request is framed from rbuf, with no read
    }

    void Engine::respond(ClientConn* client) noexcept
    {
        switch (_ops->send(*client))
        {
            case Sent::Done: finish(client); return;
            case Sent::Failed: client_gone(client); return;
            case Sent::Queued: return; // the completion finishes it
        }
    }

    ConnBase* Engine::forget(ClientConn* c) noexcept
    {
        UpstreamConn* u = unpair(c); // the other leg never keeps a pointer to a freed one
        _clients->remove(c);
        LB_DEBUG("close ", *c, " clients=", _clients->size());
        c->doomed = true;
        _doomed->add(c);
        return u;
    }

    ConnBase* Engine::forget(UpstreamConn* u) noexcept
    {
        unpair(u);
        _pool->remove(*u);
        _upconns->remove(u);
        LB_DEBUG("upstream close ", *u, " pool=", pooled_upstream_count()); // after the erase
        u->doomed = true;
        _doomed->add(u);
        return nullptr;
    }

    ConnBase* Engine::forget(ConnBase& c) noexcept
    {
        return visit(c, [this](auto& k) { return forget(&k); });
    }

    void Engine::abort_pair(ClientConn* client) noexcept
    {
        if (!client) return;
        _ops->close(*client); // and its upstream
        ++_stats.errors;
    }

    void Engine::fail_request(ClientConn* client, int code, const char* why) noexcept
    {
        if (!upstream_failed(client, code, why)) error_respond(client, code, why);
    }

    void Engine::error_respond(ClientConn* client, int code, const char* why,
                                const char* detail) noexcept
    {
        if (!client || client->doomed) return;
        // A client-caused refusal is DEBUG; anything we or the provider caused is WARN.
        // An authentication refusal names where it came from.
        const std::string peer = (code == 401 || code == 403) ? peer_of(client->fd)
                                                              : std::string{};
        LB_WARN(ReqId{client->req.f.req_seq}, " reply ", code, " ", why, " on ", *client,
                peer.empty() ? "" : " peer=", peer);
        // A reply already begun cannot be followed by a second one: close instead.
        if (client->req.f.phase >= Phase::Streaming || !client->out.idle())
        {
            abort_pair(client);
            return;
        }
        // We're replying to the client ourselves, so drop any in-flight upstream.
        if (UpstreamConn* u = unpair(client)) _ops->close(*u);
        append_error(client->out.stage(), code, detail);
        client->req.f.phase = Phase::Replying;
        client->close_after_resp = true; // closes once the reply flushes
        ++_stats.errors;
        respond(client);
    }

} // namespace llmbridge::detail
