// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// What a venue said about a response (limits, its own id, its name for a failure),
// and where a request goes after its venue failed. Shared by both backends.

#include "gateway/gateway.hpp"

#include "core/io.hpp"
#include "core/limits.hpp"
#include "core/registry.hpp"
#include "net/socket_util.hpp" // tcp_rtt
#include "scan.hpp"

#include <cstring>
#include <string_view>

namespace llmbridge
{
    using namespace detail;

    // What the provider said about its own limits, kept from the response head.
    // Shared by both backends and by the streaming and non-streaming paths, so the
    // four cannot disagree about what a refusal meant.
    void Gateway::note_quota(Connection* client, const net::http::ResponseHead& h) noexcept
    {
        client->req.f.quota_exhausted = static_cast<uint8_t>(h.quota_exhausted);
        client->req.f.retry_after_s = h.retry_after_s;
    }

    // Bedrock is first: a Claude-via-Bedrock response carries x-amzn and
    // Anthropic's own id together, and the AWS one is what CloudTrail indexes.
    void Gateway::note_venue_req_id(Connection* client, std::string_view head) noexcept
    {
        client->req.f.venue_req_id.set({});
        static constexpr std::string_view kNames[] = {"x-amzn-requestid", "request-id",
                                                      "x-request-id"};
        for (const std::string_view name : kNames)
        {
            const std::string_view v = net::http::find_header(head, name);
            if (v.empty()) continue;
            client->req.f.venue_req_id.set(v);
            return;
        }
    }

    void Gateway::note_upstream_rtt(Connection* client, const Connection* u) noexcept
    {
        // At the head, when the venue has acknowledged the whole request, and on the
        // socket under TLS. One getsockopt, after t4 is stamped so it is not inside it.
        const net::TcpRtt rtt = net::tcp_rtt(u->fd);
        client->req.f.upstream_srtt_us = rtt.srtt_us;
        client->req.f.upstream_min_rtt_us = rtt.min_us;
    }

    void Gateway::note_upstream_error(Connection* client, const net::http::ResponseHead& h,
                                      std::string_view body) noexcept
    {
        client->req.f.upstream_error.set({});
        // 0 is a head we never framed, and a 3xx is not the venue naming a failure.
        if (h.status < 400) return;
        client->req.f.upstream_error.set(scan_error_type(body));
    }

    bool Gateway::pool_upstream(Connection* u, bool keep_alive, bool body_ended) noexcept
    {
        using R = UpstreamPool::Refusal;
        switch (_pool->release(*u, keep_alive, body_ended, upstream_request_sent(u), now_ns()))
        {
            case R::None:
                return true;
            case R::Unsent:
                // The provider answered before our request finished going out, so it
                // saw a truncated request: GATEWAY-INTERNALS.md 8.
                LB_WARN("closing instead of pooling, request was still going out ", *u);
                ++_stats.upstream_unsent;
                return false;
            case R::Full:
                LB_WARN("CAP pool full, closing instead of pooling ", *u,
                        " limit=", static_cast<uint64_t>(_pool->cap()), " (reuse stops here)");
                return false;
            default:
                return false;
        }
    }

    Connection* Gateway::new_upstream(int slot) noexcept
    {
        Connection* u = new Connection();
        u->is_client = false;
        u->upstream_slot = slot;   // the pool, the venue and the TLS name all index this
        u->ts_accepted = now_ns(); // the connect deadline counts from here
        u->rbuf.reserve(kInitialBuf);
        return u;
    }

    Connection* Gateway::acquire_upstream(int slot) noexcept
    {
        // Only this venue's pool: a connection to one provider cannot serve a request
        // bound for another, and handing one over would send the request, and its
        // credential, to the wrong company.
        if (Connection* u = _pool->acquire(slot))
        {
            u->from_pool = true; // reused -> a pre-response failure is retry-eligible
            u->ts_pool_taken = now_ns();
            u->retried = false;  // fresh request: one retry available again
            ++_stats.upstream_reused;
            return u;
        }
        Connection* u = new_upstream(slot);
        adopt_warm(u);
#ifdef LLMBRIDGE_HAVE_TLS
        if (upstream_of(u).tls && !tls_attach_upstream(u))
        {
            delete u;
            return nullptr;
        }
#endif
        if (!_ops->connect(*u))
        {
            delete u;
            return nullptr;
        }
        _upconns->add(u);
        ++_stats.upstream_conns_opened;
        // Pairs with "upstream reuse": without it an upstream first appears in
        // the log at its second request, never at its birth.
        LB_DEBUG("upstream open ", *u, " to ", upstream_of(u).ip, " pool=", pooled_upstream_count());
        return u;
    }

