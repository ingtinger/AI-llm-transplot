// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "options.hpp"

#include "provider/dialect.hpp"
#include "provider/json.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <unordered_set>

namespace llmbridge::app
{
    namespace
    {
        namespace json = llmbridge::provider::json;
        constexpr size_t kMaxConfigBytes = 4 * 1024 * 1024;

        constexpr std::string_view kBackends[] = {"auto", "epoll", "uring"};
        constexpr std::string_view kOnOff[] = {"on", "off"};
        constexpr std::string_view kLevels[] = {"trace", "debug", "info", "warn", "error", "off"};
        constexpr std::string_view kRoutingModes[] = {"first", "round_robin", "sticky"};
        constexpr double kYear = 31536000; // a bigger timeout is a unit typo, not a policy

        using K = Kind;
        using S = Settings;
        const Option kOptions[] = {
            {"--listen", "listen.port", "PORT", "listening port", K::Int, 0, 65535, {}, &S::listen_port},
            {"--listen-tls", "listen.tls", "", "serve TLS only; needs a certificate and a key",
             K::Bool, 0, 0, {}, &S::listen_tls},
            {"--tls-cert", "listen.cert", "PATH", "certificate chain, PEM", K::Text, 0, 0, {}, &S::tls_cert},
            {"--tls-key", "listen.key", "PATH", "private key, PEM, owner-only", K::Text, 0, 0, {}, &S::tls_key},
            {"--upstream", "upstream.url", "URL", "IP:PORT, HOST:PORT or http(s)://HOST[:PORT][/BASE]",
             K::Text, 0, 0, {}, &S::upstream},
            {"", "upstream.id", "", "stable Provider identity used by accounting",
             K::Text, 0, 0, {}, &S::upstream_id},
            {"--upstream-dialect", "upstream.dialect", "NAME", "what the venue speaks", K::Choice, 0, 0,
             provider::venue_names(), &S::dialect},
            {"", "upstream.strip_headers", "", "header names dropped from every upstream request",
             K::List, 0, 0, {}, &S::strip_headers},
            {"", "upstream.credential_header", "", "client header containing this venue's key",
             K::Text, 0, 0, {}, &S::upstream_credential_header},
            {"", "upstream.credential_env", "", "environment variable holding this venue's key",
             K::Text, 0, 0, {}, &S::upstream_credential_env},
            {"", "upstream.model", "", "model name sent to this venue; empty keeps the client model",
             K::Text, 0, 0, {}, &S::upstream_model},
            {"", "upstream.models", "", "client model names this venue can serve",
             K::List, 0, 0, {}, &S::upstream_models},
            {"--legacy-mode", "security.legacy_mode", "", "allow unauthenticated local compatibility mode",
             K::Bool, 0, 0, {}, &S::legacy_mode},
            {"--routing-mode", "routing.mode", "MODE", "built-in upstream selection policy",
             K::Choice, 0, 0, kRoutingModes, &S::routing_mode},
            {"--routing-max-attempts", "routing.max_attempts", "N",
             "maximum venues tried for one request", K::Int, 1, 64, {}, &S::routing_max_attempts},
            {"", "routing.failure_threshold", "", "failures before a venue enters cooldown",
             K::Int, 1, 1000000, {}, &S::routing_failure_threshold},
            {"", "routing.cooldown_s", "", "seconds a failed venue is skipped",
             K::Number, 0, kYear, {}, &S::routing_cooldown_s},
            {"", "routing.sticky_header", "", "header used by sticky routing",
             K::Text, 0, 0, {}, &S::routing_sticky_header},
            {"", "routing.drained_upstreams", "", "upstream indices excluded from new requests; SIGHUP reloadable",
             K::List, 0, 0, {}, &S::routing_drained_upstreams},
            {"", "routing.allow_unsafe_replay", "", "allow retry after a request may have run upstream",
             K::Bool, 0, 0, {}, &S::routing_allow_unsafe_replay},
            {"--management-port", "management.port", "PORT",
             "loopback-only health, readiness and Prometheus metrics; 0 = off",
             K::Int, 0, 65535, {}, &S::management_port},
            {"--upstream-timeout", "timeouts.upstream_s", "SECONDS",
             "upstream silence before a request is aborted, 0 = off", K::Number, 0, kYear, {},
             &S::upstream_s},
            {"--client-idle", "timeouts.client_idle_s", "SECONDS", "idle client lifetime, 0 = off",
             K::Number, 0, kYear, {}, &S::client_idle_s},
            {"--pool-idle", "timeouts.pool_idle_s", "SECONDS", "idle pooled upstream lifetime, 0 = off",
             K::Number, 0, kYear, {}, &S::pool_idle_s},
            {"--connect-timeout", "timeouts.connect_s", "SECONDS",
             "upstream connect and TLS handshake limit, 0 = off", K::Number, 0, kYear, {},
             &S::connect_s},
            {"--body-timeout", "timeouts.body_s", "SECONDS",
             "a streamed body's whole upload, from its head, 0 = off", K::Number, 0, kYear, {},
             &S::body_s},
            {"--body-grace", "timeouts.body_grace_s", "SECONDS",
             "a streamed body's start, before --body-min-rate applies", K::Number, 0, kYear, {},
             &S::body_grace_s},
            {"--body-min-rate", "timeouts.body_min_rate", "BYTES",
             "a streamed body's least average upload in bytes a second, past its grace, 0 = off",
             K::Int, 0, 1 << 30, {}, &S::body_min_rate},
            {"--io", "runtime.io", "NAME", "event loop", K::Choice, 0, 0, kBackends, &S::io},
            {"--log-level", "runtime.log_level", "LEVEL", "log level", K::Choice, 0, 0, kLevels,
             &S::log_level},
            {"--request-log", "runtime.request_log", "",
             "emit bounded structured request logs on a dedicated thread",
             K::Bool, 0, 0, {}, &S::request_log},
            {"--workers", "runtime.workers", "N", "event loops sharing the port", K::Int, 1, 4096, {},
             &S::workers},
            {"--timing-headers", "runtime.timing_headers", "", "add x-llmbridge-* timing headers",
             K::Bool, 0, 0, {}, &S::timing_headers},
            {"--duration", "runtime.duration_s", "SECONDS",
             "stop after this long and print the profile, 0 = until signalled", K::Number, 0, kYear,
             {}, &S::duration_s},
            {"--warmup", "runtime.warmup_s", "SECONDS", "leave the first seconds out of the histograms",
             K::Number, 0, kYear, {}, &S::warmup_s},
            {"--prefault-mb", "runtime.prefault_mb", "MB", "prefault every request buffer to this size",
             K::Number, 0, 4096, {}, &S::prefault_mb},
            {"--retain-mb", "runtime.retain_mb", "MB",
             "JSON parse storage a worker keeps between bodies, up to the 16 MB body cap", K::Number,
             0, 16, {}, &S::retain_mb},
            {"--stream-body", "runtime.stream_body", "MODE",
             "forward a byte-forwarded body as it arrives; off holds every one to its last byte",
             K::Choice, 0, 0, kOnOff, &S::stream_body},
            {"--stream-body-max", "runtime.stream_body_max", "N",
             "bodies a worker forwards as they arrive at once; past it one is held, 0 = no cap",
             K::Int, 0, 1 << 20, {}, &S::stream_body_max},
            {"--stream-openai-body", "runtime.stream_openai_body", "MODE",
             "forward an OpenAI client's body as it arrives, each chunk checked first", K::Choice, 0, 0,
             kOnOff, &S::stream_openai_body},
        };

