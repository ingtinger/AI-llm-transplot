// AI-llm-transplot 持久化尝试账本测试。

#include "audit_ledger.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
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

    size_t count_text(std::string_view text, std::string_view needle)
    {
        size_t count = 0, at = 0;
        while ((at = text.find(needle, at)) != std::string_view::npos)
        {
            ++count;
            at += needle.size();
        }
        return count;
    }
}

TEST(AuditLedger, PersistsStableIdentitiesUsagePriceAndChecksumWithoutContent)
{
    const std::string path = temporary_path();
    const auto table = prices();
    {
        llmbridge::app::AuditLedger ledger(path, table, {"provider-a"},
                                           {{"tenant-a", "alice", "alice-v1"}});
        ledger.on_attempt(attempt(7));
    }
    const std::string text = read_all(path);
    EXPECT_NE(text.find("\"request_seq\":7"), std::string::npos);
    EXPECT_NE(text.find("\"tenant_id\":\"tenant-a\""), std::string::npos);
    EXPECT_NE(text.find("\"user_id\":\"alice\""), std::string::npos);
    EXPECT_NE(text.find("\"key_version\":\"alice-v1\""), std::string::npos);
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
    EXPECT_EQ(count_text(text, "\"request_seq\":"), 2u);
    ::unlink(path.c_str());
}

TEST(AuditLedger, RefusesACompleteRecordWhoseChecksumDoesNotMatch)
{
    const std::string path = temporary_path();
    const int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC);
    ASSERT_GE(fd, 0);
    constexpr std::string_view kCorrupt = R"({"crc32":"00000000"}
)";
    ASSERT_EQ(::write(fd, kCorrupt.data(), kCorrupt.size()),
              static_cast<ssize_t>(kCorrupt.size()));
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
    EXPECT_EQ(count_text(text, "\"request_seq\":"), 400u);
    ::unlink(path.c_str());
}

TEST(AuditLedger, ReplaysCurrentUtcUsageAndUnknownStateByStableUserIdentity)
{
    const std::string path = temporary_path();
    const auto table = prices();
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::system_clock::now().time_since_epoch()).count();
    {
        llmbridge::app::AuditLedger ledger(path, table, {"provider-a"},
                                           {{"tenant-a", "alice"}, {"tenant-b", "bob"}});
        auto known = attempt(501);
        known.wall_t0_ns = now_ns;
        ledger.on_attempt(known);
        auto unknown = attempt(502);
        unknown.wall_t0_ns = now_ns;
        unknown.tag = (uint64_t{2} << 32) | 1;
        unknown.tokens_in = -1;
        unknown.tokens_out = -1;
        ledger.on_attempt(unknown);
    }
    {
        llmbridge::app::AuditLedger ledger(path, table, {"provider-a"},
                                           {{"tenant-a", "alice"}, {"tenant-b", "bob"}});
        const auto replayed = ledger.replayed_usage();
        ASSERT_EQ(replayed.size(), 2u);
        EXPECT_EQ(replayed[0].daily_tokens, 14u);
        EXPECT_EQ(replayed[0].monthly_tokens, 14u);
        EXPECT_FALSE(replayed[0].daily_unknown);
        EXPECT_FALSE(replayed[0].monthly_unknown);
        EXPECT_TRUE(replayed[1].daily_unknown);
        EXPECT_TRUE(replayed[1].monthly_unknown);
    }
    ::unlink(path.c_str());
}

TEST(AuditLedger, RefusesDuplicateAttemptIdentityInTheCurrentMonth)
{
    const std::string path = temporary_path();
    const auto table = prices();
    {
        llmbridge::app::AuditLedger ledger(path, table, {"provider-a"},
                                           {{"tenant-a", "alice"}});
        auto current = attempt(601);
        current.wall_t0_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch()).count();
        ledger.on_attempt(current);
    }
    const std::string original = read_all(path);
    const size_t request = original.find("\"request_seq\":601");
    ASSERT_NE(request, std::string::npos);
    const size_t begin = original.rfind('\n', request);
    const size_t end = original.find('\n', request);
    ASSERT_NE(end, std::string::npos);
    const std::string duplicate = original.substr(begin == std::string::npos ? 0 : begin + 1,
                                                   end - (begin == std::string::npos ? 0 : begin + 1) + 1);
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::write(fd, duplicate.data(), duplicate.size()),
              static_cast<ssize_t>(duplicate.size()));
    ::close(fd);
    EXPECT_THROW((llmbridge::app::AuditLedger(path, table, {"provider-a"},
                                               {{"tenant-a", "alice"}})),
                 std::runtime_error);
    ::unlink(path.c_str());
}

TEST(AuditLedger, RefusesASecondWriterForTheSameLedger)
{
    const std::string path = temporary_path();
    const auto table = prices();
    {
        llmbridge::app::AuditLedger first(path, table, {"provider-a"},
                                          {{"tenant-a", "alice"}});
        EXPECT_THROW((llmbridge::app::AuditLedger(path, table, {"provider-a"},
                                                   {{"tenant-a", "alice"}})),
                     std::runtime_error);
    }
    ::unlink(path.c_str());
}

TEST(AuditLedger, UncleanSessionRestoresQuotaAsUnknown)
{
    const std::string path = temporary_path();
    const auto table = prices();
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        try
        {
            llmbridge::app::AuditLedger ledger(path, table, {"provider-a"},
                                               {{"tenant-a", "alice"}, {"tenant-b", "bob"}});
            ::_exit(0); // 模拟 SIGKILL：不运行析构，账本只有已同步的 session_start。
        }
        catch (...)
        {
            ::_exit(2);
        }
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    {
        llmbridge::app::AuditLedger recovered(path, table, {"provider-a"},
                                               {{"tenant-a", "alice"}, {"tenant-b", "bob"}});
        const auto usage = recovered.replayed_usage();
        ASSERT_EQ(usage.size(), 2u);
        EXPECT_TRUE(usage[0].daily_unknown);
        EXPECT_TRUE(usage[0].monthly_unknown);
        EXPECT_TRUE(usage[1].daily_unknown);
        EXPECT_TRUE(usage[1].monthly_unknown);
    }
    ::unlink(path.c_str());
}

TEST(AuditLedger, WriteFailurePermanentlyLowersTheHealthFlag)
{
    const std::string path = temporary_path();
    const auto table = prices();
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        try
        {
            llmbridge::app::AuditLedger ledger(path, table, {"provider-a"},
                                               {{"tenant-a", "alice", "alice-v1"}});
            struct stat st{};
            if (::stat(path.c_str(), &st) != 0) ::_exit(2);
            const rlimit limit{static_cast<rlim_t>(st.st_size), static_cast<rlim_t>(st.st_size)};
            if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) ::_exit(3);
            std::signal(SIGXFSZ, SIG_IGN);
            auto record = attempt(701);
            record.wall_t0_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count();
            ledger.on_attempt(record);
            for (int i = 0; i < 200 && ledger.healthy(); ++i) ::usleep(10'000);
            ::_exit(ledger.healthy() ? 4 : 0);
        }
        catch (...)
        {
            ::_exit(5);
        }
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    ::unlink(path.c_str());
}
