// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The Gateway facade: every call forwards to the Engine.

#include "gateway/gateway.hpp"

#include "engine.hpp"
#include "provider/json.hpp"

#include <utility>

namespace llmbridge
{
    // The single-upstream form: outbound TLS arrives through TlsConfig here, where the
    // table form takes it per upstream.
    Gateway::Gateway(uint16_t listen_port, std::string upstream_ip, uint16_t upstream_port,
                     int64_t warmup_ns, UpstreamDialect dialect, IoBackend io,
                     int64_t upstream_idle_ns, TlsConfig tls, bool timing_headers,
                     Policy* policy, std::vector<std::string> strip_headers)
        : Gateway(listen_port,
                  std::vector<Upstream>{Upstream{.ip = std::move(upstream_ip),
                                                 .port = upstream_port,
                                                 .tls = tls.upstream_tls,
                                                 .sni_host = tls.sni_host,
                                                 .dialect = dialect}},
                  warmup_ns, io, upstream_idle_ns, tls, timing_headers, policy,
                  std::move(strip_headers))
    {
    }

    Gateway::Gateway(uint16_t listen_port, std::vector<Upstream> upstreams, int64_t warmup_ns,
                     IoBackend io, int64_t upstream_idle_ns, TlsConfig tls, bool timing_headers,
                     Policy* policy, std::vector<std::string> strip_headers)
        : _e(std::make_unique<detail::Engine>(listen_port, std::move(upstreams), warmup_ns, io,
                                              upstream_idle_ns, std::move(tls), timing_headers,
                                              policy, std::move(strip_headers)))
    {
    }

    Gateway::~Gateway() = default;

    int Gateway::run() { return _e->run(); }
    void Gateway::request_stop() noexcept { _e->_stop.store(true, std::memory_order_relaxed); }
    void Gateway::request_stats_dump() noexcept { _e->_dump.store(true, std::memory_order_relaxed); }
    void Gateway::print_profile(std::FILE* out, const char* title) const noexcept { _e->print_profile(out, title); }
    const Stats& Gateway::stats() const noexcept { return _e->_stats; }
    IoBackend Gateway::io_backend() const noexcept { return _e->_io; }
    uint16_t Gateway::bound_port() const noexcept { return _e->bound_port(); }

    void Gateway::set_client_setup_ns(int64_t ns) noexcept { _e->_client_setup_ns = ns; }
    void Gateway::set_client_idle_ns(int64_t ns) noexcept { _e->_client_idle_ns = ns; }
    void Gateway::set_pool_idle_ns(int64_t ns) noexcept { _e->_pool_idle_ns = ns; }
    void Gateway::set_connect_ns(int64_t ns) noexcept { _e->_connect_ns = ns; }
    void Gateway::set_heartbeat_ns(int64_t ns) noexcept { _e->_heartbeat_ns = ns; }
    void Gateway::set_prefault_bytes(size_t n) noexcept { _e->_prefault_bytes = n; }
    void Gateway::set_stream_openai_body(bool on) noexcept { _e->_stream_openai_body = on; }
    void Gateway::set_stream_body(bool on) noexcept { _e->_stream_body = on; }
    void Gateway::set_stream_body_max(size_t n) noexcept { _e->_stream_body_max = n; }
    void Gateway::set_retain_bytes(size_t n) noexcept { provider::json::set_retain_bytes(n); }
    void Gateway::set_stream_body_min_for_test(size_t n) noexcept { _e->_stream_body_min = n; }
    void Gateway::set_request_sink(RequestSink* sink, std::vector<std::string> capture)
    {
        _e->set_request_sink(sink, std::move(capture));
    }

    bool Gateway::pooled_buffer_contains(std::string_view needle) const noexcept
    {
        return _e->pooled_buffer_contains(needle);
    }
    size_t Gateway::pooled_upstream_count() const noexcept { return _e->pooled_upstream_count(); }
    void Gateway::set_pool_cap_for_test(size_t n) noexcept { _e->set_pool_cap(n); }
    std::string_view Gateway::upstream_sni_host(size_t i) const noexcept
    {
        return i < _e->_upstreams.size() ? std::string_view(_e->_upstreams[i].sni_host) : std::string_view{};
    }
    void Gateway::set_uring_buf_count_for_test(unsigned n) noexcept { _e->_uring_buf_count = n; }
    void Gateway::set_client_sndbuf_for_test(int bytes) noexcept { _e->_client_sndbuf = bytes; }
    void Gateway::set_upstream_sndbuf_for_test(int bytes) noexcept { _e->_upstream_sndbuf = bytes; }
    void Gateway::set_epoll_write_cap_for_test(size_t bytes) noexcept { _e->_ep_write_cap = bytes; }
    size_t Gateway::prefault_resident_bytes_for_test() { return _e->prefault_resident_bytes_for_test(); }
    void Gateway::retire_wbuf(UpstreamConn* u) noexcept { _e->retire_wbuf(u); }
    void Gateway::adopt_warm(UpstreamConn* u) noexcept { _e->adopt_warm(u); }
    void Gateway::note_build(size_t need) noexcept { _e->note_build(need); }
    size_t Gateway::warm_bufs_for_test() const noexcept { return _e->_warm.size(); }
} // namespace llmbridge
