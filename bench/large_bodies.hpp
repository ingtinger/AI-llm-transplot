// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// OpenAI request bodies of a chosen size in the shapes large ones take: a long agent
// transcript, big tool schemas, and base64 images. Deterministic, so two runs compare.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace llmbridge::bench
{
    enum class Shape
    {
        Transcript, // an agent loop: text, escaped code, tool calls and their results
        Schemas,    // many tools with deep JSON schemas: a value every few bytes
        Images,     // a few user turns carrying base64 PNGs: one long string each
    };

    inline const char* shape_name(Shape s)
    {
        return s == Shape::Transcript ? "transcript" : s == Shape::Schemas ? "schemas" : "images";
    }

    namespace detail
    {
        // Printable filler with the escapes real text carries: quotes, newlines, tabs.
        inline std::string prose(size_t n, uint32_t seed)
        {
            static constexpr std::string_view kWords[] = {
                "the", "request", "returns", "a", "value", "when", "parse", "fails,", "then",
                "\\n", "if (x) {", "return", "\\\"ok\\\";", "}", "\\t", "context", "token", "model"};
            std::string s;
            s.reserve(n + 16);
            while (s.size() < n)
            {
                seed = seed * 1103515245u + 12345u;
                s += kWords[(seed >> 16) % std::size(kWords)];
                s += ' ';
            }
            s.resize(n);
            while (!s.empty() && s.back() == '\\') s.pop_back(); // never end inside an escape
            return s;
        }

        inline std::string base64(size_t n, uint32_t seed)
        {
            static constexpr char kAlpha[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string s(n, 'A');
            for (char& c : s)
            {
                seed = seed * 1664525u + 1013904223u;
                c = kAlpha[(seed >> 24) & 63];
            }
            return s;
        }
    } // namespace detail

    /// An OpenAI chat request of about `bytes` bytes in `shape`.
    inline std::string body(Shape shape, size_t bytes)
    {
        std::string b = R"({"model":"claude-opus-5-5","max_tokens":4096,)";
        if (shape == Shape::Schemas)
        {
            b += R"("tools":[)";
            for (int t = 0; b.size() + 2048 < bytes; ++t)
            {
                b += std::string(t ? "," : "") + R"({"type":"function","function":{"name":"tool_)" +
                     std::to_string(t) + R"(","description":"Operates on the workspace.",)" +
                     R"("parameters":{"type":"object","properties":{)";
                for (int p = 0; p < 12; ++p)
                    b += std::string(p ? "," : "") + R"("field_)" + std::to_string(p) +
                         R"(":{"type":"object","description":"a nested option",)" +
                         R"("properties":{"enabled":{"type":"boolean","default":false},)" +
                         R"("mode":{"type":"string","enum":["fast","safe","off"]},)" +
                         R"("limit":{"type":"integer","minimum":0,"maximum":100}},)" +
                         R"("required":["enabled"]})";
                b += R"(},"required":["field_0"]}}})";
            }
            return b + R"(],"messages":[{"role":"user","content":"go"}]})";
        }
        b += R"("messages":[{"role":"system","content":"You are a coding agent."})";
        for (uint32_t i = 0; b.size() + 1024 < bytes; ++i)
        {
            if (shape == Shape::Images)
            {
                const size_t n = std::min<size_t>(bytes - b.size() - 200, 2u << 20);
                b += R"(,{"role":"user","content":[{"type":"text","text":"What is in this image?"},)"
                     R"({"type":"image_url","image_url":{"url":"data:image/png;base64,)" +
                     detail::base64(n, i) + R"("}}]})";
                continue;
            }
            const std::string id = "call_" + std::to_string(i);
            b += R"(,{"role":"user","content":")" + detail::prose(900, i) + R"("})";
            b += R"(,{"role":"assistant","content":null,"tool_calls":[{"id":")" + id +
                 R"(","type":"function","function":{"name":"read_file","arguments":)"
                 R"("{\"path\":\"src/a/b/file.cpp\",\"lines\":[1,200]}"}}]})";
            b += R"(,{"role":"tool","tool_call_id":")" + id + R"(","content":")" +
                 detail::prose(2400, i * 7 + 1) + R"("})";
        }
        return b + "]}";
    }
} // namespace llmbridge::bench