        // Old spellings are refused by name: an alias would keep the old model alive.
        struct Retired
        {
            std::string_view name, why;
        };
        constexpr Retired kRetired[] = {
            {"--translate", "is now --upstream-dialect, and names what the venue speaks instead "
                            "of an action we may not perform"},
            {"upstream.translate", "is now \"dialect\", and names what the venue speaks"},
        };
        constexpr std::string_view kNoneIsOpenAi =
            " \"none\" is now \"openai\": it names an OpenAI-compatible venue, never an absence";

        bool fail(std::string& err, std::string msg)
        {
            err = std::move(msg);
            return false;
        }

        Cli refuse(std::string& err, std::string msg)
        {
            err = std::move(msg);
            return Cli::Refused;
        }

        std::string_view retired(std::string_view name)
        {
            for (const Retired& r : kRetired)
                if (r.name == name) return r.why;
            return {};
        }

        bool is_comment(std::string_view k) { return !k.empty() && k.front() == '_'; }

        bool header_name_ok(std::string_view name) noexcept
        {
            if (name.empty() || name.size() > 64) return false;
            for (const char c : name)
            {
                const bool alpha_num = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                       (c >= '0' && c <= '9');
                if (!alpha_num && std::string_view("!#$%&'*+-.^_`|~").find(c) == std::string_view::npos)
                    return false;
            }
            return true;
        }

