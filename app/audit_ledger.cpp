// AI-llm-transplot 扩展：审计线程写带 CRC 的 JSONL，数据面不执行文件 I/O。

#include "audit_ledger.hpp"
#include "provider/json.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string_view>
#include <sys/random.h>
#include <sys/stat.h>
#include <unordered_set>
#include <unistd.h>

namespace llmbridge::app
{
    namespace
    {
        uint32_t crc32(std::string_view text) noexcept
        {
            uint32_t crc = 0xffffffffU;
            for (const unsigned char byte : text)
            {
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
            }
            return ~crc;
        }

        bool valid_line(std::string_view line) noexcept
        {
            constexpr std::string_view marker = ",\"crc32\":\"";
            const size_t at = line.rfind(marker);
            if (at == std::string_view::npos || line.size() - at != marker.size() + 10 ||
                !line.ends_with("\"}")) return false;
            uint32_t expected = 0;
            const char* first = line.data() + at + marker.size();
            const auto [end, ec] = std::from_chars(first, first + 8, expected, 16);
            if (ec != std::errc{} || end != first + 8) return false;
            std::string original(line.substr(0, at));
            original.push_back('}');
            return expected == crc32(original);
        }

        template <class T>
        bool integer_member(const provider::json::Value& object, std::string_view name,
                            T& out) noexcept
        {
            const provider::json::Value* value = object.find(name);
            if (!value || value->type != provider::json::Value::Type::Number || value->sv.empty())
                return false;
            const auto [end, ec] = std::from_chars(value->sv.data(),
                                                   value->sv.data() + value->sv.size(), out);
            return ec == std::errc{} && end == value->sv.data() + value->sv.size();
        }

        bool bool_member(const provider::json::Value& object, std::string_view name,
                         bool& out) noexcept
        {
            const provider::json::Value* value = object.find(name);
            if (!value || value->type != provider::json::Value::Type::Bool) return false;
            out = value->boolean;
            return true;
        }

        struct ReplayKey
        {
            uint64_t instance_hi = 0, instance_lo = 0, request_seq = 0;
            uint32_t attempt = 0;
            bool operator==(const ReplayKey&) const = default;
        };

        struct ReplayKeyHash
        {
            size_t operator()(const ReplayKey& key) const noexcept
            {
                size_t h = static_cast<size_t>(key.instance_hi ^ key.instance_lo);
                h ^= static_cast<size_t>(key.request_seq + 0x9e3779b97f4a7c15ULL +
                                         (static_cast<uint64_t>(h) << 6) + (h >> 2));
                return h ^ (static_cast<size_t>(key.attempt) * 0x9e3779b9U);
            }
        };

        struct ReplayRecord
        {
            ReplayKey key;
            std::string_view tenant, user;
            int64_t accounted_at_ns = 0;
            int32_t tokens_in = -1, tokens_out = -1;
            bool upstream_sent = false;
        };

        bool parse_replay_record(std::string_view line, ReplayRecord& out) noexcept
        {
            bool ok = false;
            const provider::json::Value root = provider::json::parse(line, ok);
            if (!ok || !root.is_object()) return false;
            int schema = 0;
            int64_t wall_t0_ns = 0;
            const provider::json::Value* recorded = root.find("recorded_at_ns");
            if (!integer_member(root, "schema", schema) || schema != 1 ||
                !integer_member(root, "request_seq", out.key.request_seq) ||
                !integer_member(root, "attempt", out.key.attempt) ||
                !integer_member(root, "wall_t0_ns", wall_t0_ns) || wall_t0_ns < 0 ||
                !integer_member(root, "tokens_in", out.tokens_in) || out.tokens_in < -1 ||
                !integer_member(root, "tokens_out", out.tokens_out) || out.tokens_out < -1 ||
                !bool_member(root, "upstream_sent", out.upstream_sent)) return false;
            out.accounted_at_ns = wall_t0_ns;
            if (recorded)
            {
                if (!integer_member(root, "recorded_at_ns", out.accounted_at_ns) ||
                    out.accounted_at_ns < 0) return false;
            }
            const provider::json::Value* instance = root.find("instance_id");
            const provider::json::Value* tenant = root.find("tenant_id");
            const provider::json::Value* user = root.find("user_id");
            if (!instance || instance->type != provider::json::Value::Type::String ||
                instance->sv.size() != 32 ||
                !tenant || tenant->type != provider::json::Value::Type::String ||
                tenant->sv.size() > 64 || tenant->sv.find('\\') != std::string_view::npos ||
                !user || user->type != provider::json::Value::Type::String ||
                user->sv.size() > 64 || user->sv.find('\\') != std::string_view::npos)
                return false;
            const auto parse_hex = [](std::string_view value, uint64_t& number) noexcept {
                const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(),
                                                       number, 16);
                return ec == std::errc{} && end == value.data() + value.size();
            };
            if (!parse_hex(instance->sv.substr(0, 16), out.key.instance_hi) ||
                !parse_hex(instance->sv.substr(16), out.key.instance_lo)) return false;
            out.tenant = tenant->sv;
            out.user = user->sv;
            if (!out.upstream_sent && (out.tokens_in >= 0 || out.tokens_out >= 0)) return false;
            return true;
        }