    bool Gateway::retry_upstream(Connection* u) noexcept
    {
        // A pooled reuse, not yet retried, that failed before any response byte: the
        // provider almost certainly dropped it idle without running the request.
        if (!u->from_pool || u->retried) return false;
        if (now_ns() - u->ts_pool_taken > kStaleRetryWindowNs) return false; // may have run
        Connection* client = u->peer;
        if (!client || !client->req.can_redispatch()) return false; // a response byte arrived
        // The same venue: a retry that lands elsewhere is a silent reroute, and the
        // request was translated for this dialect and carries its credential.
        Connection* uf = new_upstream(u->upstream_slot);
        uf->retried = true; // this request's one allowed retry is now spent
#ifdef LLMBRIDGE_HAVE_TLS
        if (upstream_of(uf).tls && !tls_attach_upstream(uf))
        {
            delete uf;
            return false;
        }
#endif
        if (!_ops->connect(*uf))
        {
            delete uf;
            return false;
        }
        client->req.f.upstream_ip = uf->up_addr.sin_addr.s_addr;
        // Plaintext, re-pushed through the new session; a send still reading it gets a copy.
        uf->out.stage() = u->out.take();
        _upconns->add(uf);
        ++_stats.upstream_conns_opened;
        ++_stats.upstream_retries;
        // WARN: a recovered failure the client never learns of. A rising rate means the
        // provider drops keep-alives faster than --pool-idle reaps them.
        LB_WARN("upstream stale, resending on a fresh connection ", *u);
        _ops->close(*u); // discard the dead connection; unpairs it
        pair(client, uf);
        return true;
    }

    void Gateway::release_upstream(Connection* u, bool keep_alive, bool body_ended) noexcept
    {
        if (!pool_upstream(u, keep_alive, body_ended))
        {
            _ops->close(*u);
            return;
        }
        // A slow client may have paused this upstream's reads, and a pooled connection
        // must be readable or the next response is never seen. The pool's only door.
        if (_stream_backpressure) _ops->set_read(*u, true);
    }

    bool Gateway::upstream_failed(Connection* client, int status, const char* why) noexcept
    {
        const Retry r = failover_target(client, status, why);
        if (!r.retry) return false;

        LB_WARN(ReqId{client->req.f.req_seq}, " venue ", static_cast<int64_t>(client->upstream_slot),
                " failed (", why, "); retrying on ", static_cast<int64_t>(r.upstream_index));
        if (Connection* u = unpair(client)) _ops->close(*u);
        ++client->req.f.failover_attempts;
        client->upstream_slot = r.upstream_index;
        // The original request back at the front of rbuf. The forward rebuilds it for the
        // new venue's dialect and erases exactly these bytes again, so a pipelined
        // request queued behind it keeps its place.
        client->rbuf.insert(0, client->req.saved);
        ++_stats.upstream_failovers;
        _ops->forward(*client);
        return true;
    }

    Retry Gateway::failover_target(Connection* client, int status, const char* why) noexcept
    {
        // Every precondition here is about safety, not policy. Re-sending a request the
        // client has already begun receiving would duplicate output; re-sending one we
        // no longer hold is impossible; and an unbounded chain turns one dead provider
        // into a latency multiplier.
        if (!_policy || _upstreams.size() < 2) return {};
        if (!client || client->doomed) return {};
        // No response byte may have arrived, or the venue may have acted on the request,
        // and nothing may be owed to the client, or a re-send duplicates output.
        if (!client->req.can_redispatch() || !client->out.idle()) return {};
        if (client->req.saved.empty()) return {};
        if (client->req.f.failover_attempts >= kMaxFailoverAttempts - 1) return {};

        const FailureFacts f{client->upstream_slot, status, why, client->req.f.failover_attempts,
                             client->req.f.policy_tag};
        const Retry r = _policy->on_failure(f);
        if (!r.retry) return {};
        if (r.upstream_index < 0 || static_cast<size_t>(r.upstream_index) >= _upstreams.size())
        {
            LB_WARN(ReqId{client->req.f.req_seq}, " failover to out-of-range upstream ",
                    static_cast<int64_t>(r.upstream_index), "; giving up");
            return {};
        }
        // Sending it back to the venue that just failed is the one answer that cannot
        // help, and it is how a policy accidentally writes an infinite loop.
        if (r.upstream_index == client->upstream_slot)
        {
            LB_WARN(ReqId{client->req.f.req_seq}, " failover named the venue that just failed; "
                                            "giving up");
            return {};
        }
        // The new venue's overrides, never the failed one's: its model name is wrong for
        // another venue. Copied now, since the views die once on_failure has returned.
        if (!client->req.f.model_override.set(r.model) ||
            !client->req.f.tier_override.set(r.service_tier))
        {
            LB_WARN(ReqId{client->req.f.req_seq}, " failover override longer than the gateway "
                                                  "holds; giving up");
            return {};
        }
        return r;
    }

} // namespace llmbridge
