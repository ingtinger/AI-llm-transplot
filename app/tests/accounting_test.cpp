// AI-llm-transplot 版本化价目表测试。

#include "accounting.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>

namespace
{
    llmbridge::app::PriceRule rule(std::string model = "model-a")
    {
        llmbridge::app::PriceRates rates;
        rates.input = 2;
        rates.output = 5;
        return {.provider = "provider-a",
                .model = std::move(model),
                .version = "2026-10-22",
                .source = "https://provider.invalid/pricing",
                .valid_from_s = 100,
                .valid_until_s = 200,
                .rates = rates};
    }

    llmbridge::RequestRecord usage()
    {
        llmbridge::RequestRecord r{};
        r.wall_t0_ns = 150'000'000'000LL;
        r.served_model = "model-a";
        r.tokens_in = 10;
        r.tokens_out = 4;
        return r;
    }
}

TEST(Accounting, SelectsProviderModelAndEffectiveVersion)
{
    auto old = rule();
    old.version = "old";
    old.valid_from_s = 1;
    old.valid_until_s = 100;
    const llmbridge::app::PriceTable prices({old, rule()});
    const auto quote = prices.quote("provider-a", usage());
    ASSERT_TRUE(quote.known());
    EXPECT_EQ(quote.cost_pdollars, 40);
    EXPECT_EQ(quote.price_version, "2026-10-22");
    EXPECT_EQ(quote.price_source, "https://provider.invalid/pricing");
    EXPECT_EQ(prices.quote("provider-b", usage()).status,
              llmbridge::app::CostStatus::PriceMissing);
}

TEST(Accounting, UsesServedModelBeforeRequestedModel)
{
    auto requested = rule("requested");
    requested.rates.input = 100;
    auto served = rule("served");
    const llmbridge::app::PriceTable prices({requested, served});
    auto r = usage();
    r.model = "requested";
    r.served_model = "served";
    EXPECT_EQ(prices.quote("provider-a", r).cost_pdollars, 40);
}

TEST(Accounting, SeparatelyPricesTokenComponentsWithoutDoubleCounting)
{
    auto p = rule();
    p.rates.cached_input = 1;
    p.rates.cache_write_5m = 3;
    p.rates.cache_write_1h = 4;
    p.rates.reasoning = 7;
    p.rates.audio_output = 11;
    p.rates.web_search = 13;
    const llmbridge::app::PriceTable prices({p});
    auto r = usage();
    r.tokens_in = 20;
    r.cached_tokens = 5;
    r.cache_write_tokens = 5;
    r.cache_write_5m_tokens = 3;
    r.cache_write_1h_tokens = 2;
    r.tokens_out = 10;
    r.reasoning_tokens = 2;
    r.audio_out_tokens = 1;
    r.web_search_requests = 2;
    // 输入 10*2 + 5*1 + 3*3 + 2*4；输出 7*5 + 2*7 + 1*11；搜索 2*13。
    const auto quote = prices.quote("provider-a", r);
    ASSERT_TRUE(quote.known());
    EXPECT_EQ(quote.cost_pdollars, 128);
}

TEST(Accounting, DoesNotTurnUnknownUsageIntoZero)
{
    auto p = rule();
    p.rates.cached_input = 1;
    const llmbridge::app::PriceTable prices({p});
    auto r = usage();
    EXPECT_EQ(prices.quote("provider-a", r).status,
              llmbridge::app::CostStatus::UsageUnknown);

    p.rates.cached_input = p.rates.input;
    const llmbridge::app::PriceTable same_rate({p});
    EXPECT_TRUE(same_rate.quote("provider-a", r).known());
}

TEST(Accounting, RequiresTimestampAndCompleteDifferentlyPricedCacheSplit)
{
    auto p = rule();
    p.rates.cache_write = 3;
    p.rates.cache_write_5m = 4;
    p.rates.cache_write_1h = 5;
    const llmbridge::app::PriceTable prices({p});
    auto r = usage();
    r.wall_t0_ns = 0;
    EXPECT_EQ(prices.quote("provider-a", r).status,
              llmbridge::app::CostStatus::TimestampUnknown);

    r = usage();
    r.tokens_in = 15;
    r.cache_write_tokens = 5;
    EXPECT_EQ(prices.quote("provider-a", r).status,
              llmbridge::app::CostStatus::UsageUnknown);
    r.cache_write_5m_tokens = 2;
    r.cache_write_1h_tokens = -1;
    EXPECT_EQ(prices.quote("provider-a", r).status,
              llmbridge::app::CostStatus::UsageUnknown);
    r.cache_write_1h_tokens = 3;
    EXPECT_TRUE(prices.quote("provider-a", r).known());
}

TEST(Accounting, ReportsMissingComponentPriceAndInvalidPartition)
{
    const llmbridge::app::PriceTable prices({rule()});
    auto r = usage();
    r.web_fetch_requests = 1;
    EXPECT_EQ(prices.quote("provider-a", r).status,
              llmbridge::app::CostStatus::PriceMissing);
    r.web_fetch_requests = -1;
    r.cached_tokens = 11;
    EXPECT_EQ(prices.quote("provider-a", r).status,
              llmbridge::app::CostStatus::InvalidUsage);
}

TEST(Accounting, UsesPriorModelsOwnPriceVersion)
{
    auto current = rule();
    auto prior = rule("model-old");
    prior.version = "prior-v3";
    prior.source = "provider invoice archive";
    prior.rates.input = 3;
    prior.rates.output = 7;
    const llmbridge::app::PriceTable prices({current, prior});
    auto r = usage();
    r.prior_model = "model-old";
    r.prior_in_tokens = 2;
    r.prior_out_tokens = 3;
    r.prior_cached_tokens = 0;
    r.prior_cache_write_tokens = 0;
    r.prior_attempts = 1;
    const auto quote = prices.quote("provider-a", r);
    ASSERT_TRUE(quote.known());
    EXPECT_EQ(quote.cost_pdollars, 67);
    EXPECT_EQ(quote.prior_price_version, "prior-v3");
    EXPECT_EQ(quote.prior_price_source, "provider invoice archive");
}

TEST(Accounting, RejectsAmbiguousOrUnauditableRules)
{
    auto duplicate = rule();
    duplicate.version = "another";
    EXPECT_THROW(llmbridge::app::PriceTable({rule(), duplicate}), std::invalid_argument);
    auto missing_source = rule();
    missing_source.source.clear();
    EXPECT_THROW(llmbridge::app::PriceTable({missing_source}), std::invalid_argument);
    auto bad_rate = rule();
    bad_rate.rates.input = -2;
    EXPECT_THROW(llmbridge::app::PriceTable({bad_rate}), std::invalid_argument);
}

TEST(Accounting, DetectsCostOverflow)
{
    auto p = rule();
    p.rates.input = std::numeric_limits<int64_t>::max();
    const llmbridge::app::PriceTable prices({p});
    auto r = usage();
    r.tokens_in = 2;
    r.tokens_out = 0;
    EXPECT_EQ(prices.quote("provider-a", r).status, llmbridge::app::CostStatus::Overflow);
}