        struct UtcPeriod { int64_t day; int month; };

        UtcPeriod period_at(int64_t value_ns) noexcept
        {
            using namespace std::chrono;
            const sys_time<std::chrono::nanoseconds> time{std::chrono::nanoseconds{value_ns}};
            const sys_days day = floor<days>(time);
            const year_month_day ymd{day};
            return {day.time_since_epoch().count(), int(ymd.year()) * 12 + int(unsigned(ymd.month()))};
        }

        uint32_t saturating_add(uint32_t current, uint32_t add) noexcept
        {
            constexpr uint32_t maximum = (uint32_t{1} << 31) - 1;
            return static_cast<uint32_t>(std::min<uint64_t>(maximum,
                                                            uint64_t{current} + add));
        }

        void json_string(std::string& out, std::string_view value)
        {
            out.push_back('"');
            constexpr char hex[] = "0123456789abcdef";
            for (const unsigned char ch : value)
            {
                if (ch == '"' || ch == '\\')
                {
                    out.push_back('\\');
                    out.push_back(static_cast<char>(ch));
                }
                else if (ch >= 0x20 && ch < 0x7f) out.push_back(static_cast<char>(ch));
                else
                {
                    out += "\\u00";
                    out.push_back(hex[ch >> 4]);
                    out.push_back(hex[ch & 15]);
                }
            }
            out.push_back('"');
        }

        template <class T>
        void number(std::string& out, T value)
        {
            char b[32];
            const auto [end, ec] = std::to_chars(b, b + sizeof b, value);
            if (ec == std::errc{}) out.append(b, end);
        }

        template <class T>
        void field(std::string& out, std::string_view name, T value)
        {
            out += ",\"";
            out += name;
            out += "\":";
            number(out, value);
        }

        const char* outcome_name(AttemptOutcome outcome) noexcept
        {
            switch (outcome)
            {
            case AttemptOutcome::Response: return "response";
            case AttemptOutcome::TransportError: return "transport_error";
            case AttemptOutcome::ClientAbandoned: return "client_abandoned";
            case AttemptOutcome::StreamTruncated: return "stream_truncated";
            case AttemptOutcome::HedgeCut: return "hedge_cut";
            case AttemptOutcome::PolicyCut: return "policy_cut";
            }
            return "unknown";
        }

        const char* cost_name(CostStatus status) noexcept
        {
            switch (status)
            {
            case CostStatus::Known: return "known";
            case CostStatus::PriceMissing: return "price_missing";
            case CostStatus::UsageUnknown: return "usage_unknown";
            case CostStatus::TimestampUnknown: return "timestamp_unknown";
            case CostStatus::InvalidUsage: return "invalid_usage";
            case CostStatus::Overflow: return "overflow";
            }
            return "unknown";
        }

        template <size_t N>
        void append_text(std::string& out, std::string_view name, const char (&data)[N], uint16_t size)
        {
            out += ",\"";
            out += name;
            out += "\":";
            json_string(out, std::string_view(data, size));
        }

