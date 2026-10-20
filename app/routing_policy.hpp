// 本文件是 AI-llm-transplot 对 llmbridge 的扩展，遵循 Apache-2.0 许可证。

#pragma once

#include "gateway/policy.hpp"
#include "gateway/sink.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace llmbridge::app
{
    enum class RoutingMode { First, RoundRobin, Sticky };

    struct UserRule
    {
        std::string id, tenant, token;
        std::vector<std::string> models;
        std::vector<int> upstreams;
        int requests_per_minute = 60;
        int max_concurrent = 4;
        int daily_tokens = 0, monthly_tokens = 0;
        int64_t expires_unix_s = 0;
        bool disabled = false;
    };

    struct RoutingPolicyConfig
    {
        RoutingMode mode = RoutingMode::First;
        size_t upstream_count = 1;
        int max_attempts = 1;
        uint32_t failure_threshold = 3;
        int64_t cooldown_ns = 10'000'000'000LL;
        std::string sticky_header = "x-conversation-id";
        std::vector<std::string> credential_headers;
        std::vector<std::string> models;
        std::vector<std::vector<std::string>> client_models;
        bool security_enabled = false;
        bool allow_unsafe_replay = false;
        std::vector<std::string> server_credentials;
        std::vector<UserRule> users;
    };

    /// 【线程安全】内置多上游策略，可由多个网关工作线程共享。
    ///
    /// round_robin 在健康上游之间轮询；sticky 对会话头做稳定哈希。同一请求在连接失败、
    /// 超时等尚未向客户端发送响应的情况下，会尝试其他上游。连续失败的上游进入冷却期。
    class RoutingPolicy final : public Policy
    {
      public:
        struct Snapshot
        {
            uint64_t attempts = 0;
            uint64_t transport_failures = 0;
            uint64_t responses_2xx = 0;
            uint64_t responses_4xx = 0;
            uint64_t responses_5xx = 0;
            bool available = false;
        };

        explicit RoutingPolicy(RoutingPolicyConfig config);

        Decision decide(const RequestFacts& facts) noexcept override;
        Retry on_failure(const FailureFacts& facts) noexcept override;
        Retry on_status_retry(const FailureFacts& facts) noexcept override;
        void on_reply(const ReplyFacts& facts) noexcept override;
        void on_abandon(uint64_t tag, bool upstream_sent) noexcept override;
        /// 每个已放行请求必须恰好调用一次；包括客户端断连。tag 上半部是用户索引。
        void complete(const RequestRecord& record) noexcept;
        bool wants_prefix_hash() const noexcept override;
        bool allows_unsafe_replay() const noexcept override { return config_.allow_unsafe_replay; }
        /// 供管理线程读取的原子快照；多个字段不保证来自同一时刻。
        [[nodiscard]] size_t upstream_count() const noexcept { return config_.upstream_count; }
        [[nodiscard]] size_t available_count() const noexcept;
        [[nodiscard]] Snapshot snapshot(size_t index) const noexcept;
        /// 管理线程调用；一次性发布最多 64 个上游的摘除位图。旧请求仍可完成。
        void set_drained(const std::vector<bool>& drained) noexcept;

      private:
        struct UpstreamState
        {
            std::atomic<uint32_t> failures{0};
            std::atomic<int64_t> blocked_until_ns{0};
            std::atomic<int64_t> rate_limited_until_ns{0};
            std::atomic<uint64_t> attempts{0};
            std::atomic<uint64_t> transport_failures{0};
            std::atomic<uint64_t> responses_2xx{0};
            std::atomic<uint64_t> responses_4xx{0};
            std::atomic<uint64_t> responses_5xx{0};
        };

        struct UserState
        {
            std::atomic<int> concurrent{0};
            std::atomic<uint64_t> minute_and_requests{0};
            std::atomic<uint64_t> daily_usage{0};
            std::atomic<uint64_t> monthly_usage{0};
        };

        struct ModelPool
        {
            std::string model;
            std::vector<size_t> upstreams;
            std::atomic<uint64_t> next{0};
        };

        static int64_t now_ns() noexcept;
        static uint64_t hash_text(std::string_view text) noexcept;
        ModelPool* pool_for(std::string_view model) const noexcept;
        int pick(size_t start, int excluded, int first_attempt, int64_t now, std::string_view model,
                 const UserRule* user = nullptr) const noexcept;
        bool available(size_t index, int64_t now) const noexcept;
        void record_failure(size_t index, int64_t now) noexcept;
        void record_success(size_t index) noexcept;
        std::string_view credential_for(size_t index) const noexcept;
        std::string_view model_for(size_t index) const noexcept;

        RoutingPolicyConfig config_;
        std::unique_ptr<UpstreamState[]> states_;
        std::unique_ptr<UserState[]> user_states_;
        std::vector<std::unique_ptr<ModelPool>> model_pools_;
        std::atomic<uint64_t> next_{0};
        std::atomic<uint64_t> drained_mask_{0};
        bool model_pools_enabled_ = false;
    };

    /// 将配置字符串转换为路由模式；未知值返回空。
    [[nodiscard]] bool routing_mode_from_name(std::string_view name, RoutingMode& out) noexcept;
} // namespace llmbridge::app