        bool sticky_header_name_ok(std::string_view name) noexcept
        {
            if (name.empty() || name.size() > 64) return false;
            for (const char c : name)
            {
                const bool alpha_num = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                       (c >= '0' && c <= '9');
                if (!alpha_num && c != '-' && c != '_') return false;
            }
            return true;
        }

        bool stable_id_ok(std::string_view value) noexcept
        {
            if (value.empty() || value.size() > 64) return false;
            for (const char c : value)
            {
                const bool alpha_num = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                       (c >= '0' && c <= '9');
                if (!alpha_num && c != '-' && c != '_' && c != '.') return false;
            }
            return true;
        }

        std::string bounds(double v)
        {
            char b[32];
            const auto [p, ec] = std::to_chars(b, b + sizeof b, v);
            return ec == std::errc{} ? std::string(b, p) : std::string();
        }

        /// One value, as text, from either source. `where` names it the way the source does.
        bool set_text(const Option& o, std::string_view v, Settings& s, const std::string& where,
                      std::string& err)
        {
            const auto range = [&](double d) {
                return (d >= o.lo && d <= o.hi) ||
                       fail(err, where + " out of range (allowed " +
                                     bounds(o.lo) + ".." + bounds(o.hi) + ")");
            };
            if (o.kind == Kind::Int)
            {
                long long n = 0;
                const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
                if (ec == std::errc::result_out_of_range) return range(o.hi + 1);
                if (v.empty() || ec != std::errc{} || p != v.data() + v.size())
                    return fail(err, where + " must be an integer");
                if (!range(static_cast<double>(n))) return false;
                s.*std::get<int Settings::*>(o.field) = static_cast<int>(n);
                return true;
            }
            if (o.kind == Kind::Number)
            {
                // strtod over a bounded copy: from_chars for double is missing on some libstdc++.
                const std::string t(v);
                char* end = nullptr;
                const bool blank = t.empty() || std::isspace(static_cast<unsigned char>(t[0]));
                const double d = blank ? 0 : std::strtod(t.c_str(), &end);
                if (end == nullptr || end == t.c_str() || *end != '\0')
                    return fail(err, where + " is not a valid number");
                if (!range(d)) return false; // NaN fails both comparisons
                s.*std::get<double Settings::*>(o.field) = d;
                return true;
            }
            if (o.kind == Kind::Choice)
            {
                if (o.key == "upstream.dialect" && v == "none")
                    return fail(err, where + std::string(kNoneIsOpenAi));
                bool known = false;
                for (std::string_view c : o.choices) known = known || c == v;
                if (!known)
                {
                    std::string m = where + " must be one of:";
                    for (std::string_view c : o.choices) m += " " + std::string(c);
                    return fail(err, m);
                }
            }
            if (o.key == "routing.sticky_header" && !sticky_header_name_ok(v))
                return fail(err, where + " must be a 1..64 byte HTTP header name");
            s.*std::get<std::string Settings::*>(o.field) = std::string(v);
            return true;
        }

        /// Strings are copied out, so the caller may drop the zero-copy DOM. A value still
        /// holds JSON escapes; settings are paths, URLs and words, so `\` is refused.
        bool set_json(const Option& o, const json::Value& v, Settings& s, std::string& err)
        {
            const std::string where = "config: \"" + std::string(o.key) + "\"";
            const auto plain = [&](std::string_view t) {
                return t.find('\\') == std::string_view::npos ||
                       fail(err, where + " must not contain a backslash escape");
            };
            switch (o.kind)
            {
            case Kind::Bool:
                if (v.type != json::Value::Type::Bool)
                    return fail(err, where + " must be true or false");
                s.*std::get<bool Settings::*>(o.field) = v.boolean;
                return true;
            case Kind::Int:
            case Kind::Number:
                if (v.type != json::Value::Type::Number) return fail(err, where + " must be a number");
                return set_text(o, v.sv, s, where, err);
            case Kind::Text:
            case Kind::Choice:
                if (v.type != json::Value::Type::String) return fail(err, where + " must be a string");
                return plain(v.sv) && set_text(o, v.sv, s, where, err);
            case Kind::List:
                break;
            }
            // A bare string where a list was meant must not become a silent no-op.
            if (!v.is_array()) return fail(err, where + " must be an array of strings");
            if ((o.key == "upstream.strip_headers" || o.key == "routing.drained_upstreams") &&
                v.arr.size() > 64)
                return fail(err, where + " supports at most 64 entries");
            std::vector<std::string> out;
            std::unordered_set<std::string> strip_names;
            for (const json::Value& e : v.arr)
            {
                if (e.type != json::Value::Type::String)
                    return fail(err, where + " must contain only strings");
                if (e.sv.empty()) return fail(err, where + " must not contain an empty string");
                if (!plain(e.sv)) return false;
                if (o.key == "upstream.strip_headers")
                {
                    if (!header_name_ok(e.sv))
                        return fail(err, where + " has an invalid header name");
                    std::string normalized(e.sv);
                    for (char& c : normalized)
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    if (!strip_names.insert(std::move(normalized)).second)
                        return fail(err, where + " contains a duplicate header name");
                }
                if (o.key == "routing.drained_upstreams" && e.sv.size() > 20)
                    return fail(err, where + " has an index longer than 20 bytes");
                out.emplace_back(e.sv);
            }
            s.*std::get<std::vector<std::string> Settings::*>(o.field) = std::move(out);
            return true;
        }

