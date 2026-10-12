// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// The gateway behind the Gateway facade: configuration, the registries and the one
// request lifecycle (core/), run on a backend (io/) through IoOps.

#include <netinet/in.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/io.hpp"
#include "gateway/gateway.hpp"
#include "net/http.hpp"
#include "net/tls.hpp" // self-guarded by LLMBRIDGE_HAVE_TLS
#include "provider/translate.hpp"

namespace llmbridge
{
    struct ConnBase;
    struct ClientConn;
    struct UpstreamConn;
    class UpstreamPool;
    struct HeadFacts;
    template <class T> class Registry;
}

namespace llmbridge::detail
{
    /// One per worker thread. Internal: the facade, the backends and core/ reach in.
    class Engine
    {
      public:
        Engine(uint16_t listen_port, std::vector<Upstream> upstreams, int64_t warmup_ns, IoBackend io,
               int64_t upstream_idle_ns, TlsConfig tls, bool timing_headers, Policy* policy,
               std::vector<std::string> strip_headers);
        ~Engine();
        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        int run(); // picks the backend; io_uring falls back to epoll
        void print_profile(std::FILE* out, const char* title) const noexcept;
        [[nodiscard]] uint16_t bound_port() const noexcept;
        void set_request_sink(RequestSink* sink, std::vector<std::string> capture);
        [[nodiscard]] bool pooled_buffer_contains(std::string_view needle) const noexcept;
        [[nodiscard]] size_t pooled_upstream_count() const noexcept;
        void set_pool_cap(size_t n) noexcept;
        size_t prefault_resident_bytes_for_test();
        void retire_wbuf(UpstreamConn* u) noexcept;
        void adopt_warm(UpstreamConn* u) noexcept;
        void free_conn(ConnBase* c) noexcept; // closed and unreachable: retire its buffers, delete it
        void note_build(size_t need) noexcept;
        void prefault(std::string& s) const; // reserve `_prefault_bytes` and touch every page

        // The backend seam, attached by a backend's run().
        void attach(IoOps* io, const IoCaps& caps) noexcept;
        IoOps* _ops = nullptr;
        IoCaps _caps{}; ///< the running backend's

        // The request side (core/client.cpp).
        void on_client_bytes(ClientConn* c) noexcept; // frame if idle, then bound what is buffered
        void try_frame(ClientConn* c) noexcept;       // stamps t0, frames one request, dispatches it
        void open_request(ClientConn* c, const net::http::Message& m, const HeadFacts& f,
                          int64_t t0) noexcept;        // its sequence number and arrival facts
        void dispatch(ClientConn* client) noexcept;   // translate or rebuild, acquire, send
        void gate_client_reads(ClientConn* c) noexcept; // the pipelining cap pauses, never closes
        void client_gone(ClientConn* c) noexcept;       // EOF or a socket error on a client
        void send_continue(ClientConn* c) noexcept;     // the interim 100, staged like any reply
        void respond(ClientConn* client) noexcept;      // send what is owed; finish if it all went
        // A body forwarded as it arrives (core/body.cpp).
        [[nodiscard]] bool body_streams(const ClientConn* c, const net::http::Message& m) const noexcept;
        void pump_body(ClientConn* c) noexcept;       // scan, then stage what is safe upstream
        void refuse_body(ClientConn* c) noexcept;     // reset the upstream, answer the 400
        void end_body_stream(ClientConn* c, UpstreamConn* u) noexcept; // a response byte arrived
        void drop_discard(ClientConn* c) noexcept;    // bytes of a request that is over
        [[nodiscard]] std::string_view body_so_far(const ClientConn* c) const noexcept;
        void finish(ClientConn* client) noexcept;       // a staged reply is on the wire
        void abort_pair(ClientConn* client) noexcept;
        void fail_request(ClientConn* client, int code, const char* why) noexcept; // failover, else reply
        /// Reply `code` and abandon any upstream. `why` is for the log; `detail` is what
        /// a 4xx shows the client, never a policy's reason.
        void error_respond(ClientConn* client, int code, const char* why,
                           const char* detail = nullptr) noexcept;
        Decision policy_decision(ClientConn* c, const net::http::Message& m) noexcept;
        /// The bookkeeping half of a close: unpaired, deregistered, doomed. Returns a
        /// client's upstream, which the backend closes too.
        ConnBase* forget(ConnBase& c) noexcept;
        ConnBase* forget(ClientConn* c) noexcept;
        ConnBase* forget(UpstreamConn* u) noexcept;

