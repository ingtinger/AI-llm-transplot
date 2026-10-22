// AI-llm-transplot 持久化尝试账本测试。

#include "audit_ledger.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    std::string temporary_path()
    {
        char path[] = "/tmp/llmbridge-audit-XXXXXX";
        const int fd = ::mkstemp(path);
        if (fd >= 0) ::close(fd);
        return path;
    }

    llmbridge::app::PriceTable prices()
    {
        llmbridge::app::PriceRates rates;
        rates.input = 2;
        rates.output = 5;
        return llmbridge::app::PriceTable({{.provider = "provider-a",
                                            .model = "model-a",
                                            .version = "v1",
                                            .source = "provider price archive",
                                            .valid_from_s = 100,
                                            .valid_until_s = 200,
                                            .rates = rates}});
    }

    llmbridge::AttemptRecord attempt(uint64_t seq)
    {
        llmbridge::AttemptRecord r;
        r.request_seq = seq;
        r.wall_t0_ns = 150'000'000'000LL;
        r.attempt = 0;
        r.tag = (uint64_t{1} << 32) | 1;
        r.upstream_index = 0;
        r.status = 200;
        r.outcome = llmbridge::AttemptOutcome::Response;
        r.upstream_sent = true;
        r.response_received = true;
        r.tokens_in = 10;
        r.tokens_out = 4;
        r.requested_model = "chat-default";
        r.model = "model-a";
        r.served_model = "model-a";
        r.venue_req_id = "provider-request-42";
        return r;
    }

    std::string read_all(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
}

TEST(AuditLedger, PersistsStableIdentitiesUsagePriceAndChecksumWithoutContent)
{
    const std::string path = temporary_path();
    const auto table = prices();
    {
        llmbridge::app::AuditLedger ledger(path, table, {"provider-a"}, {{"tenant-a", "alice"}});
        ledger.on_attempt(attempt(7));
    }
    const std::string text = read_all(path);
    EXPECT_NE(text.find("\"request_seq\":7"), std::string::npos);
    EXPECT_NE(text.find("\"tenant_id\":\"tenant-a\""), std::string::npos);
    EXPECT_NE(text.find("\"user_id\":\"alice\""), std::string::npos);
    EXPECT_NE(text.find("\"provider_id\":\"provider-a\""), std::string::npos);
    EXPECT_NE(text.find("\"provider_request_id\":\"provider-request-42\""), std::string::npos);
    EXPECT_NE(text.find("\"calculated_cost_pdollars\":40"), std::string::npos);
    EXPECT_NE(text.find("\"price_version\":\"v1\""), std::string::npos);
    EXPECT_NE(text.find("\"price_source\":\"provider price archive\""), std::string::npos);
    EXPECT_NE(text.find("\"crc32\":"), std::string::npos);
    EXPECT_EQ(text.back(), '\n');
    EXPECT_EQ(text.find("Authorization"), std::string::npos);
    ::unlink(path.c_str());
}

TEST(AuditLedger, RecoversOnlyAnIncompleteCrashTailAndKeepsEarlierRecords)
{
    const std::string path = temporary_path();
    const auto table = prices();
    {
        llmbridge::app::AuditLedger ledger(path, table, {"provider-a"}, {{"tenant-a", "alice"}});
        ledger.on_attempt(attempt(1));
    }
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::write(fd, "partial-crash-tail", 18), 18);
    ::close(fd);
    {
        llmbridge::app::AuditLedger ledger(path, table, {"provider-a"}, {{"tenant-a", "alice"}});
        ledger.on_attempt(attempt(2));
    }
    const std::string text = read_all(path);
    EXPECT_EQ(text.find("partial-crash-tail"), std::string::npos);
    EXPECT_NE(text.find("\"request_seq\":1"), std::string::npos);
    EXPECT_NE(text.find("\"request_seq\":2"), std::string::npos);
    EXPECT_EQ(static_cast<size_t>(std::count(text.begin(), text.end(), '\n')), 2u);
    ::unlink(path.c_str());
}

TEST(AuditLedger, RefusesACompleteRecordWhoseChecksumDoesNotMatch)
{
    const std::string path = temporary_path();
    const int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC);
    ASSERT_GE(fd, 0);
    constexpr std::string_view corrupt = R"({"crc32":"00000000"}
)";
    ASSERT_EQ(::write(fd, corrupt.data(), corrupt.size()), static_cast<ssize_t>(corrupt.size()));
    ::close(fd);
    const auto table = prices();
    EXPECT_THROW((llmbridge::app::AuditLedger(path, table, {"provider-a"}, {})),
                 std::runtime_error);
    ::unlink(path.c_str());
}

TEST(AuditLedger, RefusesLedgerPermissionsThatExposeBillingMetadata)
{
    const std::string path = temporary_path();
    ASSERT_EQ(::chmod(path.c_str(), 0644), 0);
    const auto table = prices();
    EXPECT_THROW((llmbridge::app::AuditLedger(path, table, {"provider-a"}, {})),
                 std::runtime_error);
    ASSERT_EQ(::chmod(path.c_str(), 0700), 0);
    EXPECT_THROW((llmbridge::app::AuditLedger(path, table, {"provider-a"}, {})),
                 std::runtime_error);
    ::unlink(path.c_str());
}

TEST(AuditLedger, DrainsConcurrentProducersWithoutLosingRecords)
{
    const std::string path = temporary_path();
    const auto table = prices();
    {
        llmbridge::app::AuditLedger ledger(path, table, {"provider-a"},
                                           {{"tenant-a", "alice"}});
        std::vector<std::thread> producers;
        for (uint64_t producer = 0; producer < 4; ++producer)
        {
            producers.emplace_back([&ledger, producer] {
                for (uint64_t i = 0; i < 100; ++i)
                    ledger.on_attempt(attempt(producer * 100 + i));
            });
        }
        for (auto& producer : producers) producer.join();
        EXPECT_TRUE(ledger.healthy());
    }
    const std::string text = read_all(path);
    EXPECT_EQ(static_cast<size_t>(std::count(text.begin(), text.end(), '\n')), 400u);
    ::unlink(path.c_str());
}
