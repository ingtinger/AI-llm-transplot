// AI-llm-transplot 扩展：费用计算只使用 Provider 原始用量，不猜测缺失值。

#include "accounting.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace llmbridge::app
{
    namespace
    {
        bool valid_rate(int64_t rate) noexcept { return rate >= -1; }

        bool valid_rates(const PriceRates& r) noexcept
        {
            return valid_rate(r.input) && valid_rate(r.output) && valid_rate(r.cached_input) &&
                   valid_rate(r.cache_write) && valid_rate(r.cache_write_5m) &&
                   valid_rate(r.cache_write_1h) && valid_rate(r.reasoning) &&
                   valid_rate(r.audio_input) && valid_rate(r.audio_output) &&
                   valid_rate(r.accepted_prediction) && valid_rate(r.rejected_prediction) &&
                   valid_rate(r.tool_prompt) && valid_rate(r.image_input) &&
                   valid_rate(r.web_search) && valid_rate(r.web_fetch) &&
                   valid_rate(r.code_exec) && valid_rate(r.file_search);
        }

        bool overlaps(const PriceRule& a, const PriceRule& b) noexcept
        {
            const int64_t a_end = a.valid_until_s == 0 ? std::numeric_limits<int64_t>::max()
                                                       : a.valid_until_s;
            const int64_t b_end = b.valid_until_s == 0 ? std::numeric_limits<int64_t>::max()
                                                       : b.valid_until_s;
            return a.valid_from_s < b_end && b.valid_from_s < a_end;
        }

        struct Accumulator
        {
            __int128 total = 0;
            CostStatus status = CostStatus::Known;

            void add(int64_t count, int64_t rate) noexcept
            {
                if (status != CostStatus::Known) return;
                if (rate < 0)
                {
                    if (count > 0) status = CostStatus::PriceMissing;
                    return;
                }
                if (count < 0)
                {
                    if (rate != 0) status = CostStatus::UsageUnknown;
                    return;
                }
                total += static_cast<__int128>(count) * rate;
                if (total > std::numeric_limits<int64_t>::max()) status = CostStatus::Overflow;
            }
        };

        struct Part
        {
            int64_t count;
            int64_t rate;
        };

        template <size_t N>
        void add_partition(Accumulator& out, int64_t total, int64_t base_rate,
                           const std::array<Part, N>& parts) noexcept
        {
            if (out.status != CostStatus::Known) return;
            if (total < 0)
            {
                if (base_rate != 0) out.status = CostStatus::UsageUnknown;
                return;
            }
            if (base_rate < 0)
            {
                if (total > 0) out.status = CostStatus::PriceMissing;
                return;
            }
            int64_t remainder = total;
            for (const Part part : parts)
            {
                if (part.count < 0)
                {
                    if (part.rate >= 0 && part.rate != base_rate)
                    {
                        out.status = CostStatus::UsageUnknown;
                        return;
                    }
                    continue;
                }
                if (part.count > remainder)
                {
                    out.status = CostStatus::InvalidUsage;
                    return;
                }
                remainder -= part.count;
                // 子项一旦由 Provider 明确报告，就必须在规则中明确标价；
                // 即使单价恰好等于父项，也不能靠隐式回退掩盖价目表缺项。
                out.add(part.count, part.rate);
            }
            out.add(remainder, base_rate);
        }

        void add_cache_write(Accumulator& out, int64_t total, const PriceRates& rates,
                             int64_t five_minutes, int64_t one_hour) noexcept
        {
            if (out.status != CostStatus::Known) return;
            const bool priced = rates.cache_write >= 0 || rates.cache_write_5m >= 0 ||
                                rates.cache_write_1h >= 0;
            if (total < 0)
            {
                if (priced) out.status = CostStatus::UsageUnknown;
                return;
            }
            if (!priced)
            {
                if (total > 0) out.status = CostStatus::PriceMissing;
                return;
            }
            const bool has_split = five_minutes >= 0 || one_hour >= 0;
            if (!has_split)
            {
                const bool ttl_changes_price =
                    (rates.cache_write_5m >= 0 && rates.cache_write_5m != rates.cache_write) ||
                    (rates.cache_write_1h >= 0 && rates.cache_write_1h != rates.cache_write);
                if (total > 0 && ttl_changes_price)
                {
                    out.status = CostStatus::UsageUnknown;
                    return;
                }
                out.add(total, rates.cache_write);
                return;
            }
            if ((five_minutes < 0 && rates.cache_write_5m >= 0 &&
                 rates.cache_write_5m != rates.cache_write) ||
                (one_hour < 0 && rates.cache_write_1h >= 0 &&
                 rates.cache_write_1h != rates.cache_write))
            {
                out.status = CostStatus::UsageUnknown;
                return;
            }
            const int64_t five = five_minutes > 0 ? five_minutes : 0;
            const int64_t hour = one_hour > 0 ? one_hour : 0;
            if (five > total || hour > total - five)
            {
                out.status = CostStatus::InvalidUsage;
                return;
            }
            out.add(five, rates.cache_write_5m);
            out.add(hour, rates.cache_write_1h);
            out.add(total - five - hour, rates.cache_write);
        }

        void add_main_usage(Accumulator& out, const RequestRecord& r, const PriceRates& p) noexcept
        {
            // cache_write 在下方按 TTL 再拆分，因此这里只从输入总量扣除。
            const bool cache_write_priced = p.cache_write >= 0 || p.cache_write_5m >= 0 ||
                                            p.cache_write_1h >= 0;
            add_partition(out, r.tokens_in, p.input,
                          std::array<Part, 5>{{{r.cached_tokens, p.cached_input},
                                               {r.cache_write_tokens,
                                                cache_write_priced ? 0 : p.input},
                                               {r.audio_in_tokens, p.audio_input},
                                               {r.tool_prompt_tokens, p.tool_prompt},
                                               {r.image_in_tokens, p.image_input}}});
            add_cache_write(out, r.cache_write_tokens, p, r.cache_write_5m_tokens,
                            r.cache_write_1h_tokens);
            add_partition(out, r.tokens_out, p.output,
                          std::array<Part, 4>{{{r.reasoning_tokens, p.reasoning},
                                               {r.audio_out_tokens, p.audio_output},
                                               {r.accepted_prediction_tokens, p.accepted_prediction},
                                               {r.rejected_prediction_tokens, p.rejected_prediction}}});
            out.add(r.web_search_requests, p.web_search);
            out.add(r.web_fetch_requests, p.web_fetch);
            out.add(r.code_exec_requests, p.code_exec);
            out.add(r.file_search_calls, p.file_search);
        }

        void add_extra_usage(Accumulator& out, int64_t in, int64_t out_tokens, int64_t cached,
                             int64_t cache_write, const PriceRates& p) noexcept
        {
            add_partition(out, in, p.input,
                          std::array<Part, 2>{{{cached, p.cached_input},
                                               {cache_write, p.cache_write}}});
            add_partition(out, out_tokens, p.output, std::array<Part, 0>{});
        }
    } // namespace

    PriceTable::PriceTable(std::vector<PriceRule> rules) : rules_(std::move(rules))
    {
        for (const PriceRule& rule : rules_)
        {
            if (rule.provider.empty() || rule.model.empty() || rule.version.empty() ||
                rule.source.empty())
                throw std::invalid_argument("price rule identity, version and source are required");
            if (rule.valid_from_s < 0 || rule.valid_until_s < 0 ||
                (rule.valid_until_s != 0 && rule.valid_from_s >= rule.valid_until_s))
                throw std::invalid_argument("invalid price rule effective range");
            if (!valid_rates(rule.rates)) throw std::invalid_argument("invalid negative price rate");
        }
        std::sort(rules_.begin(), rules_.end(), [](const PriceRule& a, const PriceRule& b) {
            return std::tie(a.provider, a.model, a.valid_from_s) <
                   std::tie(b.provider, b.model, b.valid_from_s);
        });
        for (size_t i = 1; i < rules_.size(); ++i)
            if (rules_[i - 1].provider == rules_[i].provider &&
                rules_[i - 1].model == rules_[i].model && overlaps(rules_[i - 1], rules_[i]))
                throw std::invalid_argument("overlapping price rules");
    }

    const PriceRule* PriceTable::find(std::string_view provider, std::string_view model,
                                      int64_t at_s) const noexcept
    {
        for (const PriceRule& rule : rules_)
            if (rule.provider == provider && rule.model == model &&
                (rule.valid_from_s == 0 || at_s >= rule.valid_from_s) &&
                (rule.valid_until_s == 0 || at_s < rule.valid_until_s))
                return &rule;
        return nullptr;
    }

    CostQuote PriceTable::quote(std::string_view provider, const RequestRecord& record) const noexcept
    {
        if (record.wall_t0_ns <= 0)
        {
            CostQuote missing_time;
            missing_time.status = CostStatus::TimestampUnknown;
            return missing_time;
        }
        const std::string_view model = record.served_model.empty() ? record.model : record.served_model;
        const int64_t at_s = record.wall_t0_ns / 1'000'000'000;
        const PriceRule* rule = find(provider, model, at_s);
        if (!rule) return {};

        CostQuote quote{.status = CostStatus::Known,
                        .cost_pdollars = 0,
                        .price_version = rule->version,
                        .price_source = rule->source,
                        .prior_price_version = {},
                        .prior_price_source = {}};
        Accumulator total;
        add_main_usage(total, record, rule->rates);
        const bool has_compaction = record.compaction_in_tokens >= 0 ||
                                    record.compaction_out_tokens >= 0 ||
                                    record.compaction_cached_tokens >= 0;
        if (has_compaction)
            add_extra_usage(total, record.compaction_in_tokens, record.compaction_out_tokens,
                            record.compaction_cached_tokens, -1, rule->rates);

        const bool has_prior = record.prior_in_tokens >= 0 || record.prior_out_tokens >= 0 ||
                               record.prior_cached_tokens >= 0 ||
                               record.prior_cache_write_tokens >= 0 || record.prior_attempts > 0;
        if (has_prior)
        {
            if (record.prior_model.empty()) total.status = CostStatus::PriceMissing;
            else if (const PriceRule* prior = find(provider, record.prior_model, at_s))
            {
                quote.prior_price_version = prior->version;
                quote.prior_price_source = prior->source;
                add_extra_usage(total, record.prior_in_tokens, record.prior_out_tokens,
                                record.prior_cached_tokens, record.prior_cache_write_tokens,
                                prior->rates);
            }
            else total.status = CostStatus::PriceMissing;
        }

        quote.status = total.status;
        if (total.status == CostStatus::Known) quote.cost_pdollars = static_cast<int64_t>(total.total);
        else quote.cost_pdollars = -1;
        return quote;
    }
} // namespace llmbridge::app