        bool apply_group(std::string_view group, const json::Value& g, Settings& s, std::string& err)
        {
            if (!g.is_object())
                return fail(err, "config: \"" + std::string(group) + "\" must be an object");
            for (const auto& [k, v] : g.obj)
            {
                if (is_comment(k)) continue;
                const std::string key = std::string(group) + "." + std::string(k);
                const Option* o = nullptr;
                for (const Option& c : kOptions)
                    if (c.key == key) o = &c;
                if (!o && !retired(key).empty())
                    return fail(err, "config: \"" + std::string(k) + "\" " + std::string(retired(key)));
                if (!o)
                    return fail(err, "config: unknown key in \"" + std::string(group) + "\"");
                if (!set_json(*o, v, s, err)) return false;
            }
            return true;
        }

        /// One object, or an array of the same object for several venues. strip_headers is
        /// gateway-wide, so every entry that sets it must agree.
        bool apply_upstream(const json::Value& g, Settings& s, std::string& err)
        {
            if (!g.is_object() && !g.is_array())
                return fail(err, "config: \"upstream\" must be an object or an array of them");
            if (g.is_array() && g.arr.empty())
                return fail(err, "config: \"upstream\" must not be an empty array");
            if (g.is_array() && g.arr.size() > 64)
                return fail(err, "config: \"upstream\" supports at most 64 entries");
            const size_t n = g.is_array() ? g.arr.size() : 1;
            bool strip = false;
            bool has_models = false, missing_models = false;
            for (size_t i = 0; i < n; ++i)
            {
                const json::Value& e = g.is_array() ? g.arr[i] : g;
                if (!e.is_object())
                    return fail(err, "config: each entry in \"upstream\" must be an object");
                Settings one;
                one.upstream_id.clear();
                one.upstream.clear();
                one.dialect.clear();
                one.upstream_credential_header.clear();
                one.upstream_credential_env.clear();
                one.upstream_model.clear();
                one.upstream_models.clear();
                if (!apply_group("upstream", e, one, err)) return false;
                if (!one.upstream_id.empty() && !stable_id_ok(one.upstream_id))
                    return fail(err, "config: upstream.id must be a 1..64 byte stable identifier");
                if (e.find("models") != nullptr)
                {
                    has_models = true;
                    if (one.upstream_models.empty() || one.upstream_models.size() > 64)
                        return fail(err, "config: upstream.models must contain 1..64 names");
                    std::unordered_set<std::string_view> seen;
                    for (const auto& model : one.upstream_models)
                        if (!substitute_ok(model, 64) || !seen.insert(model).second)
                            return fail(err, "config: upstream.models contains an invalid or duplicate model");
                }
                else missing_models = true;
                if (g.is_array() && one.upstream.empty())
                    return fail(err, "config: every entry in an \"upstream\" array needs a url");
                if (e.find("strip_headers") != nullptr)
                {
                    if (strip && one.strip_headers != s.strip_headers)
                        return fail(err, "config: \"upstream.strip_headers\" is gateway-wide; every "
                                         "entry that sets it must list the same headers");
                    strip = true;
                    s.strip_headers = std::move(one.strip_headers);
                }
                if (i > 0)
                    s.more_upstreams.push_back(
                        {std::move(one.upstream_id), std::move(one.upstream),
                         one.dialect.empty() ? "openai" : one.dialect,
                         std::move(one.upstream_credential_header), std::move(one.upstream_model),
                         std::move(one.upstream_credential_env), std::move(one.upstream_models)});
                if (i == 0) s.upstream_id = std::move(one.upstream_id);
                if (i == 0 && !one.upstream.empty()) s.upstream = std::move(one.upstream);
                if (i == 0 && !one.dialect.empty()) s.dialect = std::move(one.dialect);
                if (i == 0) s.upstream_credential_header = std::move(one.upstream_credential_header);
                if (i == 0) s.upstream_credential_env = std::move(one.upstream_credential_env);
                if (i == 0) s.upstream_model = std::move(one.upstream_model);
                if (i == 0) s.upstream_models = std::move(one.upstream_models);
            }
            if (has_models && missing_models)
                return fail(err, "config: every upstream must set models when model pools are enabled");
            return true;
        }