        // The upstream side (core/upstream.cpp).
        [[nodiscard]] UpstreamConn* new_upstream(int slot) noexcept; // its address parsed, not open
        UpstreamConn* acquire_upstream(int slot) noexcept;         // pooled, else a fresh connect
        bool retry_upstream(UpstreamConn* u) noexcept;             // once, on a fresh connection
        void release_upstream(UpstreamConn* u, bool keep_alive, bool body_ended) noexcept;
        [[nodiscard]] bool pool_upstream(UpstreamConn* u, bool keep_alive, bool body_ended) noexcept;
        bool upstream_failed(ClientConn* client, int status, const char* why) noexcept; // failover
        [[nodiscard]] Retry failover_target(ClientConn* client, int status, const char* why) noexcept;
        enum class UpFail : uint8_t
        {
            Eof,
            Tls,
            Write,
        };
        void upstream_lost(UpstreamConn* u, UpFail f) noexcept; // retry, fail over, else a 502
        void on_upstream_eof(UpstreamConn* u, bool tls_failed) noexcept;
        void on_upstream_bytes(UpstreamConn* u) noexcept; // the response, its head parsed once
        void on_response(UpstreamConn* u, const net::http::ResponseHead& h, std::string_view body,
                         size_t total_len) noexcept;
        static void note_quota(ClientConn* client, const net::http::ResponseHead& h) noexcept;
        static void note_venue_req_id(ClientConn* client, std::string_view head) noexcept;
        /// Read the kernel's RTT to the venue that just answered.
        static void note_upstream_rtt(ClientConn* client, const UpstreamConn* u) noexcept;
        static void note_upstream_error(ClientConn* client, const net::http::ResponseHead& h,
                                        std::string_view body) noexcept;
        [[nodiscard]] const Upstream& upstream_of(const ClientConn* c) const noexcept; // its request's venue
        [[nodiscard]] const Upstream& upstream_of(const UpstreamConn* u) const noexcept;
        [[nodiscard]] const Upstream& venue_at(int slot) const noexcept; // 0 when out of range
        [[nodiscard]] bool upstream_is_tls(const UpstreamConn* u) const noexcept;
        /// Has the request left the machine on every transport it uses? Pooling one
        /// that has not truncates the next client's request (ProxyEarlyResponse).
        [[nodiscard]] bool upstream_request_sent(const UpstreamConn* u) const noexcept;
        /// Everything staged for `c`'s venue is in the kernel: t3, then ts_body_sent.
        static void note_sent(ClientConn* c, int64_t now) noexcept;

        // Streams (core/stream.cpp).
        void begin_stream(UpstreamConn* u, const net::http::ResponseHead& h) noexcept;
        void stream_pump(UpstreamConn* u) noexcept;     // translate new upstream body bytes
        void stream_flush(ClientConn* client) noexcept; // write them; pause, or cap, a slow client
        void stream_on_upstream_eof(UpstreamConn* u) noexcept;
        void finalize_stream(ClientConn* client) noexcept;
        static void stream_warn_if_encoded(const ClientConn* client, const net::http::ResponseHead& h) noexcept;
        void stream_record_latency(const ClientConn* client) noexcept;
        void stream_truncate(ClientConn* client) noexcept; // no [DONE]: the three mutations together

        // The sink (core/sink.cpp) and the sweep (core/sweep.cpp).
        void sink_capture(ClientConn* c) noexcept;
        [[nodiscard]] const char* capture_model(ClientConn* c) noexcept; // why to refuse, or null
        void sink_emit(ClientConn* c, int status, bool streamed) noexcept;
        void sweep_idle() noexcept; // heartbeat, pool reaping and every timeout, one walk
        void sweep_client(ClientConn* c, int64_t now, bool busy) noexcept;

#ifdef LLMBRIDGE_HAVE_TLS
        // Both legs (core/transport.cpp): ConnBase where the two kinds are alike.
        [[nodiscard]] bool tls_required(const ClientConn* c) const noexcept;
        [[nodiscard]] bool tls_required(const UpstreamConn* u) const noexcept;
        [[nodiscard]] bool tls_invariant_ok(ConnBase* c) noexcept; // false: tear it down
        bool tls_attach_upstream(UpstreamConn* u) noexcept;
        bool tls_attach_client(ClientConn* c) noexcept; // false: drop it, never answer in plaintext
        void tls_pump_out(ConnBase* c) noexcept;        // the Session's ciphertext into tls_out
        bool tls_feed(ClientConn* c, const char* p, size_t n) noexcept; // false: fatal
        bool tls_feed(UpstreamConn* u, const char* p, size_t n) noexcept;
        enum class TlsIn : uint8_t
        {
            Failed,
            Fed,
            Handshook, ///< the handshake completed on this feed
        };
        template <class C> TlsIn tls_ingest(C* c, const char* p, size_t n, bool quiet_handshake) noexcept;
        void tls_push_out(ConnBase* c) noexcept;        // unfed plaintext into the Session
        [[nodiscard]] bool tls_out_flushed(const ConnBase* c) const noexcept;
#endif

