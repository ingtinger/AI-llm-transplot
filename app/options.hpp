// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Every daemon setting, once: one Option table feeds the flags, the `--config` file and
// --help, so the two inputs share one parser and one set of bounds. DESIGN.md "Configuration".

#pragma once

#include "gateway/gateway.hpp"

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace llmbridge::app
{
    struct Venue
    {
        std::string url, dialect, credential_header, model, credential_env;
        std::vector<std::string> models; // 可服务的客户端模型；与 model 映射不同
        bool operator==(const Venue&) const = default;
    };

    struct UserSettings
    {
        std::string id, tenant, key_env;
        std::vector<std::string> models;
        std::vector<int> upstreams;
        int requests_per_minute = 60;
        int max_concurrent = 4;
        int daily_tokens = 0, monthly_tokens = 0; // 0 表示不限制
        int64_t expires_unix_s = 0; // 0 表示不过期
        bool disabled = false;
        bool operator==(const UserSettings&) const = default;
    };

    /// Initialised to the built-in defaults; the file is applied first, then the flags.
    struct Settings
    {
        int listen_port = 8088;
        bool listen_tls = false;
        std::string tls_cert, tls_key;
        std::string upstream = "127.0.0.1:9001", dialect = "openai"; // venue 0
        std::string upstream_credential_header, upstream_model;
        std::string upstream_credential_env;
        std::vector<std::string> upstream_models; // 首个上游的客户端模型池
        std::vector<Venue> more_upstreams;                           // venues 1..N, file only
        bool security_enabled = false;
        bool legacy_mode = false; // 仅显式开启旧的无认证直通行为
        bool allow_insecure_http = false; // 仅供本地 Mock 测试使用
        std::vector<UserSettings> users;
        std::vector<std::string> strip_headers;                      // gateway-wide
        std::string routing_mode = "first", routing_sticky_header = "x-conversation-id";
        std::vector<std::string> routing_drained_upstreams; // SIGHUP 可热更新的索引列表
        int routing_max_attempts = 3, routing_failure_threshold = 3;
        bool routing_allow_unsafe_replay = false;
        double routing_cooldown_s = 10;
        int management_port = 0; // 0 关闭；启用后仅监听 127.0.0.1
        bool request_log = false;
        double upstream_s = static_cast<double>(Gateway::kDefaultUpstreamIdleNs) / 1e9;
        double client_idle_s = static_cast<double>(Gateway::kDefaultClientIdleNs) / 1e9;
        double pool_idle_s = static_cast<double>(Gateway::kDefaultPoolIdleNs) / 1e9;
        double connect_s = static_cast<double>(Gateway::kDefaultConnectNs) / 1e9;
        double body_s = static_cast<double>(Gateway::kDefaultBodyTimeoutNs) / 1e9;
        double body_grace_s = static_cast<double>(Gateway::kDefaultBodyGraceNs) / 1e9;
        int body_min_rate = static_cast<int>(Gateway::kDefaultBodyMinRate);
        std::string io = "auto", log_level = "info";
        int workers = 1;
        bool timing_headers = false;
        double duration_s = 0, warmup_s = 0, prefault_mb = 0, retain_mb = 4;
        std::string stream_body = "on", stream_openai_body = "on";
        int stream_body_max = static_cast<int>(Gateway::kDefaultStreamBodyMax);
        bool operator==(const Settings&) const = default;
    };

    enum class Kind { Bool, Int, Number, Text, Choice, List };
    using Field = std::variant<bool Settings::*, int Settings::*, double Settings::*,
                               std::string Settings::*, std::vector<std::string> Settings::*>;

    /// An empty `flag` is file-only; `key` is "group.name". Bounds apply to Int and Number.
    struct Option
    {
        std::string_view flag, key, arg, help;
        Kind kind;
        double lo, hi;
        std::span<const std::string_view> choices;
        Field field;
    };
    std::span<const Option> options() noexcept;
    std::string help_text(std::string_view argv0);

    enum class Cli { Run, Help, Refused };
    /// `args` excludes argv[0]. Reads `--config` first, so a flag wins wherever it sits.
    Cli parse_cli(std::span<char* const> args, Settings& s, std::string& err);
    /// Writes only the keys present; `err` is one line naming the key.
    bool parse_config(std::string_view text, Settings& s, std::string& err);
    bool load_config(const std::string& path, Settings& s, std::string& err);
} // namespace llmbridge::app