        bool write_all(int fd, std::string_view text) noexcept
        {
            while (!text.empty())
            {
                const ssize_t n = ::write(fd, text.data(), text.size());
                if (n > 0) text.remove_prefix(static_cast<size_t>(n));
                else if (n < 0 && errno == EINTR) continue;
                else return false;
            }
            return true;
        }
    } // namespace

    AuditLedger::AuditLedger(std::string path, const PriceTable& prices,
                             std::vector<std::string> provider_ids, std::vector<AuditUser> users)
        : path_(std::move(path)), prices_(prices), provider_ids_(std::move(provider_ids)),
          users_(std::move(users))
    {
        if (path_.empty()) throw std::invalid_argument("audit ledger path is empty");
        replayed_usage_.resize(users_.size());
        unsigned char random[16];
        size_t random_bytes = 0;
        while (random_bytes < sizeof random)
        {
            const ssize_t n = ::getrandom(random + random_bytes, sizeof random - random_bytes, 0);
            if (n > 0) random_bytes += static_cast<size_t>(n);
            else if (n < 0 && errno == EINTR) continue;
            else throw std::runtime_error("cannot generate audit ledger instance id");
        }
        constexpr char hex[] = "0123456789abcdef";
        instance_id_.resize(sizeof random * 2);
        for (size_t i = 0; i < sizeof random; ++i)
        {
            instance_id_[i * 2] = hex[random[i] >> 4];
            instance_id_[i * 2 + 1] = hex[random[i] & 15];
        }
        fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ < 0) throw std::runtime_error("cannot open audit ledger: " + std::string(std::strerror(errno)));
        try
        {
            recover();
            for (size_t i = 0; i < kCapacity; ++i)
                queue_[i].sequence.store(i, std::memory_order_relaxed);
            thread_ = std::thread([this] { writer_loop(); });
        }
        catch (...)
        {
            ::close(fd_);
            fd_ = -1;
            throw;
        }
    }

    AuditLedger::~AuditLedger()
    {
        stopping_.store(true, std::memory_order_release);
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) ::close(fd_);
    }

    void AuditLedger::recover()
    {
        struct stat st{};
        if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode))
            throw std::runtime_error("audit ledger is not a regular file");
        if (st.st_uid != ::geteuid() || (st.st_mode & 0777) != 0600)
            throw std::runtime_error("audit ledger must be owned by this user and have mode 0600");
        std::string pending;
        pending.reserve(8192);
        char block[8192];
        off_t offset = 0, valid_end = 0;
        const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch()).count();
        const UtcPeriod current = period_at(now_ns);
        std::unordered_set<ReplayKey, ReplayKeyHash> seen;
        while (offset < st.st_size)
        {
            const ssize_t n = ::pread(fd_, block, sizeof block, offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("cannot read audit ledger");
            offset += n;
            pending.append(block, static_cast<size_t>(n));
            if (pending.size() > 65536 && pending.find('\n') == std::string::npos)
                throw std::runtime_error("audit ledger record exceeds 64 KiB");
            for (;;)
            {
                const size_t newline = pending.find('\n');
                if (newline == std::string::npos) break;
                if (newline > 65536) throw std::runtime_error("audit ledger record exceeds 64 KiB");
                if (!valid_line(std::string_view(pending).substr(0, newline)))
                    throw std::runtime_error("audit ledger checksum mismatch");
                ReplayRecord record;
                if (!parse_replay_record(std::string_view(pending).substr(0, newline), record))
                    throw std::runtime_error("audit ledger record is invalid");
                const UtcPeriod period = period_at(record.accounted_at_ns);
                if (period.month == current.month)
                {
                    if (!seen.insert(record.key).second)
                        throw std::runtime_error("audit ledger contains a duplicate attempt");
                    size_t user_index = users_.size();
                    for (size_t i = 0; i < users_.size(); ++i)
                        if (users_[i].tenant_id == record.tenant && users_[i].user_id == record.user)
                        {
                            user_index = i;
                            break;
                        }
                    if (user_index < users_.size())
                    {
                        ReplayedUsage& usage = replayed_usage_[user_index];
                        const bool known = record.tokens_in >= 0 && record.tokens_out >= 0;
                        const bool unknown = record.upstream_sent && !known;
                        const uint32_t tokens = known
                            ? static_cast<uint32_t>(std::min<int64_t>((int64_t{1} << 31) - 1,
                                  static_cast<int64_t>(record.tokens_in) + record.tokens_out))
                            : 0;
                        usage.monthly_tokens = saturating_add(usage.monthly_tokens, tokens);
                        usage.monthly_unknown = usage.monthly_unknown || unknown;
                        if (period.day == current.day)
                        {
                            usage.daily_tokens = saturating_add(usage.daily_tokens, tokens);
                            usage.daily_unknown = usage.daily_unknown || unknown;
                        }
                    }
                }
                valid_end += static_cast<off_t>(newline + 1);
                pending.erase(0, newline + 1);
            }
        }
        // 进程可能死在一次追加中间；只有无换行的尾部可安全丢弃，完整坏记录拒绝启动。
        if (!pending.empty() && ::ftruncate(fd_, valid_end) != 0)
            throw std::runtime_error("cannot truncate partial audit ledger tail");
        if (::lseek(fd_, 0, SEEK_END) < 0) throw std::runtime_error("cannot seek audit ledger");
    }

    void AuditLedger::on_attempt(const AttemptRecord& r) noexcept
    {
        if (!healthy_.load(std::memory_order_acquire)) return;
        Entry entry;
        entry.record = r;
        entry.recorded_at_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch()).count();
        const auto copy_text = []<size_t N>(Text<N>& out, std::string_view value) noexcept {
            const size_t n = std::min(value.size(), N - 1);
            if (n > 0) std::memcpy(out.data, value.data(), n);
            out.size = static_cast<uint16_t>(n);
        };
        const size_t provider = r.upstream_index >= 0 ? static_cast<size_t>(r.upstream_index) : provider_ids_.size();
        const uint64_t tagged_user = r.tag >> 32;
        if (provider < provider_ids_.size()) copy_text(entry.provider, provider_ids_[provider]);
        if (tagged_user > 0 && tagged_user <= users_.size())
        {
            copy_text(entry.tenant, users_[tagged_user - 1].tenant_id);
            copy_text(entry.user, users_[tagged_user - 1].user_id);
        }
        copy_text(entry.requested_model, r.requested_model);
        copy_text(entry.model, r.model);
        copy_text(entry.served_model, r.served_model);
        copy_text(entry.venue_req_id, r.venue_req_id);
        const CostQuote quote = provider < provider_ids_.size()
                              ? prices_.quote(provider_ids_[provider], r) : CostQuote{};
        entry.cost_status = quote.status;
        entry.cost_pdollars = quote.cost_pdollars;
        copy_text(entry.price_version, quote.price_version);
        copy_text(entry.prior_price_version, quote.prior_price_version);
        copy_text(entry.price_source, quote.price_source);
        copy_text(entry.prior_price_source, quote.prior_price_source);

        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        Slot* slot = nullptr;
        for (;;)
        {
            slot = &queue_[pos % kCapacity];
            const size_t sequence = slot->sequence.load(std::memory_order_acquire);
            const intptr_t difference = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(pos);
            if (difference == 0)
            {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            }
            else if (difference < 0)
            {
                healthy_.store(false, std::memory_order_release);
                return;
            }
            else pos = enqueue_pos_.load(std::memory_order_relaxed);
        }
        if (!slot)
        {
            healthy_.store(false, std::memory_order_release);
            return;
        }
        slot->entry = entry;
        slot->sequence.store(pos + 1, std::memory_order_release);
        queued_.fetch_add(1, std::memory_order_release);
        cv_.notify_one();
    }

    bool AuditLedger::healthy() const noexcept { return healthy_.load(std::memory_order_acquire); }
    uint64_t AuditLedger::written() const noexcept { return written_.load(std::memory_order_relaxed); }

    bool AuditLedger::append(const Entry& e) noexcept
    {
        try
        {
            const AttemptRecord& r = e.record;
            std::string line;
            line.reserve(1536);
            line = "{\"schema\":1,\"instance_id\":";
            json_string(line, instance_id_);
            field(line, "request_seq", r.request_seq);
            field(line, "wall_t0_ns", r.wall_t0_ns);
            field(line, "recorded_at_ns", e.recorded_at_ns);
            field(line, "attempt", r.attempt);
            append_text(line, "tenant_id", e.tenant.data, e.tenant.size);
            append_text(line, "user_id", e.user.data, e.user.size);
            append_text(line, "provider_id", e.provider.data, e.provider.size);
            field(line, "upstream_index", r.upstream_index);
            field(line, "status", r.status);
            line += ",\"outcome\":";
            json_string(line, outcome_name(r.outcome));
            line += ",\"upstream_sent\":";
            line += r.upstream_sent ? "true" : "false";
            line += ",\"response_received\":";
            line += r.response_received ? "true" : "false";
            line += ",\"retrying\":";
            line += r.retrying ? "true" : "false";
            append_text(line, "requested_model", e.requested_model.data, e.requested_model.size);
            append_text(line, "model", e.model.data, e.model.size);
            append_text(line, "served_model", e.served_model.data, e.served_model.size);
            append_text(line, "provider_request_id", e.venue_req_id.data, e.venue_req_id.size);
            field(line, "tokens_in", r.tokens_in);
            field(line, "tokens_out", r.tokens_out);
            field(line, "cached_tokens", r.cached_tokens);
            field(line, "cache_write_tokens", r.cache_write_tokens);
            field(line, "cache_write_5m_tokens", r.cache_write_5m_tokens);
            field(line, "cache_write_1h_tokens", r.cache_write_1h_tokens);
            field(line, "reasoning_tokens", r.reasoning_tokens);
            field(line, "audio_in_tokens", r.audio_in_tokens);
            field(line, "audio_out_tokens", r.audio_out_tokens);
            field(line, "accepted_prediction_tokens", r.accepted_prediction_tokens);
            field(line, "rejected_prediction_tokens", r.rejected_prediction_tokens);
            field(line, "tool_prompt_tokens", r.tool_prompt_tokens);
            field(line, "image_in_tokens", r.image_in_tokens);
            field(line, "web_search_requests", r.web_search_requests);
            field(line, "web_fetch_requests", r.web_fetch_requests);
            field(line, "code_exec_requests", r.code_exec_requests);
            field(line, "file_search_calls", r.file_search_calls);
            field(line, "venue_cost_pdollars", r.venue_cost_pdollars);
            field(line, "compaction_in_tokens", r.compaction_in_tokens);
            field(line, "compaction_out_tokens", r.compaction_out_tokens);
            field(line, "compaction_cached_tokens", r.compaction_cached_tokens);
            field(line, "prior_in_tokens", r.prior_in_tokens);
            field(line, "prior_out_tokens", r.prior_out_tokens);
            field(line, "prior_cached_tokens", r.prior_cached_tokens);
            field(line, "prior_cache_write_tokens", r.prior_cache_write_tokens);
            field(line, "prior_attempts", r.prior_attempts);
            line += ",\"cost_status\":";
            json_string(line, cost_name(e.cost_status));
            field(line, "calculated_cost_pdollars", e.cost_pdollars);
            append_text(line, "price_version", e.price_version.data, e.price_version.size);
            append_text(line, "price_source", e.price_source.data, e.price_source.size);
            append_text(line, "prior_price_version", e.prior_price_version.data,
                        e.prior_price_version.size);
            append_text(line, "prior_price_source", e.prior_price_source.data,
                        e.prior_price_source.size);
            line.push_back('}');
            const uint32_t checksum = crc32(line);
            line.pop_back();
            char suffix[32];
            const int n = std::snprintf(suffix, sizeof suffix, ",\"crc32\":\"%08x\"}\n", checksum);
            line.append(suffix, static_cast<size_t>(n));
            return write_all(fd_, line) && ::fdatasync(fd_) == 0;
        }
        catch (...)
        {
            return false;
        }
    }

    void AuditLedger::writer_loop() noexcept
    {
        for (;;)
        {
            Entry entry;
            {
                std::unique_lock<std::mutex> lock(wait_mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                    return stopping_.load(std::memory_order_acquire) ||
                           queued_.load(std::memory_order_acquire) > 0;
                });
                if (queued_.load(std::memory_order_acquire) == 0 &&
                    stopping_.load(std::memory_order_acquire)) return;
            }
            Slot& slot = queue_[dequeue_pos_ % kCapacity];
            if (slot.sequence.load(std::memory_order_acquire) != dequeue_pos_ + 1) continue;
            entry = slot.entry;
            slot.sequence.store(dequeue_pos_ + kCapacity, std::memory_order_release);
            ++dequeue_pos_;
            queued_.fetch_sub(1, std::memory_order_release);
            if (!append(entry)) healthy_.store(false, std::memory_order_release);
            else written_.fetch_add(1, std::memory_order_relaxed);
        }
    }
} // namespace llmbridge::app
