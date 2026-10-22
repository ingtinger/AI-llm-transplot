// AI-llm-transplot 内置路由策略测试。

#include "routing_policy.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <stdexcept>
#include <thread>

namespace
{
    llmbridge::app::RoutingPolicyConfig config(size_t upstreams)
    {
        return {.mode = llmbridge::app::RoutingMode::RoundRobin,
                .upstream_count = upstreams,
                .max_attempts = static_cast<int>(upstreams),
                .failure_threshold = 1,
                .cooldown_ns = 60'000'000'000LL,
                .sticky_header = "x-conversation-id",
                .credential_headers = std::vector<std::string>(upstreams),
                .models = std::vector<std::string>(upstreams),
                .client_models = {},
                .security_enabled = false,
                .allow_unsafe_replay = true,
                .server_credentials = {},
                .users = {}};
    }
}

TEST(RoutingPolicy, RoundRobinVisitsEveryUpstream)
{
    llmbridge::app::RoutingPolicy policy(config(3));
    llmbridge::RequestFacts facts{};
    EXPECT_EQ(policy.decide(facts).upstream_index, 0);
    EXPECT_EQ(policy.decide(facts).upstream_index, 1);
    EXPECT_EQ(policy.decide(facts).upstream_index, 2);
    EXPECT_EQ(policy.decide(facts).upstream_index, 0);
}

TEST(RoutingPolicy, StickyHeaderLengthIsBounded)
{
    auto c = config(1);
    c.sticky_header.assign(65, 'x');
    EXPECT_THROW(llmbridge::app::RoutingPolicy(std::move(c)), std::invalid_argument);
}

TEST(RoutingPolicy, SecureUserPermissionsMustBeUnique)
{
    auto duplicate_models = config(1);
    duplicate_models.security_enabled = true;
    duplicate_models.server_credentials = {"provider-secret"};
    duplicate_models.users = {{"alice", "tenant", "gateway-key-alice-123456",
                               {"chat", "chat"}, {0}, 10, 1, 0, 0, 0, false}};
    EXPECT_THROW(llmbridge::app::RoutingPolicy(std::move(duplicate_models)), std::invalid_argument);

    auto duplicate_upstreams = config(1);
    duplicate_upstreams.security_enabled = true;
    duplicate_upstreams.server_credentials = {"provider-secret"};
    duplicate_upstreams.users = {{"alice", "tenant", "gateway-key-alice-123456",
                                  {"chat"}, {0, 0}, 10, 1, 0, 0, 0, false}};
    EXPECT_THROW(llmbridge::app::RoutingPolicy(std::move(duplicate_upstreams)), std::invalid_argument);
}

TEST(RoutingPolicy, UnsafeStatusRetryRequiresExplicitOptIn)
{
    auto c = config(2);
    c.allow_unsafe_replay = false;
    llmbridge::app::RoutingPolicy policy(std::move(c));
    EXPECT_FALSE(policy.allows_unsafe_replay());
    EXPECT_FALSE(policy.on_status_retry({.upstream_index = 0, .status = 503,
                                         .retryable = true, .response_received = true}).retry);
}

