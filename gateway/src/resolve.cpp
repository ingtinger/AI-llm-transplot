// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Moving a venue to its next address after a failed connect, and resolving its name off
// the loop thread: after a failure, and for a venue whose name has no address yet.

#include "engine.hpp"

#include "core/limits.hpp"
#include "net/socket_util.hpp"
#include "request.hpp"

#include <netinet/in.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "net/upstream.hpp"

namespace llmbridge::detail
{

    bool Engine::has_address(const Upstream& up) noexcept
    {
        // The parse new_upstream dials with, so the two cannot disagree.
        sockaddr_in a{};
        return net::resolve_ipv4(up.ip.c_str(), up.port, a);
    }

    void Engine::readdress(Upstream& up, const std::string& ip) noexcept
    {
        try
        {
            up.ip = ip;
            // The bare form's Host names the address dialled, so it follows the address.
            if (up.sni_host.empty()) up.host_hdr = host_header_for(up);
        }
        catch (...)
        {
            // Out of memory: the address in use, and its Host, stand.
        }
    }

    void Engine::note_connect_failure(int slot, const char* why) noexcept
    {
        if (slot < 0 || static_cast<size_t>(slot) >= _upstreams.size()) return;
        Upstream& up = _upstreams[static_cast<size_t>(slot)];
        const std::string failed = up.ip;
        if (up.ips.size() > 1)
        {
            const auto it = std::find(up.ips.begin(), up.ips.end(), up.ip);
            const size_t i = it == up.ips.end() ? 0 : static_cast<size_t>(it - up.ips.begin());
            readdress(up, up.ips[(i + 1) % up.ips.size()]);
        }
        if (up.ip != failed)
            LB_WARN("venue ", static_cast<int64_t>(slot), " connect ", why, " at ", failed, ":",
                    static_cast<int64_t>(up.port), "; next address ", up.ip);
        else
            LB_WARN("venue ", static_cast<int64_t>(slot), " connect ", why, " at ", failed, ":",
                    static_cast<int64_t>(up.port));
        request_reresolve(slot);
    }

    void Engine::request_reresolve(int slot) noexcept
    {
        if (slot < 0 || static_cast<size_t>(slot) >= _upstreams.size()) return;
        const auto i = static_cast<size_t>(slot);
        if (_rr_inflight[i] || _upstreams[i].host.empty()) return;
        // Asked lately: a name is never looked up at the rate requests fail.
        if (now_ns() < _rr_next_ns[i]) return;
        try
        {
            std::lock_guard<std::mutex> g(_rr->mutex);
            if (!_rr_thread.joinable()) _rr_thread = std::thread([rr = _rr] { reresolve_loop(rr); });
            _rr->pending.emplace_back(slot, _upstreams[i].host);
        }
        catch (...)
        {
            LB_WARN("venue ", static_cast<int64_t>(slot), " re-resolution could not be queued");
            return;
        }
        _rr_inflight[i] = 1;
        _rr->cv.notify_one();
    }

    void Engine::resolve_unaddressed() noexcept
    {
        for (size_t i = 0; i < _upstreams.size(); ++i)
            if (!_upstreams[i].host.empty() && !has_address(_upstreams[i]))
                request_reresolve(static_cast<int>(i));
    }

    // The resolver thread's whole world is `rr`, never the engine: the name travels with
    // the job, and the answer goes back the same way.
    void Engine::reresolve_loop(const std::shared_ptr<Reresolver>& rr) noexcept
    {
        for (;;)
        {
            std::pair<int, std::string> job;
            {
                std::unique_lock<std::mutex> lk(rr->mutex);
                rr->cv.wait(lk, [&rr] { return rr->stop || !rr->pending.empty(); });
                if (rr->stop) return;
                job = std::move(rr->pending.front());
                rr->pending.erase(rr->pending.begin());
                rr->busy = true;
            }
            Reresolver::Done d;
            d.slot = job.first;
            try
            {
                d.ips = rr->resolve(job.second, &d.err);
            }
            catch (...)
            {
                // Out of memory: an empty answer, so the venue is asked again later
                // instead of waiting forever on a lookup that never reports.
                d.ips.clear();
            }
            std::lock_guard<std::mutex> g(rr->mutex);
            rr->busy = false;
            rr->done.push_back(std::move(d)); // within the capacity reserved for every venue
            rr->ready.store(true, std::memory_order_release);
        }
    }

    void Engine::apply_reresolved() noexcept
    {
        if (!_rr->ready.load(std::memory_order_acquire)) return;
        {
            std::lock_guard<std::mutex> g(_rr->mutex);
            _rr_batch.swap(_rr->done);
            _rr->ready.store(false, std::memory_order_relaxed);
        }
        const int64_t now = now_ns();
        for (Reresolver::Done& d : _rr_batch)
        {
            const auto i = static_cast<size_t>(d.slot);
            _rr_inflight[i] = 0;
            Upstream& up = _upstreams[i];
            const bool had = has_address(up);
            if (d.ips.empty())
            {
                // Twice as long each time, so a dead name costs a lookup a half minute at
                // worst, and one line per outage: the retries say nothing new.
                const int64_t was = _rr_backoff_ns[i];
                _rr_backoff_ns[i] = was == 0 ? kReresolveMinNs : std::min(2 * was, kReresolveMaxNs);
                _rr_next_ns[i] = now + _rr_backoff_ns[i];
                if (was == 0)
                    LB_WARN("venue ", static_cast<int64_t>(d.slot), " resolving ", up.host, " failed (",
                            d.err.empty() ? std::string_view("no address") : std::string_view(d.err), "); ",
                            had ? "keeping " : "it has no address yet",
                            had ? std::string_view(up.ip) : std::string_view(""));
                continue;
            }
            _rr_backoff_ns[i] = 0;
            _rr_next_ns[i] = now + kReresolveMinNs;
            const bool changed = d.ips != up.ips;
            up.ips = std::move(d.ips);
            // The rotation already moved off the failed address; keep that choice when
            // the fresh list still has it, else start the fresh list from the top.
            if (std::find(up.ips.begin(), up.ips.end(), up.ip) == up.ips.end()) readdress(up, up.ips.front());
            ++_stats.upstream_reresolved;
            LB_INFO("venue ", static_cast<int64_t>(d.slot), " resolved ", up.host, ": ",
                    static_cast<int64_t>(up.ips.size()), " addresses",
                    changed ? "" : " (unchanged)", ", using ", up.ip, had ? "" : " (it had none)");
        }
        _rr_batch.clear();
    }

} // namespace llmbridge::detail