        bool parse_nonnegative_i64(const json::Value& value, int64_t& out) noexcept
        {
            if (value.type != json::Value::Type::Number || value.sv.empty()) return false;
            const auto parsed =
                std::from_chars(value.sv.data(), value.sv.data() + value.sv.size(), out);
            return parsed.ec == std::errc{} && parsed.ptr == value.sv.data() + value.sv.size() &&
                   out >= 0;
        }

        bool apply_price_rates(const json::Value& value, PriceRates& rates, std::string& err)
        {
            if (!value.is_object())
                return fail(err, "config: accounting.prices.rates must be an object");
            struct RateField
            {
                std::string_view name;
                int64_t PriceRates::*member;
            };
            constexpr RateField fields[] = {
                {"input", &PriceRates::input},
                {"output", &PriceRates::output},
                {"cached_input", &PriceRates::cached_input},
                {"cache_write", &PriceRates::cache_write},
                {"cache_write_5m", &PriceRates::cache_write_5m},
                {"cache_write_1h", &PriceRates::cache_write_1h},
                {"reasoning", &PriceRates::reasoning},
                {"audio_input", &PriceRates::audio_input},
                {"audio_output", &PriceRates::audio_output},
                {"accepted_prediction", &PriceRates::accepted_prediction},
                {"rejected_prediction", &PriceRates::rejected_prediction},
                {"tool_prompt", &PriceRates::tool_prompt},
                {"image_input", &PriceRates::image_input},
                {"web_search", &PriceRates::web_search},
                {"web_fetch", &PriceRates::web_fetch},
                {"code_exec", &PriceRates::code_exec},
                {"file_search", &PriceRates::file_search},
            };
            for (const auto& [name, item] : value.obj)
            {
                if (is_comment(name)) continue;
                const RateField* found = nullptr;
                for (const RateField& field : fields)
                    if (field.name == name) found = &field;
                if (!found)
                    return fail(err, "config: unknown accounting.prices.rates field");
                int64_t amount = 0;
                if (!parse_nonnegative_i64(item, amount))
                    return fail(err, "config: accounting price rates must be nonnegative integers");
                rates.*found->member = amount;
            }
            if (rates.input < 0 || rates.output < 0)
                return fail(err, "config: accounting price rates require input and output");
            return true;
        }

