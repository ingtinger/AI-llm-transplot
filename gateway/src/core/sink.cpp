// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The request sink: header capture and the model read at framing, the record at
// completion.

#include "engine.hpp"

#include "core/limits.hpp"
#include "net/sha256.hpp"
#include "request.hpp"
#include "stream.hpp"

#include <cstring>
#include <ctime>
#include <string_view>

namespace llmbridge::detail
{

    // The header names that carry a credential, which is the set this gateway extracts
    // and re-emits: authorization and x-api-key (Bearer / Anthropic), x-goog-api-key
    // (Gemini), api-key (Azure), and the key family.
    static bool is_credential_header(std::string_view lowered) noexcept
    {
        return lowered == "authorization" || lowered == "x-api-key" ||
               lowered == "x-goog-api-key" || lowered == "api-key" ||
               lowered.starts_with(kKeyHeaderPrefix);
    }

    void Engine::set_request_sink(RequestSink* sink, std::vector<std::string> capture)
    {
        _sink = sink;
        _sink_capture_names.clear();
        for (std::string& h : capture)
        {
            if (_sink_capture_names.size() == kSinkCaptureMax) break;
            for (char& ch : h)
                ch = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
            // Checked as find_header will read it, without its colon. One left inside
            // names no header, yet would match into a value.
            while (!h.empty() && (h.back() == ':' || h.back() == ' ' || h.back() == '\t')) h.pop_back();
            if (is_credential_header(h) || h.find(':') != std::string::npos)
            {
                LB_ERROR("sink capture refused for header ", h.c_str(),
                         ": it can carry a credential, which must never reach a sink");
                continue;
            }
            _sink_capture_names.push_back(std::move(h));
        }
    }

    void Engine::sink_capture(ClientConn* c) noexcept
    {
        // The request buffer is reused long before completion, so the sink's header
        // values are copied now, bounded, and the wall clock (the cross-process
        // merge key; the ts_* stamps are monotonic) is taken in the same breath.
        timespec tw{};
        clock_gettime(CLOCK_REALTIME, &tw);
        c->req.f.wall_t0 = static_cast<int64_t>(tw.tv_sec) * 1000000000 + tw.tv_nsec;
        const std::string_view head(c->rbuf.data(), c->msg.header_len);
        for (size_t i = 0; i < kSinkCaptureMax && i < _sink_capture_names.size(); ++i)
            c->req.f.captured[i].set(net::http::find_header(head, _sink_capture_names[i]));
    }

    // The one body read on the byte-forward path, and it happens only with a sink or
    // a policy installed. A stock build parses nothing here, as before.
    // Split out of sink_capture because a policy needs it too and needs it before the
    // decision, while sink_capture's other work is only ever read at the end.
    const char* Engine::capture_model(ClientConn* c) noexcept
    {
        const std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
        // One walk of the top level for everything a policy or the sink reads: a key
        // sent last, as the OpenAI SDK sends them, costs a whole-body walk per read.
        const provider::TopLevelFacts top =
            detail::body_facts(std::string_view(c->rbuf.data(), c->msg.header_len), body);
        c->req.f.asked_stream = top.stream;
        c->req.f.asked_usage = top.include_usage;
        // What a policy or the sink reads here must be what the provider reads. Our
        // readers take the first raw match; a provider unescapes and may take the last.
        switch (top.keys)
        {
            case provider::KeyCheck::Ok: break;
            case provider::KeyCheck::DuplicateKey: return "duplicate top-level key";
            case provider::KeyCheck::EscapedKey: return "escaped top-level key";
            case provider::KeyCheck::TooManyKeys: return "too many top-level keys";
        }
        // A longer name is left out, not cut: a prefix could name another model.
        if (top.model.size() <= sizeof c->req.f.model.b) c->req.f.model.set(top.model);
        if (_wants_prefix_hash && !body.empty())
            c->req.f.prefix_hash = prefix_hash_of(body.substr(0, body.size() < kPrefixHashBytes ? body.size() : kPrefixHashBytes));
        return nullptr;
    }

    uint64_t Engine::prefix_hash_of(std::string_view lead) noexcept
    {
        // noexcept end to end and no allocation, so there is nothing to catch: the path
        // is exception-free by construction
        return net::Sha256::truncated(net::Sha256::hash(lead));
    }