TEST(RoutingPolicy, DrainMaskIsPublishedAsOneRoutingVersion)
{
    llmbridge::app::RoutingPolicy policy(config(2));
    std::atomic<bool> running{true};
    std::atomic<int> denied{0};
    std::thread worker([&] {
        llmbridge::RequestFacts facts{};
        while (running.load(std::memory_order_relaxed))
            if (!policy.decide(facts).allow) denied.fetch_add(1, std::memory_order_relaxed);
    });
    for (int i = 0; i < 10'000; ++i)
        policy.set_drained(i % 2 ? std::vector<bool>{false, true} : std::vector<bool>{true, false});
    running.store(false, std::memory_order_relaxed);
    worker.join();
    EXPECT_EQ(denied.load(), 0);
    policy.set_drained({true, false});
    EXPECT_EQ(policy.decide({}).upstream_index, 1);
    policy.set_drained({false, false});
    EXPECT_EQ(policy.available_count(), 2U);
}

TEST(RoutingPolicy, StickyConversationKeepsTheSameUpstream)
{
    auto c = config(3);
    c.mode = llmbridge::app::RoutingMode::Sticky;
    llmbridge::app::RoutingPolicy policy(std::move(c));
    const std::string head =
        "POST /v1/chat/completions HTTP/1.1\r\nX-Conversation-Id: conversation-42\r\n\r\n";
    llmbridge::RequestFacts facts{};
    facts.head = head;
    facts.seq = 1;
    const int first = policy.decide(facts).upstream_index;
    facts.seq = 999;
    EXPECT_EQ(policy.decide(facts).upstream_index, first);
}

TEST(RoutingPolicy, FailureRetriesAnotherUpstreamAndOpensCircuit)
{
    auto c = config(3);
    c.mode = llmbridge::app::RoutingMode::Sticky;
    llmbridge::app::RoutingPolicy policy(std::move(c));
    const std::string head =
        "POST /v1/chat/completions HTTP/1.1\r\nX-Conversation-Id: same-chat\r\n\r\n";
    llmbridge::RequestFacts request{};
    request.head = head;
    request.seq = 1;
    const int first = policy.decide(request).upstream_index;
    const llmbridge::Retry retry = policy.on_failure(
        {.upstream_index = first, .attempt = 0, .retryable = true});
    ASSERT_TRUE(retry.retry);
    EXPECT_NE(retry.upstream_index, first);
    EXPECT_NE(policy.decide(request).upstream_index, first);
}

TEST(RoutingPolicy, SelectedVenueSuppliesItsCredentialAndModel)
{
    auto c = config(2);
    c.credential_headers = {"x-kottos-key-primary", "x-kottos-key-backup"};
    c.models = {"model-a", "model-b"};
    llmbridge::app::RoutingPolicy policy(std::move(c));
    const llmbridge::Decision first = policy.decide({});
    const llmbridge::Decision second = policy.decide({});
    EXPECT_EQ(first.credential_header, "x-kottos-key-primary");
    EXPECT_EQ(first.model, "model-a");
    EXPECT_EQ(second.credential_header, "x-kottos-key-backup");
    EXPECT_EQ(second.model, "model-b");
}

TEST(RoutingPolicy, UnsafeCredentialHeaderIsRejected)
{
    auto c = config(1);
    c.credential_headers = {"authorization"};
    EXPECT_THROW(llmbridge::app::RoutingPolicy(std::move(c)), std::invalid_argument);
}

TEST(RoutingPolicy, SecureUsersAreIsolatedAcrossAuthenticationPermissionAndFailover)
{
    auto c = config(2);
    c.mode = llmbridge::app::RoutingMode::Sticky;
    c.security_enabled = true;
    c.server_credentials = {"provider-secret-a", "provider-secret-b"};
    c.users = {{"alice", "tenant-a", "gateway-key-alice-123456", {"model-a"}, {0, 1}, 2, 1, 0, 0, 0, false},
               {"bob", "tenant-b", "gateway-key-bob-12345678", {"model-b"}, {1}, 2, 1, 0, 0, 0, false}};
    llmbridge::app::RoutingPolicy policy(std::move(c));
    const std::string alice_head = "POST /v1/chat/completions HTTP/1.1\r\nAuthorization: Bearer gateway-key-alice-123456\r\nX-Conversation-Id: shared\r\n\r\n";
    const std::string bob_head = "POST /v1/chat/completions HTTP/1.1\r\nAuthorization: Bearer gateway-key-bob-12345678\r\nX-Conversation-Id: shared\r\n\r\n";
    llmbridge::RequestFacts facts{};
    facts.head = alice_head;
    facts.model = "model-a";
    const auto alice = policy.decide(facts);
    ASSERT_TRUE(alice.allow);
    EXPECT_EQ(alice.server_credential, alice.upstream_index == 0 ? "provider-secret-a" : "provider-secret-b");
    EXPECT_EQ(alice.tag >> 32, 1U);
    EXPECT_EQ(policy.decide(facts).deny_status, 429);
    policy.on_abandon(alice.tag, false);
    const auto after_abort = policy.decide(facts);
    ASSERT_TRUE(after_abort.allow);
    llmbridge::RequestRecord alice_record{};
    alice_record.tag = after_abort.tag;
    policy.complete(alice_record);
    facts.head = bob_head;
    EXPECT_EQ(policy.decide(facts).deny_status, 403);
    facts.model = "model-b";
    const auto bob = policy.decide(facts);
    ASSERT_TRUE(bob.allow);
    EXPECT_EQ(bob.upstream_index, 1);
    EXPECT_EQ(bob.server_credential, "provider-secret-b");
    EXPECT_EQ(bob.tag >> 32, 2U);
    EXPECT_FALSE(policy.on_failure({.upstream_index = 1, .tag = bob.tag, .retryable = true}).retry);
    llmbridge::RequestRecord bob_record{};
    bob_record.tag = bob.tag;
    policy.complete(bob_record);
    facts.head = "POST / HTTP/1.1\r\nAuthorization: Bearer wrong-token\r\n\r\n";
    EXPECT_EQ(policy.decide(facts).deny_status, 401);
    facts.head = "POST / HTTP/1.1\r\nAuthorization: Bearer gateway-key-alice-123456\r\nAuthorization: Bearer gateway-key-alice-123456\r\n\r\n";
    EXPECT_EQ(policy.decide(facts).deny_status, 401);
}

TEST(RoutingPolicy, SecureQuotaAccountsKnownUsageAndFailsClosedOnUnknownUsage)
{
    auto c = config(1);
    c.security_enabled = true;
    c.server_credentials = {"provider-secret"};
    c.users = {{"alice", "tenant", "gateway-key-alice-123456", {"chat"}, {0},
                10, 1, 5, 5, 0, false}};
    llmbridge::app::RoutingPolicy policy(std::move(c));
    const std::string head = "POST / HTTP/1.1\r\nAuthorization: Bearer gateway-key-alice-123456\r\n\r\n";
    llmbridge::RequestFacts facts{};
    facts.head = head;
    facts.model = "chat";
    const auto decision = policy.decide(facts);
    ASSERT_TRUE(decision.allow);
    llmbridge::RequestRecord record{};
    record.tag = decision.tag;
    record.tokens_in = 3;
    record.tokens_out = 2;
    policy.complete(record);
    EXPECT_EQ(policy.decide(facts).deny_status, 429);

    auto unknown_config = config(1);
    unknown_config.security_enabled = true;
    unknown_config.server_credentials = {"provider-secret"};
    unknown_config.users = {{"bob", "tenant", "gateway-key-bob-12345678", {"chat"}, {0},
                             10, 1, 5, 5, 0, false}};
    llmbridge::app::RoutingPolicy unknown_policy(std::move(unknown_config));
    facts.head = "POST / HTTP/1.1\r\nAuthorization: Bearer gateway-key-bob-12345678\r\n\r\n";
    const auto unknown_decision = unknown_policy.decide(facts);
    ASSERT_TRUE(unknown_decision.allow);
    record = {};
    record.tag = unknown_decision.tag;
    record.ts_up_sent = 1;
    unknown_policy.complete(record);
    EXPECT_EQ(unknown_policy.decide(facts).deny_status, 429);
}

TEST(RoutingPolicy, QuotaUsersFailClosedWhenTheAuditLedgerIsUnhealthy)
{
    std::atomic<bool> accounting_healthy{false};
    auto c = config(1);
    c.security_enabled = true;
    c.server_credentials = {"provider-secret"};
    c.users = {{"alice", "tenant", "gateway-key-alice-123456", {"chat"}, {0},
                10, 1, 5, 5, 0, false}};
    c.accounting_healthy = &accounting_healthy;
    llmbridge::app::RoutingPolicy policy(std::move(c));
    llmbridge::RequestFacts facts{};
    facts.model = "chat";
    facts.head = "POST / HTTP/1.1\r\nAuthorization: Bearer gateway-key-alice-123456\r\n\r\n";
    const auto denied = policy.decide(facts);
    EXPECT_EQ(denied.deny_status, 503);
    EXPECT_STREQ(denied.reason, "accounting unavailable");
    accounting_healthy.store(true, std::memory_order_release);
    const auto allowed = policy.decide(facts);
    EXPECT_TRUE(allowed.allow);
    policy.on_abandon(allowed.tag, false);
}

TEST(RoutingPolicy, RetryAttemptsAreIncludedInTokenQuotaWithoutDoubleCountingTheFinalLeg)
{
    auto c = config(1);
    c.security_enabled = true;
    c.server_credentials = {"provider-secret"};
    c.users = {{"alice", "tenant", "gateway-key-alice-123456", {"chat"}, {0},
                10, 1, 10, 10, 0, false}};
    llmbridge::app::RoutingPolicy policy(std::move(c));
    llmbridge::RequestFacts facts{};
    facts.model = "chat";
    facts.head = "POST / HTTP/1.1\r\nAuthorization: Bearer gateway-key-alice-123456\r\n\r\n";
    const auto decision = policy.decide(facts);
    ASSERT_TRUE(decision.allow);

    llmbridge::AttemptRecord retry;
    retry.tag = decision.tag;
    retry.retrying = true;
    retry.upstream_sent = true;
    retry.tokens_in = 4;
    retry.tokens_out = 3;
    policy.complete_attempt(retry);
    llmbridge::RequestRecord final_record;
    final_record.tag = decision.tag;
    final_record.tokens_in = 2;
    final_record.tokens_out = 1;
    policy.complete(final_record);
    EXPECT_EQ(policy.decide(facts).deny_status, 429);

    auto unknown = config(1);
    unknown.security_enabled = true;
    unknown.server_credentials = {"provider-secret"};
    unknown.users = {{"bob", "tenant", "gateway-key-bob-12345678", {"chat"}, {0},
                      10, 1, 100, 100, 0, false}};
    llmbridge::app::RoutingPolicy unknown_policy(std::move(unknown));
    facts.head = "POST / HTTP/1.1\r\nAuthorization: Bearer gateway-key-bob-12345678\r\n\r\n";
    const auto bob = unknown_policy.decide(facts);
    ASSERT_TRUE(bob.allow);
    retry = {};
    retry.tag = bob.tag;
    retry.retrying = true;
    retry.upstream_sent = true;
    unknown_policy.complete_attempt(retry);
    final_record = {};
    final_record.tag = bob.tag;
    final_record.tokens_in = 1;
    final_record.tokens_out = 1;
    unknown_policy.complete(final_record);
    EXPECT_EQ(unknown_policy.decide(facts).deny_status, 429);
}

TEST(RoutingPolicy, DisabledAndExpiredUserKeysAreRejected)
{
    auto c = config(1);
    c.security_enabled = true;
    c.server_credentials = {"provider-secret"};
    c.users = {{"alice", "tenant", "gateway-key-alice-123456", {"chat"}, {0},
                10, 1, 0, 0, 1, false},
               {"bob", "tenant", "gateway-key-bob-12345678", {"chat"}, {0},
                10, 1, 0, 0, 0, true}};
    llmbridge::app::RoutingPolicy policy(std::move(c));
    llmbridge::RequestFacts facts{};
    facts.model = "chat";
    facts.head = "POST / HTTP/1.1\r\nAuthorization: Bearer gateway-key-alice-123456\r\n\r\n";
    EXPECT_EQ(policy.decide(facts).deny_status, 401);
    facts.head = "POST / HTTP/1.1\r\nAuthorization: Bearer gateway-key-bob-12345678\r\n\r\n";
    EXPECT_EQ(policy.decide(facts).deny_status, 401);
}

TEST(RoutingPolicy, StopsAfterConfiguredAttemptLimit)
{
    auto c = config(3);
    c.mode = llmbridge::app::RoutingMode::First;
    c.max_attempts = 2;
    llmbridge::app::RoutingPolicy policy(std::move(c));
    EXPECT_EQ(policy.decide({}).upstream_index, 0);
    const auto second = policy.on_failure(
        {.upstream_index = 0, .attempt = 0, .retryable = true});
    ASSERT_TRUE(second.retry);
    EXPECT_EQ(second.upstream_index, 1);
    EXPECT_FALSE(policy.on_failure(
        {.upstream_index = 1, .attempt = 1, .retryable = true}).retry);
}

TEST(RoutingPolicy, NonRetryableFailureStillOpensCircuit)
{
    auto c = config(2);
    c.mode = llmbridge::app::RoutingMode::First;
    llmbridge::app::RoutingPolicy policy(std::move(c));
    EXPECT_FALSE(policy.on_failure(
        {.upstream_index = 0, .attempt = 0, .retryable = false}).retry);
    EXPECT_EQ(policy.decide({}).upstream_index, 1);
}

TEST(RoutingPolicy, AllCoolingUpstreamsStillAllowOneProbe)
{
    auto c = config(2);
    c.mode = llmbridge::app::RoutingMode::First;
    llmbridge::app::RoutingPolicy policy(std::move(c));
    policy.on_failure({.upstream_index = 0, .retryable = false});
    policy.on_failure({.upstream_index = 1, .retryable = false});
    EXPECT_EQ(policy.decide({}).upstream_index, 0);
}

TEST(RoutingPolicy, CooldownExpiryRestoresPreferredUpstream)
{
    auto c = config(2);
    c.mode = llmbridge::app::RoutingMode::First;
    c.cooldown_ns = 1'000'000;
    llmbridge::app::RoutingPolicy policy(std::move(c));
    policy.on_failure({.upstream_index = 0, .retryable = false});
    EXPECT_EQ(policy.decide({}).upstream_index, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(policy.decide({}).upstream_index, 0);
}

TEST(RoutingPolicy, ConsecutiveFailuresIncreaseCooldownWithinABound)
{
    auto c = config(2);
    c.mode = llmbridge::app::RoutingMode::First;
    c.cooldown_ns = 200'000'000;
    llmbridge::app::RoutingPolicy policy(std::move(c));
    policy.on_failure({.upstream_index = 0, .retryable = false});
    EXPECT_FALSE(policy.snapshot(0).available);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_TRUE(policy.snapshot(0).available);
    policy.on_failure({.upstream_index = 0, .retryable = false});
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    EXPECT_FALSE(policy.snapshot(0).available);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(policy.snapshot(0).available);
}

TEST(RoutingPolicy, ErrorReplyAffectsLaterRequestsAndSuccessClearsIt)
{
    auto c = config(2);
    c.mode = llmbridge::app::RoutingMode::First;
    llmbridge::app::RoutingPolicy policy(std::move(c));
    policy.on_reply({.upstream_index = 0, .status = 429});
    EXPECT_EQ(policy.decide({}).upstream_index, 1);
    policy.on_reply({.upstream_index = 0, .status = 200});
    EXPECT_EQ(policy.decide({}).upstream_index, 0);
}

TEST(RoutingPolicy, RetryAfterPreventsCoolingProbeAndStatusIsNotTransportFailure)
{
    auto c = config(2);
    c.mode = llmbridge::app::RoutingMode::First;
    c.max_attempts = 2;
    llmbridge::app::RoutingPolicy policy(std::move(c));
    policy.on_reply({.upstream_index = 0, .status = 429, .retry_after_s = 30});
    const auto retry = policy.on_status_retry({.upstream_index = 0, .status = 429,
                                               .retryable = true, .response_received = true});
    ASSERT_TRUE(retry.retry);
    EXPECT_EQ(retry.upstream_index, 1);
    EXPECT_EQ(policy.snapshot(0).transport_failures, 0u);
    policy.on_reply({.upstream_index = 1, .status = 429, .retry_after_s = 30});
    EXPECT_FALSE(policy.decide({}).allow);
}

TEST(RoutingPolicy, ModelPoolsConstrainInitialSelectionAndRetry)
{
    auto c = config(3);
    c.mode = llmbridge::app::RoutingMode::First;
    c.max_attempts = 2;
    c.client_models = {{"chat"}, {"code"}, {"chat"}};
    llmbridge::app::RoutingPolicy policy(std::move(c));
    llmbridge::RequestFacts request{};
    request.model = "chat";
    EXPECT_EQ(policy.decide(request).upstream_index, 0);
    const auto retry = policy.on_failure({.upstream_index = 0, .model = "chat"});
    ASSERT_TRUE(retry.retry);
    EXPECT_EQ(retry.upstream_index, 2);
    EXPECT_FALSE(policy.on_failure({.upstream_index = 2, .attempt = 1, .model = "chat"}).retry);
    request.model = "code";
    EXPECT_EQ(policy.decide(request).upstream_index, 1);
    request.model = "other";
    EXPECT_FALSE(policy.decide(request).allow);
}

TEST(RoutingPolicy, ModelPoolConfigurationMustCoverEveryVenueAndUserModel)
{
    auto partial = config(2);
    partial.client_models = {{"chat"}, {}};
    EXPECT_THROW(llmbridge::app::RoutingPolicy(std::move(partial)), std::invalid_argument);

    auto secure = config(2);
    secure.client_models = {{"chat"}, {"code"}};
    secure.security_enabled = true;
    secure.server_credentials = {"provider-a", "provider-b"};
    secure.users = {{"alice", "tenant", "gateway-key-alice-123456", {"code"}, {0},
                     10, 1, 0, 0, 0, false}};
    EXPECT_THROW(llmbridge::app::RoutingPolicy(std::move(secure)), std::invalid_argument);
}

TEST(RoutingPolicy, ModelPoolsHaveIndependentRoundRobinCursors)
{
    auto c = config(3);
    c.client_models = {{"chat"}, {"code"}, {"chat"}};
    llmbridge::app::RoutingPolicy policy(std::move(c));
    llmbridge::RequestFacts facts{};
    facts.model = "chat";
    EXPECT_EQ(policy.decide(facts).upstream_index, 0);
    facts.model = "code";
    EXPECT_EQ(policy.decide(facts).upstream_index, 1);
    EXPECT_EQ(policy.decide(facts).upstream_index, 1);
    facts.model = "chat";
    EXPECT_EQ(policy.decide(facts).upstream_index, 2);
    EXPECT_EQ(policy.decide(facts).upstream_index, 0);
}

TEST(RoutingPolicy, ThirdAttemptDoesNotRevisitFirstVenue)
{
    auto c = config(3);
    c.mode = llmbridge::app::RoutingMode::First;
    c.client_models = {{"chat"}, {"code"}, {"chat"}};
    llmbridge::app::RoutingPolicy policy(std::move(c));
    llmbridge::RequestFacts facts{};
    facts.model = "chat";
    const auto first = policy.decide(facts);
    ASSERT_TRUE(first.allow);
    const auto second = policy.on_failure({.upstream_index = first.upstream_index,
                                           .tag = first.tag, .model = "chat"});
    ASSERT_TRUE(second.retry);
    EXPECT_EQ(second.upstream_index, 2);
    EXPECT_FALSE(policy.on_failure({.upstream_index = second.upstream_index,
                                    .attempt = 1, .tag = first.tag, .model = "chat"}).retry);
}