        bool apply_accounting(const json::Value& g, Settings& s, std::string& err)
        {
            if (!g.is_object()) return fail(err, "config: accounting must be an object");
            for (const auto& [key, value] : g.obj)
            {
                if (is_comment(key)) continue;
                if (key == "ledger_path")
                {
                    if (value.type != json::Value::Type::String || value.sv.empty() ||
                        value.sv.size() > 4096 || value.sv.front() != '/' ||
                        value.sv.find('\0') != std::string_view::npos)
                        return fail(err, "config: accounting.ledger_path must be a nonempty absolute path");
                    s.audit_ledger_path = value.sv;
                    continue;
                }
                if (key != "prices") return fail(err, "config: unknown accounting field");
                if (!value.is_array() || value.arr.size() > 1024)
                    return fail(err,
                                "config: accounting.prices must be an array of at most 1024 rules");
                std::vector<PriceRule> rules;
                rules.reserve(value.arr.size());
                for (const json::Value& item : value.arr)
                {
                    if (!item.is_object())
                        return fail(err, "config: accounting.prices entries must be objects");
                    PriceRule rule;
                    bool saw_rates = false, saw_from = false, saw_until = false;
                    for (const auto& [field, member] : item.obj)
                    {
                        if (is_comment(field)) continue;
                        if (field == "provider" || field == "model" || field == "version" ||
                            field == "source")
                        {
                            const size_t limit =
                                field == "source" ? 512 : field == "model" ? 128 : 64;
                            if (member.type != json::Value::Type::String || member.sv.empty() ||
                                member.sv.size() > limit ||
                                member.sv.find('\\') != std::string_view::npos)
                                return fail(err, "config: accounting price identity fields are invalid");
                            (field == "provider"   ? rule.provider
                             : field == "model"   ? rule.model
                             : field == "version" ? rule.version
                                                  : rule.source) = member.sv;
                        }
                        else if (field == "valid_from_unix_s" || field == "valid_until_unix_s")
                        {
                            int64_t timestamp = 0;
                            if (!parse_nonnegative_i64(member, timestamp) || timestamp > 4102444800LL)
                                return fail(err, "config: accounting price timestamp is out of range");
                            (field == "valid_from_unix_s" ? rule.valid_from_s : rule.valid_until_s) =
                                timestamp;
                            if (field == "valid_from_unix_s") saw_from = true;
                            else saw_until = true;
                        }
                        else if (field == "rates")
                        {
                            if (!apply_price_rates(member, rule.rates, err)) return false;
                            saw_rates = true;
                        }
                        else return fail(err, "config: unknown accounting.prices field");
                    }
                    if (!stable_id_ok(rule.provider) || rule.model.empty() || rule.version.empty() ||
                        rule.source.empty() || !saw_from || !saw_until || !saw_rates)
                        return fail(err, "config: accounting price rule is incomplete");
                    rules.push_back(std::move(rule));
                }
                try
                {
                    (void)PriceTable(rules);
                }
                catch (const std::invalid_argument&)
                {
                    return fail(err, "config: accounting.prices contains invalid or overlapping rules");
                }
                s.price_rules = std::move(rules);
            }
            return true;
        }

