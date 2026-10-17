// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// A hedged request (GATEWAY-INTERNALS.md 13): each leg is a request of its own on a client
// connection with no socket, run by the same lifecycle as any other, and cut once the
// decision's own leg has its answer.

#include "engine.hpp"

#include "core/io.hpp"
#include "core/limits.hpp"
#include "core/registry.hpp"

#include <algorithm>
#include <iterator>

namespace llmbridge::detail
{
    // Every leg is checked before any is sent: a bad one is the policy's 500, and no venue
    // has heard of the request.
    bool Engine::apply_hedge(ClientConn* c, const Decision& d) noexcept
    {
        ReqState& f = c->req.f;
        f.hedge_n = 0;
        if (d.hedge_legs == 0) return true;
        const char* bad = nullptr;
        if (!d.hedge || d.hedge_legs > kMaxHedgeLegs) bad = "policy hedge unusable";
        // One client key would otherwise reach every venue, each another company's.
        else if (d.credential_header.empty()) bad = "policy hedge names no key for its own venue";
        else if (!d.substitute_model.empty()) bad = "policy hedge with a model substitution";
        for (uint8_t i = 0; !bad && i < d.hedge_legs; ++i)
        {
            const HedgeLeg& l = d.hedge[i];
            HedgeLegCopy& k = f.hedge[i];
            k.venue = l.upstream_index;
            if (l.upstream_index < 0 || static_cast<size_t>(l.upstream_index) >= _upstreams.size())
                bad = "policy hedge leg past the table";
            else if (!key_header_ok(l.credential_header))
                bad = "policy hedge leg names no usable key";
            else if (!l.anthropic_beta.empty() && !beta_token_ok(l.anthropic_beta))
                bad = "policy hedge leg anthropic_beta unusable";
            else if (!k.model.set(l.model) || !k.tier.set(l.service_tier) || !k.speed.set(l.speed) ||
                     !k.geo.set(l.inference_geo))
                bad = "policy hedge leg override longer than the gateway holds";
            k.beta.set(l.anthropic_beta);
            k.key.set(l.credential_header);
        }
        if (bad) return error_respond(c, 500, bad), false;
        f.hedge_n = d.hedge_legs;
        return true;
    }

    // Each leg gets its own copy of the request, as framed, and is decided and dispatched as
    // any request is: its own translation, overrides and key, and only its own key.
    bool Engine::spawn_hedge(ClientConn* c) noexcept
    {
        ReqState& f = c->req.f;
        // Held to its last byte by construction (decide_on_head, confirm_body); a leg given
        // part of a body would run a request nobody sent, so this is refused, never sent.
        if (c->rbuf.size() < c->msg.total_len)
        {
            LB_ERROR(ReqId{f.req_seq}, " hedge reached dispatch with its body incomplete; refusing");
            f.hedge_n = 0;
            error_respond(c, 500, "hedge without the whole body");
            return false;
        }
        f.hedged = true;
        f.hedge_group = f.req_seq;
        f.hedge_won = true; // the decision's own leg is the one that serves the client
        ++_stats.hedged_requests;
        for (uint8_t i = 0; i < f.hedge_n; ++i)
        {
            auto* s = new ClientConn();
            s->shadow = true;
            s->hedge_of = c;
            c->hedge_legs[i] = s;
            s->id = c->id;
            s->ever_framed = true;
            s->ts_accepted = c->ts_accepted;
            s->ts_client_activity = now_ns();
            s->msg = c->msg;
            s->rbuf.assign(c->rbuf, 0, c->msg.total_len);
            s->req.f = f; // the same arrival: its stamps, head facts, captures and model
            ReqState& sf = s->req.f;
            sf.req_seq = g_seq.fetch_add(1, std::memory_order_relaxed);
            sf.hedge_leg = static_cast<uint8_t>(i + 1);
            sf.hedge_n = 0; // a leg has no legs
            sf.hedge_won = false;
            _clients->add(s);
            ++_stats.hedge_legs;
            const HedgeLegCopy& l = f.hedge[i];
            Decision d{};
            d.allow = true;
            d.upstream_index = l.venue;
            d.model = l.model.view();
            d.service_tier = l.tier.view();
            d.speed = l.speed.view();
            d.inference_geo = l.geo.view();
            d.anthropic_beta = l.beta.view();
            d.credential_header = l.key.view();
            d.tag = f.policy_tag;
            if (apply_decision(s, d)) dispatch(s);
        }
        return true;
    }

    void Engine::settle_hedge(ClientConn* c) noexcept
    {
        c->req.f.hedge_n = 0;
        ClientConn* legs[kMaxHedgeLegs];
        std::copy(std::begin(c->hedge_legs), std::end(c->hedge_legs), legs); // each cut unlinks itself
        for (ClientConn* s : legs)
            if (s) cut_leg(s);
    }

    void Engine::cut_leg(ClientConn* s) noexcept
    {
        s->req.f.hedge_cut = true;
        ++_stats.hedge_legs_cut;
        LB_DEBUG(ReqId{s->req.f.req_seq}, " hedge leg cut on venue ", static_cast<int64_t>(s->upstream_slot));
        // Mid-request: never pooled, and a reset is how the venue learns to stop.
        if (s->up) reset_venue(s->up);
        _ops->close(*s);
    }

    void Engine::close_shadow(ClientConn* s) noexcept
    {
        if (s->doomed) return;
        // Every leg leaves a record, whatever ended it; a cut one was answered nothing.
        ReqState& f = s->req.f;
        if (_sink && !f.sunk && f.phase != Phase::Idle)
            sink_emit(s, f.hedge_cut ? 0 : f.streaming ? 200 : 502, f.streaming);
        if (ConnBase* u = forget(s)) _shadow_io.inner->close(*u);
    }

    void Engine::bury_shadows() noexcept
    {
        for (ClientConn* s : _dead_shadows) delete s;
        _dead_shadows.clear();
    }

} // namespace llmbridge::detail