    void Engine::sink_emit(ClientConn* c, int status, bool streamed) noexcept
    {
        RequestRecord r;
        r.seq = c->req.f.req_seq;
        r.wall_t0_ns = c->req.f.wall_t0;
        r.client_upload_ns = c->req.f.client_upload_ns;
        r.client_conn_reused = c->req.f.client_conn_reused;
        r.client_conn_setup_ns = c->req.f.client_conn_setup_ns;
        r.request_bytes = c->msg.total_len;
        r.client_encoded = c->msg.encoded;
        r.ts_req_recvd = c->req.f.ts_req_recvd;
        r.ts_req_built = c->req.f.ts_req_built;
        r.ts_wire_ready = c->req.f.ts_wire_ready;
        r.ts_up_sent = c->req.f.ts_up_sent;
        r.ts_up_recvd = c->req.f.ts_up_recvd;
        r.ts_first_token = c->req.f.ts_first_token;
        r.ts_first_thinking = c->req.f.ts_first_thinking;
        r.max_chunk_gap_ns = c->req.f.max_chunk_gap_ns;
        r.quota_exhausted = c->req.f.quota_exhausted;
        r.retry_after_s = c->req.f.retry_after_s;
        r.ts_done = now_ns();
        r.tag = c->req.f.policy_tag;
        r.status = status;
        r.upstream_index = c->upstream_slot;
        r.upstream_ip = c->req.f.upstream_ip;
        r.upstream_srtt_us = c->req.f.upstream_srtt_us;
        r.upstream_min_rtt_us = c->req.f.upstream_min_rtt_us;
        r.attempts = c->req.f.failover_attempts;
        r.streamed = streamed;
        r.error_reply = !streamed && (c->close_after_resp || c->req.f.reply_kept);
        r.truncated = streamed && c->close_after_resp;
        r.translated = c->req.f.translate_body;
        r.backend = _active_backend;
        r.model = c->req.f.model.view();
        r.asked_tier = c->req.f.asked_tier.view();
        r.upstream_error = c->req.f.upstream_error.view();
        r.venue_req_id = c->req.f.venue_req_id.view();
        r.prefix_hash = c->req.f.prefix_hash;
        r.served_tier = c->req.f.served_tier.view();
        r.served_model = c->req.f.served_model.view();
        r.from_pool = c->req.f.upstream_pooled;
        // A stream's counts from its translator or its own usage events; -1 is "not
        // reported" and not "zero".
        const BodyUsage u = streamed ? stream_tokens(c) : c->req.f.tok;
        r.tokens_in = static_cast<int32_t>(u.in);
        r.tokens_out = static_cast<int32_t>(u.out);
        r.cached_tokens = static_cast<int32_t>(u.cached);
        r.cache_write_tokens = static_cast<int32_t>(u.cache_write);
        r.cache_write_5m_tokens = static_cast<int32_t>(u.cache_write_5m);
        r.cache_write_1h_tokens = static_cast<int32_t>(u.cache_write_1h);
        r.reasoning_tokens = static_cast<int32_t>(u.reasoning);
        r.audio_in_tokens = static_cast<int32_t>(u.audio_in);
        r.audio_out_tokens = static_cast<int32_t>(u.audio_out);
        r.accepted_prediction_tokens = static_cast<int32_t>(u.accepted_prediction);
        r.rejected_prediction_tokens = static_cast<int32_t>(u.rejected_prediction);
        r.tool_prompt_tokens = static_cast<int32_t>(u.tool_prompt);
        for (size_t i = 0; i < kSinkCaptureMax; ++i)
            r.captured[i] = c->req.f.captured[i].view();
        r.ts_body_end = c->req.f.ts_body_end;
        r.ts_body_sent = c->req.f.ts_body_sent;
        r.body_work_ns = c->req.f.body_work_ns;
        r.body_streamed = c->req.f.body_streamed;
        r.decided_on_head = c->req.f.decided_on_head;
        r.confirm_outcome = c->req.f.confirm_outcome;
        _sink->on_request(r);
    }

} // namespace llmbridge::detail