        bool apply_security(const json::Value& g, Settings& s, std::string& err)
        {
            if (!g.is_object()) return fail(err, "config: security must be an object");
            for (const auto& [k, v] : g.obj)
            {
                if (is_comment(k)) continue;
                if (k == "enabled")
                {
                    if (v.type != json::Value::Type::Bool)
                        return fail(err, "config: security.enabled must be boolean");
                    s.security_enabled = v.boolean;
                }
                else if (k == "legacy_mode")
                {
                    if (v.type != json::Value::Type::Bool)
                        return fail(err, "config: security.legacy_mode must be boolean");
                    s.legacy_mode = v.boolean;
                }
                else if (k == "allow_insecure_http")
                {
                    if (v.type != json::Value::Type::Bool)
                        return fail(err, "config: security.allow_insecure_http must be boolean");
                    s.allow_insecure_http = v.boolean;
                }
                else if (k == "users")
                {
                    if (!v.is_array() || v.arr.empty() || v.arr.size() > 256)
                        return fail(err, "config: security.users must contain 1..256 users");
                    s.users.clear();
                    std::unordered_set<std::string> user_ids;
                    std::unordered_set<std::string> key_envs;
                    for (const auto& item : v.arr)
                    {
                        if (!item.is_object()) return fail(err, "config: security.users entries must be objects");
                        UserSettings user;
                        for (const auto& [field, value] : item.obj)
                        {
                            if (is_comment(field)) continue;
                            if (field == "id" || field == "tenant" || field == "key_env")
                            {
                                if (value.type != json::Value::Type::String || value.sv.empty() ||
                                    value.sv.size() > 64 || value.sv.find('\\') != std::string_view::npos)
                                    return fail(err, "config: security.users identity fields must be plain strings (1..64)");
                                (field == "id" ? user.id : field == "tenant" ? user.tenant : user.key_env) = value.sv;
                            }
                            else if (field == "models")
                            {
                                if (!value.is_array() || value.arr.empty() || value.arr.size() > 64)
                                    return fail(err, "config: security.users.models must contain 1..64 names");
                                std::unordered_set<std::string_view> seen;
                                for (const auto& e : value.arr)
                                {
                                    if (e.type != json::Value::Type::String || !substitute_ok(e.sv, 64))
                                        return fail(err, "config: security.users.models contains an invalid model");
                                    if (!seen.insert(e.sv).second)
                                        return fail(err, "config: security.users.models contains a duplicate model");
                                    user.models.emplace_back(e.sv);
                                }
                            }
                            else if (field == "upstreams")
                            {
                                if (!value.is_array() || value.arr.empty() || value.arr.size() > 64)
                                    return fail(err, "config: security.users.upstreams must contain 1..64 indices");
                                std::unordered_set<int> seen;
                                for (const auto& e : value.arr)
                                {
                                    if (e.type != json::Value::Type::Number || e.sv.empty())
                                        return fail(err, "config: security.users.upstreams contains an invalid index");
                                    int n = -1;
                                    const auto parsed = std::from_chars(e.sv.data(), e.sv.data() + e.sv.size(), n);
                                    if (parsed.ec != std::errc{} || parsed.ptr != e.sv.data() + e.sv.size() || n < 0)
                                        return fail(err, "config: security.users.upstreams contains an invalid index");
                                    if (!seen.insert(n).second)
                                        return fail(err, "config: security.users.upstreams contains a duplicate index");
                                    user.upstreams.push_back(n);
                                }
                            }
                            else if (field == "requests_per_minute" || field == "max_concurrent" ||
                                     field == "daily_tokens" || field == "monthly_tokens" ||
                                     field == "expires_unix_s")
                            {
                                if (value.type != json::Value::Type::Number || value.sv.empty())
                                    return fail(err, "config: security.users numeric field must be a nonnegative integer");
                                int64_t n = 0;
                                const auto parsed = std::from_chars(value.sv.data(), value.sv.data() + value.sv.size(), n);
                                if (parsed.ec != std::errc{} || parsed.ptr != value.sv.data() + value.sv.size() ||
                                    n < (field == "requests_per_minute" || field == "max_concurrent" ? 1 : 0) ||
                                    n > (field == "expires_unix_s" ? 4102444800LL : 1000000000LL))
                                    return fail(err, "config: security.users numeric field out of range");
                                if (field == "requests_per_minute") user.requests_per_minute = static_cast<int>(n);
                                else if (field == "max_concurrent") user.max_concurrent = static_cast<int>(n);
                                else if (field == "daily_tokens") user.daily_tokens = static_cast<int>(n);
                                else if (field == "monthly_tokens") user.monthly_tokens = static_cast<int>(n);
                                else user.expires_unix_s = n;
                            }
                            else if (field == "disabled")
                            {
                                if (value.type != json::Value::Type::Bool)
                                    return fail(err, "config: security.users.disabled must be boolean");
                                user.disabled = value.boolean;
                            }
                            else return fail(err, "config: unknown security.users field");
                        }
                        if (user.id.empty() || user.tenant.empty() || user.key_env.empty() ||
                            user.models.empty() || user.upstreams.empty())
                            return fail(err, "config: security.users requires id, tenant, key_env, models, upstreams");
                        if (!user_ids.insert(user.id).second || !key_envs.insert(user.key_env).second)
                            return fail(err, "config: security.users contains a duplicate id or key_env");
                        s.users.push_back(std::move(user));
                    }
                }
                else return fail(err, "config: unknown security field");
            }
            return true;
        }

        bool is_group(std::string_view k)
        {
            for (const Option& o : kOptions)
                if (o.key.substr(0, o.key.find('.')) == k) return true;
            return false;
        }

        std::string row(std::string_view left, std::string_view help)
        {
            std::string r = "  " + std::string(left);
            r.resize(std::max<size_t>(r.size() + 1, 30), ' ');
            return r + std::string(help) + "\n";
        }
    } // namespace

    std::span<const Option> options() noexcept { return kOptions; }

    std::string help_text(std::string_view argv0)
    {
        std::string out = "usage: " + std::string(argv0) + " [--config FILE] [flags]\n" +
                          "Flags override the file wherever they appear. In parentheses: the file key.\n";
        out += row("--config FILE", "JSON settings; see app/llmbridge.example.json");
        for (const Option& o : kOptions)
        {
            if (o.flag.empty()) continue;
            std::string help(o.help);
            if (o.kind == Kind::Int) help += ", " + bounds(o.lo) + ".." + bounds(o.hi);
            for (size_t i = 0; i < o.choices.size(); ++i)
                help += (i ? "|" : ": ") + std::string(o.choices[i]);
            const std::string flag = std::string(o.flag) + (o.arg.empty() ? "" : " ") + std::string(o.arg);
            out += row(flag, help + " (" + std::string(o.key) + ")");
        }
        out += "File only:\n";
        for (const Option& o : kOptions)
            if (o.flag.empty()) out += row(o.key, o.help);
        return out + row("-h, --help", "print this and exit");
    }

