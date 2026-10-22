// AI-llm-transplot 扩展：审计线程写带 CRC 的 JSONL，数据面不执行文件 I/O。

#include "audit_ledger.hpp"

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
