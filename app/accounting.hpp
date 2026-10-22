// AI-llm-transplot 扩展：版本化价目表与可核对费用计算。

#pragma once

#include "gateway/sink.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace llmbridge::app
{
    /// -1 表示该规则没有为此项目定价；0 表示明确免费。
    /// Token 单价使用皮美元（10^-12 美元）/Token，工具单价使用皮美元/次。
    struct PriceRates
    {
        int64_t input = -1;
        int64_t output = -1;
        int64_t cached_input = -1;
        int64_t cache_write = -1;
        int64_t cache_write_5m = -1;
        int64_t cache_write_1h = -1;
        int64_t reasoning = -1;
        int64_t audio_input = -1;
        int64_t audio_output = -1;
        int64_t accepted_prediction = -1;
        int64_t rejected_prediction = -1;
        int64_t tool_prompt = -1;
        int64_t image_input = -1;
        int64_t web_search = -1;
        int64_t web_fetch = -1;
        int64_t code_exec = -1;
        int64_t file_search = -1;
        bool operator==(const PriceRates&) const = default;
    };

    struct PriceRule
    {
        std::string provider;
        std::string model;
        std::string version;
        std::string source;
        int64_t valid_from_s = 0; ///< 含此 Unix 秒；0 表示不设下界。
        int64_t valid_until_s = 0; ///< 不含此 Unix 秒；0 表示不设上界。
        PriceRates rates;
        bool operator==(const PriceRule&) const = default;
    };

    enum class CostStatus : uint8_t
    {
        Known,
        PriceMissing,
        UsageUnknown,
        TimestampUnknown,
        InvalidUsage,
        Overflow,
    };

    struct CostQuote
    {
        CostStatus status = CostStatus::PriceMissing;
        int64_t cost_pdollars = -1;
        /// 字符串由 PriceTable 持有；PriceTable 销毁后失效。
        std::string_view price_version;
        std::string_view price_source;
        /// 仅当响应包含 prior_model 用量时设置。
        std::string_view prior_price_version;
        std::string_view prior_price_source;

        [[nodiscard]] bool known() const noexcept { return status == CostStatus::Known; }
    };

    /// 构造后不可变，可由多个工作线程并发调用 quote()。
    /// 规则字符串和 CostQuote 中指向它们的视图都由本对象持有。
    class PriceTable
    {
      public:
        explicit PriceTable(std::vector<PriceRule> rules);

        /// provider 是配置中的稳定 Provider 标识；模型优先采用上游响应的
        /// served_model，缺失时退回客户端请求模型。wall_t0_ns 必须有效，
        /// 否则不能审计按日期生效的价格。函数不分配内存且线程安全。
        [[nodiscard]] CostQuote quote(std::string_view provider,
                                      const RequestRecord& record) const noexcept;

      private:
        [[nodiscard]] const PriceRule* find(std::string_view provider, std::string_view model,
                                            int64_t at_s) const noexcept;

        std::vector<PriceRule> rules_;
    };
} // namespace llmbridge::app