    Cli parse_cli(std::span<char* const> args, Settings& s, std::string& err)
    {
        const auto missing = [&](size_t i) {
            return i + 1 >= args.size() || std::string_view(args[i + 1]).starts_with("--");
        };
        // The file first, so flags overwrite it; validated before any I/O.
        const char* path = nullptr;
        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::string_view a = args[i];
            if (a == "--help" || a == "-h") return Cli::Help;
            if (a != "--config") continue;
            if (missing(i)) return refuse(err, "--config needs a path");
            if (path)
                return refuse(err, "--config given twice; pass it once");
            path = args[++i];
        }
        if (path && !load_config(path, s, err)) return Cli::Refused;

        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::string_view a = args[i];
            if (a == "--config") { ++i; continue; }
            const Option* o = nullptr;
            for (const Option& c : kOptions)
                if (!c.flag.empty() && c.flag == a) o = &c;
            if (!o && !retired(a).empty())
                return refuse(err, std::string(a) + " " + std::string(retired(a)));
            if (!o)
            {
                if (const size_t eq = a.find('='); eq != std::string_view::npos)
                    return refuse(err, "unknown argument; flags take a separate value, not '='. "
                                       "Run --help for the accepted flags.");
                return refuse(err, "unknown argument. Run --help for the accepted flags.");
            }
            if (o->kind == Kind::Bool)
            {
                s.*std::get<bool Settings::*>(o->field) = true;
                continue;
            }
            if (missing(i))
                return refuse(err, std::string(a) + " needs a value (" + std::string(o->arg) + ")");
            if (!set_text(*o, args[++i], s, std::string(a), err)) return Cli::Refused;
        }
        return Cli::Run;
    }

    bool parse_config(std::string_view text, Settings& s, std::string& err)
    {
        if (text.size() > kMaxConfigBytes)
            return fail(err, "config: exceeds 4 MiB size limit");
        bool ok = false;
        const json::Value root = json::parse(text, ok);
        if (!ok)
        {
            // Keys::Any tells a repeated key, which parse() refuses, from bad JSON.
            bool any = false;
            (void)json::parse(text, any, json::Keys::Any);
            return fail(err, any ? "config: a key appears twice in one object"
                                 : "config: not valid JSON");
        }
        if (!root.is_object()) return fail(err, "config: top level must be an object");
        for (const auto& [k, v] : root.obj)
        {
            if (is_comment(k)) continue;
            if (!is_group(k) && k != "security" && k != "accounting")
                return fail(err, "config: unknown key in \"(top level)\"");
            if (!(k == "upstream" ? apply_upstream(v, s, err) :
                  k == "security" ? apply_security(v, s, err) :
                  k == "accounting" ? apply_accounting(v, s, err) : apply_group(k, v, s, err)))
                return false;
        }
        if (!s.price_rules.empty() || !s.audit_ledger_path.empty())
        {
            std::unordered_set<std::string_view> providers;
            if (!stable_id_ok(s.upstream_id) || !providers.insert(s.upstream_id).second)
                return fail(
                    err, "config: every upstream needs a unique id when accounting is configured");
            for (const Venue& venue : s.more_upstreams)
                if (!stable_id_ok(venue.id) || !providers.insert(venue.id).second)
                    return fail(
                        err, "config: every upstream needs a unique id when accounting is configured");
            for (const PriceRule& rule : s.price_rules)
                if (!providers.contains(rule.provider))
                    return fail(err, "config: accounting price refers to an unknown provider");
        }
        if (!s.audit_ledger_path.empty() && s.price_rules.empty())
            return fail(err, "config: accounting.ledger_path requires accounting.prices");
        return true;
    }

    bool load_config(const std::string& path, Settings& s, std::string& err)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return fail(err, "config: cannot read file");
        std::string text;
        char chunk[4096];
        while (in)
        {
            in.read(chunk, sizeof(chunk));
            const size_t n = static_cast<size_t>(in.gcount());
            if (n > kMaxConfigBytes - text.size())
                return fail(err, "config: exceeds 4 MiB size limit");
            text.append(chunk, n);
        }
        if (!in.eof()) return fail(err, "config: cannot read file");
        if (text.empty()) return fail(err, "config: file is empty");
        if (!parse_config(text, s, err)) return false;
        return true;
    }
} // namespace llmbridge::app
