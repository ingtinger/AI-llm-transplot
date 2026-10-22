// AI-llm-transplot 扩展：工作线程只更新原子计数和尝试入队，日志写盘由专用线程执行。

#include "observability.hpp"
#include "audit_ledger.hpp"
#include "routing_policy.hpp"

#include <cstdio>

namespace llmbridge::app
{
    Observability::Observability(size_t upstream_count, bool request_log, RoutingPolicy* policy,
                                 AuditLedger* audit)
        : upstream_count_(upstream_count),
          policy_(policy),
          audit_(audit),
          latencies_(std::make_unique<UpstreamLatency[]>(upstream_count)),
          request_log_(request_log)
    {
        if (request_log_) log_thread_ = std::thread([this] { log_loop(); });
    }

    void Observability::on_attempt(const AttemptRecord& r) noexcept
    {
        if (policy_) policy_->complete_attempt(r);
        if (audit_) audit_->on_attempt(r);
    }

    Observability::~Observability()
    {
        if (!request_log_) return;
        {
            const std::lock_guard<std::mutex> lock(log_mutex_);
            stopping_ = true;
        }
        log_cv_.notify_one();
        log_thread_.join();
    }

    void Observability::on_request(const RequestRecord& r) noexcept
    {
        if (policy_) policy_->complete(r);
        if (r.upstream_index >= 0 && static_cast<size_t>(r.upstream_index) < upstream_count_ &&
            r.ts_up_sent > 0 && r.ts_up_recvd >= r.ts_up_sent)
        {
            UpstreamLatency& v = latencies_[static_cast<size_t>(r.upstream_index)];
            v.count.fetch_add(1, std::memory_order_relaxed);
            v.total_ns.fetch_add(static_cast<uint64_t>(r.ts_up_recvd - r.ts_up_sent),
                                 std::memory_order_relaxed);
        }
        if (!request_log_) return;

        LogEntry entry{.request_id = r.seq,
                       .initial_upstream = (r.tag & 0xffffffffULL) ?
                                           static_cast<int>((r.tag & 0xffffffffULL) - 1) : -1,
                       .final_upstream = r.upstream_index,
                       .status = r.status,
                       .attempts = r.attempts + 1,
                       .duration_us = r.ts_done > r.ts_req_recvd
                                          ? (r.ts_done - r.ts_req_recvd) / 1000 : 0,
                       .streamed = r.streamed,
                       .user_index = (r.tag >> 32) ? static_cast<int>((r.tag >> 32) - 1) : -1,
                       .tokens_in = r.tokens_in,
                       .tokens_out = r.tokens_out,
                       .venue_cost_pdollars = r.venue_cost_pdollars};
        std::unique_lock<std::mutex> lock(log_mutex_, std::try_to_lock);
        if (!lock.owns_lock() || log_count_ == kLogCapacity)
        {
            dropped_logs_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        log_queue_[log_tail_] = entry;
        log_tail_ = (log_tail_ + 1) % kLogCapacity;
        ++log_count_;
        lock.unlock();
        log_cv_.notify_one();
    }

    Observability::Latency Observability::latency(size_t index) const noexcept
    {
        if (index >= upstream_count_) return {};
        const UpstreamLatency& v = latencies_[index];
        return {v.count.load(std::memory_order_relaxed),
                v.total_ns.load(std::memory_order_relaxed)};
    }

    uint64_t Observability::dropped_logs() const noexcept
    {
        return dropped_logs_.load(std::memory_order_relaxed);
    }

    bool Observability::audit_healthy() const noexcept { return !audit_ || audit_->healthy(); }
    uint64_t Observability::audit_written() const noexcept { return audit_ ? audit_->written() : 0; }

    void Observability::log_loop() noexcept
    {
        for (;;)
        {
            LogEntry entry;
            {
                std::unique_lock<std::mutex> lock(log_mutex_);
                log_cv_.wait(lock, [this] { return stopping_ || log_count_ > 0; });
                if (log_count_ == 0 && stopping_) return;
                entry = log_queue_[log_head_];
                log_head_ = (log_head_ + 1) % kLogCapacity;
                --log_count_;
            }
            // 全部字段都是网关生成的数字，不包含请求头、密钥或聊天正文。
            std::fprintf(stderr,
                         "{\"event\":\"request\",\"request_id\":\"%llu\","
                         "\"initial_upstream\":%d,\"final_upstream\":%d,"
                         "\"status\":%d,\"attempts\":%d,\"duration_us\":%lld,"
                         "\"streamed\":%s,\"user_index\":%d,\"tokens_in\":%d,"
                         "\"tokens_out\":%d,\"venue_cost_pdollars\":%lld}\n",
                         static_cast<unsigned long long>(entry.request_id),
                         entry.initial_upstream, entry.final_upstream, entry.status,
                         entry.attempts, static_cast<long long>(entry.duration_us),
                         entry.streamed ? "true" : "false", entry.user_index,
                         entry.tokens_in, entry.tokens_out,
                         static_cast<long long>(entry.venue_cost_pdollars));
        }
    }
} // namespace llmbridge::app
