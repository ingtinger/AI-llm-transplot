// AI-llm-transplot 扩展：带崩溃尾部恢复和故障闭锁的单机尝试审计账本。

#pragma once

#include "accounting.hpp"
#include "gateway/sink.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace llmbridge::app
{
    struct AuditUser
    {
        std::string tenant_id;
        std::string user_id;
    };

    /// 多个网关线程共享的持久化账本。on_attempt() 不分配内存、不等待锁且不做 I/O；
    /// 字符串在回调内复制到固定大小事件。专用线程追加并同步记录。对象必须晚于网关线程销毁。
    class AuditLedger
    {
      public:
        AuditLedger(std::string path, const PriceTable& prices,
                    std::vector<std::string> provider_ids, std::vector<AuditUser> users);
        ~AuditLedger();
        AuditLedger(const AuditLedger&) = delete;
        AuditLedger& operator=(const AuditLedger&) = delete;

        void on_attempt(const AttemptRecord& record) noexcept;
        [[nodiscard]] bool healthy() const noexcept;
        /// 指针在本对象生命周期内稳定，供共享路由策略做无锁 fail-closed 检查。
        [[nodiscard]] const std::atomic<bool>* health_flag() const noexcept { return &healthy_; }
        [[nodiscard]] uint64_t written() const noexcept;
        [[nodiscard]] std::string_view instance_id() const noexcept { return instance_id_; }

      private:
        template <size_t N>
        struct Text
        {
            char data[N]{};
            uint16_t size = 0;
        };
        struct Entry
        {
            AttemptRecord record;
            Text<65> tenant, user, provider, price_version, prior_price_version;
            Text<513> price_source, prior_price_source;
            Text<65> requested_model;
            Text<256> model;
            Text<129> served_model;
            Text<257> venue_req_id;
            CostStatus cost_status = CostStatus::PriceMissing;
            int64_t cost_pdollars = -1;
        };
        struct Slot
        {
            std::atomic<size_t> sequence{0};
            Entry entry;
        };

        void recover();
        void writer_loop() noexcept;
        bool append(const Entry& entry) noexcept;

        std::string path_;
        const PriceTable& prices_;
        std::vector<std::string> provider_ids_;
        std::vector<AuditUser> users_;
        int fd_ = -1;
        std::string instance_id_;
        static constexpr size_t kCapacity = 1024;
        std::array<Slot, kCapacity> queue_{};
        std::atomic<size_t> enqueue_pos_{0};
        size_t dequeue_pos_ = 0; // 仅审计线程访问
        std::atomic<size_t> queued_{0};
        std::mutex wait_mutex_; // 仅供审计线程休眠；生产者不获取
        std::condition_variable cv_;
        std::atomic<bool> stopping_{false};
        std::thread thread_;
        std::atomic<bool> healthy_{true};
        std::atomic<uint64_t> written_{0};
    };
} // namespace llmbridge::app
