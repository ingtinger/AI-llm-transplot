// 本文件是 AI-llm-transplot 对 llmbridge 的扩展，遵循 Apache-2.0 许可证。

#include "routing_policy.hpp"

#include "net/http.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <unordered_set>

namespace llmbridge::app
{
    namespace
    {
        constexpr uint64_t kUsageUnknown = 1ULL << 31;
        uint64_t quota_value(const std::atomic<uint64_t>& state, uint32_t epoch) noexcept
        {
            const uint64_t value = state.load(std::memory_order_relaxed);
            return (value >> 32) == epoch ? value & 0xffffffffULL : 0;
        }

        void add_usage(std::atomic<uint64_t>& state, uint32_t epoch, uint32_t tokens,
                       bool unknown) noexcept
        {
            uint64_t old = state.load(std::memory_order_relaxed);
            for (;;)
            {
                const uint64_t current = (old >> 32) == epoch ? old & 0xffffffffULL : 0;
                const uint64_t count = std::min<uint64_t>(kUsageUnknown - 1,
                                                          (current & ~kUsageUnknown) + tokens);
                const uint64_t next = (static_cast<uint64_t>(epoch) << 32) | count |
                                      (unknown || (current & kUsageUnknown) ? kUsageUnknown : 0);
                if (state.compare_exchange_weak(old, next, std::memory_order_acq_rel)) break;
            }
        }

        struct UtcPeriod { uint32_t day, month; };
        UtcPeriod utc_period() noexcept
        {
            using namespace std::chrono;
            const auto today = floor<days>(system_clock::now());
            const year_month_day ymd{today};
            return {static_cast<uint32_t>(today.time_since_epoch().count()),
                    static_cast<uint32_t>(int(ymd.year()) * 12 + unsigned(ymd.month()))};
        }

        bool header_name_ok(std::string_view name) noexcept
        {
            if (name.empty() || name.size() > 64) return false;
            for (const unsigned char c : name)
            {
                const bool alpha_num = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                       (c >= '0' && c <= '9');
                if (!alpha_num && c != '-' && c != '_') return false;
            }
            return true;
        }
    } // namespace