        // Re-resolution (resolve.cpp).
        void note_connect_failure(int slot, const char* why) noexcept;
        void request_reresolve(int slot) noexcept;
        void reresolve_loop() noexcept;
        void apply_reresolved() noexcept;
        struct Reresolver
        {
            std::thread thread;
            std::mutex mutex;
            std::condition_variable cv;
            std::vector<int> pending;
            std::vector<std::pair<int, std::vector<std::string>>> done;
            bool stop = false;
            std::atomic<bool> ready{false}; ///< `done` is non-empty; the loop's cheap check
        } _rr;
        std::vector<uint8_t> _rr_inflight; ///< per slot: a resolution is outstanding

        /// At most this many venues per request: one dead provider is not a latency
        /// multiplier under a policy that always names another.
        static constexpr int kMaxFailoverAttempts = 3;

        [[nodiscard]] bool any_upstream_tls() const noexcept
        {
            for (const Upstream& u : _upstreams)
                if (u.tls) return true;
            return false;
        }

        uint16_t _listen_port;
        std::vector<Upstream> _upstreams; ///< never empty
        int64_t _warmup_ns;
        int64_t _client_setup_ns = Gateway::kClientSetupNs;
        int64_t _client_idle_ns = Gateway::kDefaultClientIdleNs;
        int64_t _pool_idle_ns = Gateway::kDefaultPoolIdleNs;
        int64_t _connect_ns = Gateway::kDefaultConnectNs;
        int64_t _heartbeat_ns = Gateway::kDefaultHeartbeatNs;
        size_t _prefault_bytes = 0;
        IoBackend _io;
        int64_t _upstream_idle_ns; // 0 = no idle timeout
        TlsConfig _tls;            // both legs; each flag inits its own context
        bool _timing_headers = false;
        /// Forward an OpenAI client's body as it arrives, each chunk scanned first.
        bool _stream_openai_body = true;
        size_t _stream_body_min = 16 * 1024; ///< a body under this arrives too fast to matter
        Policy* const _policy = nullptr; ///< `const`: swapping it under the loop would race
        bool _wants_prefix_hash = false; ///< Policy::wants_prefix_hash(), read once
        RequestSink* _sink = nullptr;
        std::vector<std::string> _sink_capture_names; ///< lowercased, no colon
        uint8_t _active_backend = 0;                  ///< 1 epoll, 2 io_uring; set by run
        bool _uring_active = false;
        /// Dropped from every upstream request: lower-cased with the colon.
        std::vector<std::string> _strip_headers;
#ifdef LLMBRIDGE_HAVE_TLS
        /// Per direction: the upstream context verifies a peer, the client context
        /// presents a certificate.
        net::tls::Context _tls_upstream_ctx;
        net::tls::Context _tls_client_ctx;
#endif
        unsigned _uring_buf_count = 0; // test seams: 0 is the default
        int _client_sndbuf = 0;
        size_t _ep_write_cap = 0;
        int64_t _last_sweep_ns = 0;
        int64_t _last_heartbeat_ns = 0; ///< 0 until the first sweep, which beats at once

        int _listen_fd = -1;
        int64_t _accept_resume_ns = 0; ///< listener paused until then; 0 when accepting
        /// Every connection is in exactly one: live clients, live upstreams (pooled ones
        /// too), or closed and awaiting the free (GATEWAY-INTERNALS.md 7).
        std::unique_ptr<Registry<ClientConn>> _clients;
        std::unique_ptr<Registry<UpstreamConn>> _upconns;
        std::unique_ptr<Registry<ConnBase>> _doomed;
        std::unique_ptr<UpstreamPool> _pool;
        uint64_t _next_client_id = 1;
        /// The rebuilt request, swapped with an upstream's `out` so buffers rotate.
        std::string _rebuild;
        struct WarmSet
        {
            std::string out;
#ifdef LLMBRIDGE_HAVE_TLS
            std::string tls_out;
#endif
        };
        std::vector<WarmSet> _warm; ///< retired upstream buffers, capacity kept
        std::string _xlate;         ///< a translated or rewritten body, on its way to _rebuild

        int64_t _t_start = 0;
        Stats _stats;
        /// Set by other threads, observed by the loop; nothing else depends on them.
        std::atomic<bool> _stop{false};
        std::atomic<bool> _dump{false};
    };
} // namespace llmbridge::detail
