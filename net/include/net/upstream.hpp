// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Parsing and DNS resolution of --upstream. The accepted and refused forms, and why each
// refusal exists: DESIGN.md "Upstream URLs". Setup path only: allocation and blocking are fine.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace llmbridge::net
{
    struct UpstreamSpec
    {
        std::string host;   ///< as written: DNS name or IPv4 literal; feeds Host and SNI
        std::string path;   ///< "" or "/..." without a trailing slash, prefixed to the target
        std::string query;  ///< without the '?'; only a mode that builds its own target uses it
        uint16_t port{0};
        bool tls{false};
        std::string error;  ///< non-empty => parse failed, other fields unspecified

        [[nodiscard]] bool ok() const noexcept { return error.empty(); }
    };

    /// 解析上游地址。语法错误由返回值的 error 表示；内存分配失败仍可能抛异常。
    /// 参数仅在调用期间借用，结果拥有自己的字符串；不同线程可并发调用。
    [[nodiscard]] UpstreamSpec parse_upstream(std::string_view arg);

    /// 在初始化路径解析 IPv4 地址，去重并保留解析器顺序；IPv4 字面量直接返回。
    /// 返回值拥有地址字符串；err 非空时仅在调用期间借用，失败时写入原因。
    /// 不共享可变状态，可并发调用；DNS 查询可能阻塞或分配内存，不能在热路径使用。
    [[nodiscard]] std::vector<std::string> resolve_host_ipv4(const std::string& host,
                                                             std::string* err = nullptr);
} // namespace llmbridge::net
