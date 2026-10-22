// AI-llm-transplot 扩展：跨工作线程的请求统计与有界异步访问日志。

#pragma once

#include "gateway/sink.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace llmbridge::app
{
    class AuditLedger;
    class RoutingPolicy;
    /// 多个 Gateway 共享的请求接收器；计数使用原子操作，日志入队不等待锁。
    /// 必须在所有 Gateway 工作线程结束后销毁，析构时排空已入队日志。
    class Observability final : public RequestSink
    {
      public:
        struct Latency
        {
            uint64_t count = 0;
            uint64_t total_ns = 0;
        };

        Observability(size_t upstream_count, bool request_log, RoutingPolicy* policy = nullptr,
                      AuditLedger* audit = nullptr);
        ~Observability() override;
        Observability(const Observability&) = delete;
        Observability& operator=(const Observability&) = delete;

        void on_attempt(const AttemptRecord& record) noexcept override;
        void on_request(const RequestRecord& record) noexcept override;
        [[nodiscard]] Latency latency(size_t index) const noexcept;
        [[nodiscard]] uint64_t dropped_logs() const noexcept;
        [[nodiscard]] bool audit_healthy() const noexcept;
        [[nodiscard]] uint64_t audit_written() const noexcept;

      private:
        struct UpstreamLatency
        {
            std::atomic<uint64_t> count{0};
            std::atomic<uint64_t> total_ns{0};
        };
        struct LogEntry
        {
            uint64_t request_id = 0;
            int initial_upstream = -1;
            int final_upstream = -1;
            int status = 0;
            int attempts = 0;
            int64_t duration_us = 0;
            bool streamed = false;
            int user_index = -1;
            int tokens_in = -1;
            int tokens_out = -1;
            int64_t venue_cost_pdollars = -1;
        };

        void log_loop() noexcept;

        size_t upstream_count_;
        RoutingPolicy* policy_;
        AuditLedger* audit_;
        std::unique_ptr<UpstreamLatency[]> latencies_;
        bool request_log_;
        static constexpr size_t kLogCapacity = 1024;
        std::array<LogEntry, kLogCapacity> log_queue_{};
        std::mutex log_mutex_;
        std::condition_variable log_cv_;
        size_t log_head_ = 0;
        size_t log_tail_ = 0;
        size_t log_count_ = 0;
        bool stopping_ = false;
        std::thread log_thread_;
        std::atomic<uint64_t> dropped_logs_{0};
    };
} // namespace llmbridge::app
