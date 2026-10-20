// AI-llm-transplot 扩展：独立回环管理监听，不复用对外的数据端口。

#pragma once

#include "observability.hpp"
#include "routing_policy.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace llmbridge::app
{
    /// 独立管理线程；只借用策略与统计对象，二者必须活得比本对象更久。
    /// start/stop 由主线程调用；读取的健康状态和指标来自原子快照。
    class ManagementServer final
    {
      public:
        // 仅绑定 127.0.0.1；构造失败会抛出包含原因的异常，调用方应拒绝启动。
        ManagementServer(uint16_t port, const RoutingPolicy& policy,
                         const Observability& observability);
        ~ManagementServer();
        ManagementServer(const ManagementServer&) = delete;
        ManagementServer& operator=(const ManagementServer&) = delete;

        void start();
        void stop() noexcept;

      private:
        void serve() noexcept;
        void handle(int client) const noexcept;
        [[nodiscard]] std::string metrics() const;

        int listener_ = -1;
        const RoutingPolicy& policy_;
        const Observability& observability_;
        std::atomic<bool> stopping_{false};
        std::atomic<bool> running_{false};
        std::thread thread_;
    };
} // namespace llmbridge::app