    RoutingPolicy::RoutingPolicy(RoutingPolicyConfig config) : config_(std::move(config))
    {
        if (config_.upstream_count == 0 || config_.upstream_count > 64)
            throw std::invalid_argument("routing: 1..64 upstreams required");
        if (config_.max_attempts < 1) throw std::invalid_argument("routing.max_attempts must be positive");
        if (config_.failure_threshold == 0)
            throw std::invalid_argument("routing.failure_threshold must be positive");
        if (config_.cooldown_ns < 0) throw std::invalid_argument("routing.cooldown_s must not be negative");
        if (!header_name_ok(config_.sticky_header))
            throw std::invalid_argument("routing.sticky_header is not a valid HTTP header name");
        if (config_.credential_headers.size() != config_.upstream_count ||
            config_.models.size() != config_.upstream_count)
            throw std::invalid_argument("routing: upstream metadata size mismatch");
        if (config_.client_models.empty()) config_.client_models.resize(config_.upstream_count);
        if (config_.client_models.size() != config_.upstream_count)
            throw std::invalid_argument("routing: client model pool size mismatch");
        for (const auto& pool : config_.client_models)
            model_pools_enabled_ = model_pools_enabled_ || !pool.empty();
        if (model_pools_enabled_)
        {
            for (size_t index = 0; index < config_.client_models.size(); ++index)
            {
                const auto& pool = config_.client_models[index];
                if (pool.empty() || pool.size() > 64)
                    throw std::invalid_argument("routing: every upstream needs 1..64 client models");
                std::unordered_set<std::string_view> seen;
                for (const auto& model : pool)
                {
                    if (!substitute_ok(model, 64) || !seen.insert(model).second)
                        throw std::invalid_argument("routing: invalid or duplicate client model");
                    ModelPool* target = pool_for(model);
                    if (!target)
                    {
                        auto fresh = std::make_unique<ModelPool>();
                        fresh->model = model;
                        target = fresh.get();
                        model_pools_.push_back(std::move(fresh));
                    }
                    target->upstreams.push_back(index);
                }
            }
        }
        if (config_.security_enabled)
        {
            if (config_.server_credentials.size() != config_.upstream_count || config_.users.empty())
                throw std::invalid_argument("security: credentials and users are required");
            for (const auto& key : config_.server_credentials)
                if (!server_credential_ok(key)) throw std::invalid_argument("security: invalid provider key");
            std::unordered_set<std::string> ids, tokens;
            for (const auto& user : config_.users)
            {
                if (user.id.empty() || user.tenant.empty() || user.token.size() < 16 ||
                    !server_credential_ok(user.token) || !ids.insert(user.id).second ||
                    !tokens.insert(user.token).second || user.models.empty() || user.upstreams.empty() ||
                    user.requests_per_minute < 1 || user.max_concurrent < 1)
                    throw std::invalid_argument("security: invalid or duplicate user");
                if (user.daily_tokens < 0 || user.daily_tokens > 1'000'000'000 ||
                    user.monthly_tokens < 0 || user.monthly_tokens > 1'000'000'000)
                    throw std::invalid_argument("security: invalid token quota");
                std::unordered_set<std::string_view> user_models;
                for (const auto& model : user.models)
                    if (!user_models.insert(model).second)
                        throw std::invalid_argument("security: duplicate user model");
                std::unordered_set<int> user_upstreams;
                for (int index : user.upstreams)
                {
                    if (!user_upstreams.insert(index).second)
                        throw std::invalid_argument("security: duplicate user upstream index");
                    if (index < 0 || static_cast<size_t>(index) >= config_.upstream_count)
                        throw std::invalid_argument("security: user upstream index out of range");
                }
                if (model_pools_enabled_)
                    for (const auto& model : user.models)
                    {
                        bool reachable = false;
                        for (const int index : user.upstreams)
                            for (const auto& supported : config_.client_models[static_cast<size_t>(index)])
                                reachable = reachable || supported == model;
                        if (!reachable)
                            throw std::invalid_argument("security: user model has no permitted upstream");
                    }
            }
            user_states_ = std::make_unique<UserState[]>(config_.users.size());
        }
        for (const std::string& header : config_.credential_headers)
            if (!header.empty() && !key_header_ok(header))
                throw std::invalid_argument("upstream.credential_header must start with x-kottos-key-");
        for (const std::string& model : config_.models)
            if (!model.empty() && !substitute_ok(model, kMaxSubstituteModel))
                throw std::invalid_argument("upstream.model is not safe for JSON substitution");
        states_ = std::make_unique<UpstreamState[]>(config_.upstream_count);
    }

    int64_t RoutingPolicy::now_ns() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    uint64_t RoutingPolicy::hash_text(std::string_view text) noexcept
    {
        uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char c : text)
        {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    RoutingPolicy::ModelPool* RoutingPolicy::pool_for(std::string_view model) const noexcept
    {
        for (const auto& pool : model_pools_)
            if (pool->model == model) return pool.get();
        return nullptr;
    }

    bool RoutingPolicy::available(size_t index, int64_t now) const noexcept
    {
        return (drained_mask_.load(std::memory_order_acquire) & (uint64_t{1} << index)) == 0 &&
               states_[index].blocked_until_ns.load(std::memory_order_relaxed) <= now &&
               states_[index].rate_limited_until_ns.load(std::memory_order_relaxed) <= now;
    }

    int RoutingPolicy::pick(size_t start, int excluded, int first_attempt, int64_t now,
                            std::string_view model,
                            const UserRule* user) const noexcept
    {
        const uint64_t drained = drained_mask_.load(std::memory_order_acquire);
        const ModelPool* pool = model_pools_enabled_ ? pool_for(model) : nullptr;
        if (model_pools_enabled_ && !pool) return -1;
        const size_t count = pool ? pool->upstreams.size() : config_.upstream_count;
        const auto index_at = [pool](size_t position) {
            return pool ? pool->upstreams[position] : position;
        };
        const auto permitted = [user](size_t index) {
            if (user)
            {
                for (int allowed : user->upstreams)
                    if (allowed == static_cast<int>(index)) return true;
                return false;
            }
            return true;
        };
        for (size_t offset = 0; offset < count; ++offset)
        {
            const size_t index = index_at((start + offset) % count);
            if (static_cast<int>(index) != excluded && static_cast<int>(index) != first_attempt &&
                permitted(index) && (drained & (uint64_t{1} << index)) == 0 &&
                states_[index].blocked_until_ns.load(std::memory_order_relaxed) <= now &&
                states_[index].rate_limited_until_ns.load(std::memory_order_relaxed) <= now)
                return static_cast<int>(index);
        }
        // 所有熔断器都打开时允许一次探测，避免网关永久没有可用上游。
        for (size_t offset = 0; offset < count; ++offset)
        {
            const size_t index = index_at((start + offset) % count);
            if (static_cast<int>(index) != excluded && static_cast<int>(index) != first_attempt &&
                permitted(index) &&
                (drained & (uint64_t{1} << index)) == 0 &&
                states_[index].rate_limited_until_ns.load(std::memory_order_relaxed) <= now)
                return static_cast<int>(index);
        }
        return -1;
    }

    void RoutingPolicy::set_drained(const std::vector<bool>& drained) noexcept
    {
        if (drained.size() != config_.upstream_count) return;
        uint64_t mask = 0;
        for (size_t i = 0; i < drained.size(); ++i)
            if (drained[i]) mask |= uint64_t{1} << i;
        drained_mask_.store(mask, std::memory_order_release);
    }

    std::string_view RoutingPolicy::credential_for(size_t index) const noexcept
    {
        return index < config_.credential_headers.size() ? config_.credential_headers[index]
                                                         : std::string_view{};
    }

    std::string_view RoutingPolicy::model_for(size_t index) const noexcept
    {
        return index < config_.models.size() ? config_.models[index] : std::string_view{};
    }

    Decision RoutingPolicy::decide(const RequestFacts& facts) noexcept
    {
        const UserRule* user = nullptr;
        size_t user_index = 0;
        if (config_.security_enabled)
        {
            std::string_view auth;
            int count = 0;
            const std::string_view head = facts.head.ends_with("\r\n\r\n") ? facts.head.substr(0, facts.head.size() - 4) : facts.head;
            const bool valid = net::http::walk_headers(head, [&](const net::http::HeaderLine& h) noexcept {
                if (h.field == net::http::Field::Authorization)
                {
                    ++count;
                    auth = net::http::detail::trim_ows(h.value);
                }
                return true;
            });
            if (!valid || count != 1 || !auth.starts_with("Bearer ") || auth.size() > 262)
                return {.allow = false, .deny_status = 401, .reason = "gateway authentication",
                        .message = "gateway credential required"};
            const std::string_view token = auth.substr(7);
            for (size_t i = 0; i < config_.users.size(); ++i)
            {
                const std::string_view expected = config_.users[i].token;
                unsigned diff = static_cast<unsigned>(expected.size() ^ token.size());
                for (size_t j = 0; j < expected.size(); ++j)
                    diff |= static_cast<unsigned char>(expected[j]) ^
                            static_cast<unsigned char>(j < token.size() ? token[j] : 0);
                if (diff == 0) { user = &config_.users[i]; user_index = i; }
            }
            const int64_t wall_s = std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch()).count();
            if (!user || user->disabled || (user->expires_unix_s > 0 && wall_s >= user->expires_unix_s))
                return {.allow = false, .deny_status = 401, .reason = "gateway authentication",
                        .message = "invalid gateway credential"};
            bool model_allowed = false;
            for (const auto& model : user->models)
                if (facts.model == model) model_allowed = true;
            if (!model_allowed)
                return {.allow = false, .deny_status = 403, .reason = "model permission",
                        .message = "model not permitted"};
            if ((user->daily_tokens > 0 || user->monthly_tokens > 0) &&
                config_.accounting_healthy &&
                !config_.accounting_healthy->load(std::memory_order_acquire))
                return {.allow = false, .deny_status = 503, .reason = "accounting unavailable",
                        .message = "accounting temporarily unavailable"};
            const UtcPeriod period = utc_period();
            const UserState& state = user_states_[user_index];
            if ((user->daily_tokens > 0 &&
                 quota_value(state.daily_usage, period.day) >= static_cast<uint64_t>(user->daily_tokens)) ||
                (user->monthly_tokens > 0 &&
                 quota_value(state.monthly_usage, period.month) >= static_cast<uint64_t>(user->monthly_tokens)))
                return {.allow = false, .deny_status = 429, .reason = "user token quota",
                        .message = "token quota exhausted"};
        }
        size_t start = 0;
        ModelPool* pool = model_pools_enabled_ ? pool_for(facts.model) : nullptr;
        if (model_pools_enabled_ && !pool)
            return {.allow = false, .deny_status = 503, .reason = "model pool unavailable",
                    .message = "model pool unavailable"};
        if (config_.mode == RoutingMode::RoundRobin)
            start = static_cast<size_t>((pool ? pool->next : next_).fetch_add(1, std::memory_order_relaxed));
        else if (config_.mode == RoutingMode::Sticky)
        {
            const std::string_view conversation =
                net::http::find_header(facts.head, config_.sticky_header);
            const uint64_t key = !conversation.empty() ? hash_text(conversation) ^
                                                        (user ? hash_text(user->tenant) ^ hash_text(user->id) : 0)
                                 : facts.prefix_hash != 0 ? facts.prefix_hash
                                                          : facts.seq;
            start = static_cast<size_t>(key);
        }
        const int selected = pick(start % (pool ? pool->upstreams.size() : config_.upstream_count),
                                  -1, -1, now_ns(), facts.model, user);
        if (selected < 0)
            return {.allow = false, .deny_status = 503, .reason = "no upstream available",
                    .message = "no upstream available"};
        const size_t index = static_cast<size_t>(selected);
        if (user)
        {
            UserState& state = user_states_[user_index];
            int concurrent = state.concurrent.load(std::memory_order_relaxed);
            do {
                if (concurrent >= user->max_concurrent)
                    return {.allow = false, .deny_status = 429, .reason = "user concurrency",
                            .message = "too many concurrent requests"};
            } while (!state.concurrent.compare_exchange_weak(concurrent, concurrent + 1,
                                                               std::memory_order_acq_rel));
            const uint64_t minute = static_cast<uint64_t>(now_ns() / 60'000'000'000LL);
            uint64_t old = state.minute_and_requests.load(std::memory_order_relaxed);
            for (;;)
            {
                const uint64_t count = (old >> 32) == minute ? old & 0xffffffffULL : 0;
                if (count >= static_cast<uint64_t>(user->requests_per_minute))
                {
                    state.concurrent.fetch_sub(1, std::memory_order_acq_rel);
                    return {.allow = false, .deny_status = 429, .reason = "user rate",
                            .message = "request rate exceeded"};
                }
                if (state.minute_and_requests.compare_exchange_weak(old, (minute << 32) | (count + 1),
                                                                     std::memory_order_acq_rel))
                    break;
            }
        }
        states_[index].attempts.fetch_add(1, std::memory_order_relaxed);
        return {.allow = true,
                .upstream_index = selected,
                .model = model_for(index),
                .reason = "built-in routing policy",
                .tag = (static_cast<uint64_t>(user ? user_index + 1 : 0) << 32) |
                       static_cast<uint32_t>(selected + 1),
                .credential_header = user ? std::string_view{} : credential_for(index),
                .server_credential = user ? std::string_view(config_.server_credentials[index]) : std::string_view{}};
    }

    void RoutingPolicy::complete(const RequestRecord& record) noexcept
    {
        const uint64_t user = record.tag >> 32;
        if (config_.security_enabled && user > 0 && user <= config_.users.size())
        {
            user_states_[user - 1].concurrent.fetch_sub(1, std::memory_order_acq_rel);
            const UserRule& rule = config_.users[user - 1];
            if (rule.daily_tokens == 0 && rule.monthly_tokens == 0) return;
            const bool unknown = record.ts_up_sent > 0 &&
                                 (record.truncated || record.tokens_in < 0 || record.tokens_out < 0);
            const uint32_t tokens = record.tokens_in >= 0 && record.tokens_out >= 0 ?
                                    static_cast<uint32_t>(std::min<int64_t>(kUsageUnknown - 1,
                                        static_cast<int64_t>(record.tokens_in) + record.tokens_out)) : 0;
            if (!unknown && tokens == 0) return;
            const UtcPeriod period = utc_period();
            if (rule.daily_tokens > 0) add_usage(user_states_[user - 1].daily_usage, period.day, tokens, unknown);
            if (rule.monthly_tokens > 0) add_usage(user_states_[user - 1].monthly_usage, period.month, tokens, unknown);
        }
    }

    void RoutingPolicy::complete_attempt(const AttemptRecord& record) noexcept
    {
        if (!record.retrying) return;
        const uint64_t user = record.tag >> 32;
        if (!config_.security_enabled || user == 0 || user > config_.users.size()) return;
        const UserRule& rule = config_.users[user - 1];
        if (rule.daily_tokens == 0 && rule.monthly_tokens == 0) return;
        const bool complete_usage = record.tokens_in >= 0 && record.tokens_out >= 0;
        const bool unknown = record.upstream_sent && !complete_usage;
        const uint32_t tokens = complete_usage
            ? static_cast<uint32_t>(std::min<int64_t>(kUsageUnknown - 1,
                  static_cast<int64_t>(record.tokens_in) + record.tokens_out))
            : 0;
        if (!unknown && tokens == 0) return;
        const UtcPeriod period = utc_period();
        if (rule.daily_tokens > 0)
            add_usage(user_states_[user - 1].daily_usage, period.day, tokens, unknown);
        if (rule.monthly_tokens > 0)
            add_usage(user_states_[user - 1].monthly_usage, period.month, tokens, unknown);
    }

    void RoutingPolicy::restore_usage(size_t user_index, uint32_t daily_tokens,
                                      bool daily_unknown, uint32_t monthly_tokens,
                                      bool monthly_unknown) noexcept
    {
        if (!config_.security_enabled || user_index >= config_.users.size()) return;
        const UtcPeriod period = utc_period();
        add_usage(user_states_[user_index].daily_usage, period.day, daily_tokens, daily_unknown);
        add_usage(user_states_[user_index].monthly_usage, period.month, monthly_tokens,
                  monthly_unknown);
    }

    void RoutingPolicy::on_abandon(uint64_t tag, bool upstream_sent) noexcept
    {
        const uint64_t user = tag >> 32;
        if (!config_.security_enabled || user == 0 || user > config_.users.size()) return;
        user_states_[user - 1].concurrent.fetch_sub(1, std::memory_order_acq_rel);
        const UserRule& rule = config_.users[user - 1];
        if (!upstream_sent || (rule.daily_tokens == 0 && rule.monthly_tokens == 0)) return;
        const UtcPeriod period = utc_period();
        if (rule.daily_tokens > 0) add_usage(user_states_[user - 1].daily_usage, period.day, 0, true);
        if (rule.monthly_tokens > 0) add_usage(user_states_[user - 1].monthly_usage, period.month, 0, true);
    }

    void RoutingPolicy::record_failure(size_t index, int64_t now) noexcept
    {
        if (index >= config_.upstream_count) return;
        const uint32_t failures =
            states_[index].failures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failures >= config_.failure_threshold)
        {
            // 连续失败最多放大到基础冷却的 16 倍；并发失败不得缩短已有截止时间。
            const uint32_t step = std::min<uint32_t>(failures - config_.failure_threshold, 4);
            const int64_t until = now + config_.cooldown_ns * (int64_t{1} << step);
            auto& blocked = states_[index].blocked_until_ns;
            int64_t old = blocked.load(std::memory_order_relaxed);
            while (old < until && !blocked.compare_exchange_weak(old, until,
                                                                  std::memory_order_relaxed)) {}
        }
    }

    void RoutingPolicy::record_success(size_t index) noexcept
    {
        if (index >= config_.upstream_count) return;
        states_[index].failures.store(0, std::memory_order_relaxed);
        states_[index].blocked_until_ns.store(0, std::memory_order_relaxed);
    }

    Retry RoutingPolicy::on_failure(const FailureFacts& facts) noexcept
    {
        if (facts.upstream_index < 0 ||
            static_cast<size_t>(facts.upstream_index) >= config_.upstream_count)
            return {};
        const size_t failed = static_cast<size_t>(facts.upstream_index);
        if (!facts.response_received)
        {
            states_[failed].transport_failures.fetch_add(1, std::memory_order_relaxed);
            record_failure(failed, now_ns());
        }
        if (!facts.retryable || facts.attempt + 1 >= config_.max_attempts ||
            config_.upstream_count < 2)
            return {};
        size_t start = (static_cast<size_t>(facts.upstream_index) + 1) % config_.upstream_count;
        if (model_pools_enabled_)
        {
            const ModelPool* pool = pool_for(facts.model);
            if (!pool) return {};
            const auto position = std::find(pool->upstreams.begin(), pool->upstreams.end(), failed);
            if (position == pool->upstreams.end()) return {};
            start = (static_cast<size_t>(position - pool->upstreams.begin()) + 1) % pool->upstreams.size();
        }
        const uint64_t user_id = facts.tag >> 32;
        const UserRule* user = config_.security_enabled && user_id > 0 && user_id <= config_.users.size()
                                   ? &config_.users[user_id - 1] : nullptr;
        if (config_.security_enabled && !user) return {};
        // 第三次尝试不得绕回本请求起始上游；tag 低位保存首次选中索引。
        const uint32_t initial_tag = static_cast<uint32_t>(facts.tag);
        const int first_attempt = facts.attempt > 0 && initial_tag > 0 &&
                                  initial_tag <= config_.upstream_count
                                      ? static_cast<int>(initial_tag - 1) : -1;
        const int selected = pick(start, facts.upstream_index, first_attempt,
                                  now_ns(), facts.model, user);
        if (selected < 0) return {};
        const size_t index = static_cast<size_t>(selected);
        states_[index].attempts.fetch_add(1, std::memory_order_relaxed);
        return {.retry = true,
                .upstream_index = selected,
                .model = model_for(index),
                .credential_header = user ? std::string_view{} : credential_for(index),
                .server_credential = user ? std::string_view(config_.server_credentials[index]) : std::string_view{}};
    }

    Retry RoutingPolicy::on_status_retry(const FailureFacts& facts) noexcept
    {
        if (!config_.allow_unsafe_replay) return {};
        return on_failure(facts);
    }

    void RoutingPolicy::on_reply(const ReplyFacts& facts) noexcept
    {
        if (facts.upstream_index < 0 ||
            static_cast<size_t>(facts.upstream_index) >= config_.upstream_count)
            return;
        const size_t index = static_cast<size_t>(facts.upstream_index);
        if (facts.status >= 200 && facts.status < 300)
            states_[index].responses_2xx.fetch_add(1, std::memory_order_relaxed);
        else if (facts.status >= 400 && facts.status < 500)
            states_[index].responses_4xx.fetch_add(1, std::memory_order_relaxed);
        else if (facts.status >= 500 && facts.status < 600)
            states_[index].responses_5xx.fetch_add(1, std::memory_order_relaxed);
        if (facts.status == 408 || facts.status == 429 || (facts.status >= 500 && facts.status < 600))
            record_failure(index, now_ns());
        else
            record_success(index);
        if ((facts.status == 429 || facts.status == 503) && facts.retry_after_s > 0)
        {
            // 限流期间连熔断探测也禁止；上游给出的等待时间最多采信五分钟。
            const int64_t until = now_ns() +
                static_cast<int64_t>(std::min<uint16_t>(facts.retry_after_s, 300)) * 1'000'000'000LL;
            auto& blocked = states_[index].rate_limited_until_ns;
            int64_t old = blocked.load(std::memory_order_relaxed);
            while (old < until && !blocked.compare_exchange_weak(old, until,
                                                                  std::memory_order_relaxed)) {}
        }
    }

    bool RoutingPolicy::wants_prefix_hash() const noexcept
    {
        return config_.mode == RoutingMode::Sticky;
    }

    size_t RoutingPolicy::available_count() const noexcept
    {
        const int64_t now = now_ns();
        size_t count = 0;
        for (size_t i = 0; i < config_.upstream_count; ++i)
            if (available(i, now)) ++count;
        return count;
    }

    RoutingPolicy::Snapshot RoutingPolicy::snapshot(size_t index) const noexcept
    {
        if (index >= config_.upstream_count) return {};
        const UpstreamState& s = states_[index];
        return {.attempts = s.attempts.load(std::memory_order_relaxed),
                .transport_failures = s.transport_failures.load(std::memory_order_relaxed),
                .responses_2xx = s.responses_2xx.load(std::memory_order_relaxed),
                .responses_4xx = s.responses_4xx.load(std::memory_order_relaxed),
                .responses_5xx = s.responses_5xx.load(std::memory_order_relaxed),
                .available = available(index, now_ns())};
    }

    bool routing_mode_from_name(std::string_view name, RoutingMode& out) noexcept
    {
        if (name == "first") out = RoutingMode::First;
        else if (name == "round_robin") out = RoutingMode::RoundRobin;
        else if (name == "sticky") out = RoutingMode::Sticky;
        else return false;
        return true;
    }
} // namespace llmbridge::app
