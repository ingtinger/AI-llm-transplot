// 本文件是 AI-llm-transplot 对 llmbridge 的扩展，遵循 Apache-2.0 许可证。

#include "routing_policy.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
    constexpr uint64_t kIterations = 1'000'000;

    llmbridge::app::RoutingPolicyConfig config(bool pools)
    {
        llmbridge::app::RoutingPolicyConfig c;
        c.mode = llmbridge::app::RoutingMode::RoundRobin;
        c.upstream_count = 4;
        c.max_attempts = 2;
        c.credential_headers.resize(4);
        c.models.resize(4);
        if (pools) c.client_models = {{"chat"}, {"code"}, {"chat"}, {"code"}};
        return c;
    }

    double run(llmbridge::app::RoutingPolicy& policy, uint64_t& checksum)
    {
        llmbridge::RequestFacts facts{};
        for (uint64_t i = 0; i < 10'000; ++i)
        {
            facts.model = i & 1 ? "code" : "chat";
            checksum += static_cast<uint64_t>(policy.decide(facts).upstream_index);
        }
        const auto start = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < kIterations; ++i)
        {
            facts.model = i & 1 ? "code" : "chat";
            checksum += static_cast<uint64_t>(policy.decide(facts).upstream_index);
        }
        const auto end = std::chrono::steady_clock::now();
        return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) /
               static_cast<double>(kIterations);
    }
}

int main()
{
    llmbridge::app::RoutingPolicy legacy(config(false));
    llmbridge::app::RoutingPolicy pooled(config(true));
    uint64_t checksum = 0;
    const double legacy_ns = run(legacy, checksum);
    const double pooled_ns = run(pooled, checksum);
    std::printf("decide ns/request: unpooled=%.2f pooled=%.2f overhead=%.2f checksum=%llu\n",
                legacy_ns, pooled_ns, pooled_ns - legacy_ns,
                static_cast<unsigned long long>(checksum));
    return 0;
}
