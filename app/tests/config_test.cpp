// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The config parser exists to refuse things, so most of this file is rejection
// cases. The one that matters most is the unknown key: a parser that ignores a
// misspelled setting fails open, silently, with no `ps` output to catch it, which
// is the exact shape of the `--listen-tls`-on-a-non-TLS-build defect.

#include "options.hpp"

#include "provider/json.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <utility>

using llmbridge::app::Settings;
using llmbridge::app::parse_config;

TEST(Config, OversizedConfigurationIsRejectedBeforeParsing)
{
    // 文件加载同样需要有界；/dev/zero 可验证读取不会等到 EOF 或耗尽内存。
    Settings s;
    std::string err;
    EXPECT_FALSE(parse_config(std::string(4 * 1024 * 1024 + 1, ' '), s, err));
    EXPECT_NE(err.find("4 MiB"), std::string::npos);
    EXPECT_FALSE(llmbridge::app::load_config("/dev/zero", s, err));
    EXPECT_NE(err.find("4 MiB"), std::string::npos);
}

TEST(Config, UnknownFieldNamesAreRejectedWithoutEchoingThem)
{
    // 未知字段可能误贴入凭据；保留组名即可定位，不能回显原字段名。
    for (const std::string_view input : {
             R"({"sk-secret-marker":true})",
             R"({"listen":{"sk-secret-marker":true}})",
             R"({"security":{"sk-secret-marker":true}})",
             R"({"security":{"users":[{"sk-secret-marker":true}]}})"})
    {
        Settings s;
        std::string err;
        EXPECT_FALSE(parse_config(input, s, err));
        EXPECT_NE(err.find("unknown"), std::string::npos) << err;
        EXPECT_EQ(err.find("sk-secret-marker"), std::string::npos) << err;
    }
}

TEST(Config, UnreadablePathIsNotEchoed)
{
    Settings s;
    std::string err;
    EXPECT_FALSE(llmbridge::app::load_config("/nonexistent/sk-secret-marker.json", s, err));
    EXPECT_NE(err.find("cannot read"), std::string::npos);
    EXPECT_EQ(err.find("sk-secret-marker"), std::string::npos);
}

TEST(Config, SecurityUsersAndEnvironmentNamesAreStrict)
{
    Settings s;
    std::string err;
    ASSERT_TRUE(parse_config(R"({"upstream":{"credential_env":"PROVIDER_KEY"},"security":{"enabled":true,"users":[{"id":"alice","tenant":"team","key_env":"ALICE_KEY","models":["chat"],"upstreams":[0],"requests_per_minute":2,"max_concurrent":1}]}})", s, err)) << err;
    EXPECT_TRUE(s.security_enabled);
    EXPECT_EQ(s.upstream_credential_env, "PROVIDER_KEY");
    ASSERT_EQ(s.users.size(), 1U);
    EXPECT_EQ(s.users[0].id, "alice");
    EXPECT_EQ(s.users[0].upstreams[0], 0);
    Settings invalid;
    EXPECT_FALSE(parse_config(R"({"security":{"users":[{"id":"alice","tenant":"team","key_env":"ALICE_KEY","models":["chat"],"upstreams":[1.5]}]}})", invalid, err));
    EXPECT_FALSE(parse_config(R"({"security":{"unknown":true}})", invalid, err));
    EXPECT_FALSE(parse_config(R"({"security":{"allow_insecure_http":"true"}})", invalid, err));
}

TEST(Config, StripHeaderNamesAreBoundedAndValid)
{
    Settings s;
    std::string err;
    EXPECT_TRUE(parse_config(R"({"upstream":{"strip_headers":["Authorization","X.Custom_Header"]}})", s, err)) << err;
    for (const std::string_view invalid : {
             R"({"upstream":{"strip_headers":["authorization:"]}})",
             R"({"upstream":{"strip_headers":["x bad"]}})",
             R"({"upstream":{"strip_headers":["x/bad"]}})"})
    {
        Settings rejected;
        EXPECT_FALSE(parse_config(invalid, rejected, err)) << invalid;
    }
    Settings long_name;
    Settings boundary;
    EXPECT_TRUE(parse_config("{\"upstream\":{\"strip_headers\":[\"" +
                                 std::string(64, 'x') + "\"]}}", boundary, err)) << err;
    EXPECT_FALSE(parse_config("{\"upstream\":{\"strip_headers\":[\"" +
                                  std::string(65, 'x') + "\"]}}", long_name, err));
    Settings many_names;
    std::string many = "{\"upstream\":{\"strip_headers\":[";
    for (int i = 0; i < 65; ++i)
        many += (i ? "," : "") + std::string("\"x-" + std::to_string(i) + "\"");
    many += "]}}";
    EXPECT_FALSE(parse_config(many, many_names, err));
}

TEST(Config, DrainIndicesAreReadFromRoutingGroup)
{
    Settings s;
    std::string err;
    ASSERT_TRUE(parse_config(R"({"routing":{"drained_upstreams":["0","2"]}})", s, err)) << err;
    EXPECT_EQ(s.routing_drained_upstreams, (std::vector<std::string>{"0", "2"}));
    EXPECT_FALSE(parse_config(R"({"routing":{"drained_upstreams":[0]}})", s, err));
}

TEST(Config, DrainListIsBoundedBeforeCopying)
{
    // 摘除位图至多有 64 位，配置解析不应复制任意长的索引列表或索引文本。
    Settings s;
    std::string err;
    EXPECT_FALSE(parse_config("{\"routing\":{\"drained_upstreams\":[\"" +
                                  std::string(21, '0') + "\"]}}", s, err));
    std::string many = "{\"routing\":{\"drained_upstreams\":[";
    for (int i = 0; i < 65; ++i)
        many += (i ? "," : "") + std::string("\"") + std::to_string(i) + "\"";
    many += "]}}";
    EXPECT_FALSE(parse_config(many, s, err));
}

TEST(Config, StickyHeaderIsBoundedAndValid)
{
    Settings s;
    std::string err;
    EXPECT_TRUE(parse_config(R"({"routing":{"sticky_header":"x_chat-id"}})", s, err)) << err;
    for (const std::string_view invalid : {
             R"({"routing":{"sticky_header":"x bad"}})",
             R"({"routing":{"sticky_header":"x.bad"}})",
             R"({"routing":{"sticky_header":"x:bad"}})"})
    {
        Settings rejected;
        EXPECT_FALSE(parse_config(invalid, rejected, err)) << invalid;
    }
    EXPECT_TRUE(parse_config("{\"routing\":{\"sticky_header\":\"" +
                                 std::string(64, 'x') + "\"}}", s, err)) << err;
    EXPECT_FALSE(parse_config("{\"routing\":{\"sticky_header\":\"" +
                                  std::string(65, 'x') + "\"}}", s, err));
}

TEST(Config, FullFileAppliesEveryGroup)
{
    const std::string text = R"({
      "_note": "comments are keys beginning with underscore",
      "listen":   { "port": 8443, "tls": true, "cert": "/c.pem", "key": "/k.pem" },
      "upstream": { "url": "https://api.anthropic.com", "dialect": "anthropic",
                    "credential_header": "x-kottos-key-anthropic", "model": "claude-sonnet",
                    "strip_headers": ["authorization", "X-Internal"] },
      "routing":  { "mode": "sticky", "max_attempts": 2, "failure_threshold": 4,
                    "cooldown_s": 15, "sticky_header": "x-chat-id" },
      "management": { "port": 9091 },
      "timeouts": { "upstream_s": 90, "client_idle_s": 259200, "pool_idle_s": 45,
                    "connect_s": 7 },
      "runtime":  { "io": "uring", "workers": 3, "timing_headers": true,
                    "duration_s": 12, "warmup_s": 2, "log_level": "debug", "request_log": true,
                    "prefault_mb": 16 }
    })";
    Settings c;
    std::string err;
    ASSERT_TRUE(parse_config(text, c, err)) << err;

    EXPECT_EQ(c.listen_port, 8443);
    EXPECT_TRUE(c.listen_tls);
    EXPECT_EQ(c.tls_cert, "/c.pem");
    EXPECT_EQ(c.tls_key, "/k.pem");
    EXPECT_TRUE(c.more_upstreams.empty()); // the object form is one entry
    EXPECT_EQ(c.upstream, "https://api.anthropic.com");
    EXPECT_EQ(c.dialect, "anthropic");
    EXPECT_EQ(c.upstream_credential_header, "x-kottos-key-anthropic");
    EXPECT_EQ(c.upstream_model, "claude-sonnet");
    EXPECT_EQ(c.routing_mode, "sticky");
    EXPECT_EQ(c.routing_max_attempts, 2);
    EXPECT_EQ(c.routing_failure_threshold, 4);
    EXPECT_DOUBLE_EQ(c.routing_cooldown_s, 15);
    EXPECT_EQ(c.routing_sticky_header, "x-chat-id");
    EXPECT_EQ(c.management_port, 9091);
    EXPECT_TRUE(c.request_log);
    EXPECT_DOUBLE_EQ(c.upstream_s, 90);
    EXPECT_DOUBLE_EQ(c.client_idle_s, 259200);
    EXPECT_DOUBLE_EQ(c.pool_idle_s, 45);
    EXPECT_DOUBLE_EQ(c.connect_s, 7);
    EXPECT_EQ(c.io, "uring");
    EXPECT_EQ(c.log_level, "debug");
    EXPECT_EQ(c.workers, 3);
    EXPECT_TRUE(c.timing_headers);
    EXPECT_DOUBLE_EQ(c.duration_s, 12);
    EXPECT_DOUBLE_EQ(c.warmup_s, 2);
    EXPECT_DOUBLE_EQ(c.prefault_mb, 16);
    ASSERT_EQ(c.strip_headers.size(), 2u);
    EXPECT_EQ(c.strip_headers[0], "authorization");
    EXPECT_EQ(c.strip_headers[1], "X-Internal"); // normalized by the Gateway, not here
}

// The values must survive the DOM they were parsed from. provider::json is
// zero-copy, so every string in the parsed Value is a view into the input buffer;
// if a Settings field ever held a string_view instead of a string, this test
// reads freed memory and ASan says so.
TEST(Config, ValuesOutliveTheParsedBuffer)
{
    Settings c;
    std::string err;
    {
        std::string scratch = R"({"upstream":{"url":"https://example.invalid/v1"}})";
        ASSERT_TRUE(parse_config(scratch, c, err)) << err;
        scratch.assign(4096, 'x'); // clobber the buffer the DOM pointed into
    }
    EXPECT_EQ(c.upstream, "https://example.invalid/v1");
}

// An absent key must leave the caller's default alone, which is what makes
// "file first, flags second" work at all.
TEST(Config, AbsentKeysAreNotApplied)
{
    Settings c;
    c.workers = 7;
    c.upstream = "10.0.0.1:1";
    std::string err;
    ASSERT_TRUE(parse_config(R"({"listen":{"port":9000}})", c, err)) << err;
    EXPECT_EQ(c.listen_port, 9000);
    EXPECT_FALSE(c.listen_tls);
    EXPECT_EQ(c.workers, 7);
    EXPECT_TRUE(c.tls_cert.empty());
    EXPECT_EQ(c.upstream, "10.0.0.1:1");
    EXPECT_EQ(c.dialect, "openai");
    EXPECT_TRUE(c.upstream_credential_header.empty());
    EXPECT_TRUE(c.upstream_model.empty());
    EXPECT_EQ(c.routing_mode, "first");
    EXPECT_EQ(c.routing_max_attempts, 3);
    EXPECT_EQ(c.routing_failure_threshold, 3);
    EXPECT_DOUBLE_EQ(c.routing_cooldown_s, 10);
    EXPECT_EQ(c.routing_sticky_header, "x-conversation-id");
    EXPECT_TRUE(c.routing_drained_upstreams.empty());
    EXPECT_FALSE(c.routing_allow_unsafe_replay);
    EXPECT_EQ(c.management_port, 0);
    EXPECT_FALSE(c.request_log);
}

// ── The upstream table ───────────────────────────────────────────────────────
//
// `upstream` accepts an object (one venue, what nearly every deployment writes) or
// an array of the same object. The array is what lets a policy route, and the object
// form must keep meaning exactly one entry so no existing config changes behaviour.

TEST(Config, UpstreamArrayBecomesATableInOrder)
{
    const char* text = R"({
      "upstream": [ { "url": "127.0.0.1:9001", "dialect": "anthropic",
                      "credential_header": "x-kottos-key-a", "model": "model-a" },
                    { "url": "127.0.0.1:9002", "dialect": "openai",
                      "credential_header": "x-kottos-key-b", "model": "model-b" },
                    { "url": "https://api.anthropic.com", "dialect": "anthropic" } ]
    })";
    Settings c;
    std::string err;
    ASSERT_TRUE(parse_config(text, c, err)) << err;
    ASSERT_EQ(c.more_upstreams.size(), 2u);
    EXPECT_EQ(c.upstream, "127.0.0.1:9001");
    EXPECT_EQ(c.more_upstreams[0].url, "127.0.0.1:9002");
    EXPECT_EQ(c.more_upstreams[1].url, "https://api.anthropic.com");
    // Order is the contract: a policy selects by index, so a table that reorders
    // silently sends requests to the wrong venue.
    EXPECT_EQ(c.dialect, "anthropic");
    EXPECT_EQ(c.more_upstreams[0].dialect, "openai");
    EXPECT_EQ(c.more_upstreams[1].dialect, "anthropic");
    EXPECT_EQ(c.upstream_credential_header, "x-kottos-key-a");
    EXPECT_EQ(c.upstream_model, "model-a");
    EXPECT_EQ(c.more_upstreams[0].credential_header, "x-kottos-key-b");
    EXPECT_EQ(c.more_upstreams[0].model, "model-b");
}

TEST(Config, EachEntryValidatesLikeTheObjectForm)
{
    Settings c;
    std::string err;
    // A misspelling inside an array entry must fail exactly as it does in an object.
    EXPECT_FALSE(parse_config(R"({"upstream":[{"url":"x:1","dialec":"openai"}]})", c, err));
    EXPECT_NE(err.find("unknown key in \"upstream\""), std::string::npos) << err;
    err.clear();
    EXPECT_FALSE(parse_config(R"({"upstream":[{"url":"x:1","dialect":"claude"}]})", c, err));
    EXPECT_NE(err.find("must be one of"), std::string::npos) << err;
}

TEST(Config, PerUpstreamClientModelPoolsAreStrict)
{
    Settings c;
    std::string err;
    ASSERT_TRUE(parse_config(R"({"upstream":[{"url":"a:1","models":["chat","code"],"model":"provider-a"},{"url":"b:2","models":["code"]}]})", c, err)) << err;
    EXPECT_EQ(c.upstream_models, (std::vector<std::string>{"chat", "code"}));
    ASSERT_EQ(c.more_upstreams.size(), 1u);
    EXPECT_EQ(c.more_upstreams[0].models, (std::vector<std::string>{"code"}));
    EXPECT_EQ(c.upstream_model, "provider-a");
    EXPECT_FALSE(parse_config(R"({"upstream":[{"url":"a:1","models":["chat"]},{"url":"b:2"}]})", c, err));
    EXPECT_NE(err.find("every upstream"), std::string::npos);
    EXPECT_FALSE(parse_config(R"({"upstream":{"models":["chat","chat"]}})", c, err));
    EXPECT_NE(err.find("duplicate"), std::string::npos);
    EXPECT_FALSE(parse_config(R"({"upstream":{"models":[]}})", c, err));
    EXPECT_FALSE(parse_config(R"({"upstream":{"models":"chat"}})", c, err));
}

class ConfigReject : public ::testing::TestWithParam<std::pair<const char*, const char*>> {};

TEST_P(ConfigReject, IsRefusedAndNamesTheProblem)
{
    Settings c;
    std::string err;
    EXPECT_FALSE(parse_config(GetParam().first, c, err)) << "accepted: " << GetParam().first;
    EXPECT_NE(err.find(GetParam().second), std::string::npos)
        << "the error did not name the problem. got: " << err;
}

INSTANTIATE_TEST_SUITE_P(
    Cases, ConfigReject,
    ::testing::Values(
        // The one that matters: a misspelling must not be silently ignored.
        std::make_pair(R"({"listen":{"listen_tls":true}})", "unknown key in \"listen\""),
        std::make_pair(R"({"lisen":{"port":1}})", "unknown key in \"(top level)\""),
        std::make_pair(R"({"runtime":{"worker":2}})", "unknown key in \"runtime\""),
        // wrong types
        std::make_pair(R"({"listen":{"tls":"yes"}})", "true or false"),
        std::make_pair(R"({"listen":{"port":"8443"}})", "must be a number"),
        std::make_pair(R"({"upstream":{"url":443}})", "must be a string"),
        // The array form: empty is a config with nowhere to send anything, and a
        // non-object entry is a typo that must not silently become one venue.
        std::make_pair(R"({"upstream":[]})", "must not be an empty array"),
        std::make_pair(R"({"upstream":[1,2]})", "must be an object"),
        std::make_pair(R"({"upstream":[{"dialect":"openai"},{"url":"x:1"}]})", "needs a url"),
        std::make_pair(R"({"upstream":"127.0.0.1:9001"})", "object or an array"),
        // A bare string where a list was meant must not quietly become a no-op on a
        // header the operator believes is being dropped.
        std::make_pair(R"({"upstream":{"strip_headers":"authorization"}})", "array of strings"),
        std::make_pair(R"({"upstream":{"strip_headers":[1]}})", "only strings"),
        std::make_pair(R"({"upstream":{"strip_headers":[""]}})", "empty string"),
        std::make_pair(R"({"listen":"8443"})", "must be an object"),
        // bad enum values, and the message lists what is allowed
        std::make_pair(R"({"upstream":{"dialect":"claude"}})", "must be one of"),
        std::make_pair(R"({"runtime":{"io":"kqueue"}})", "must be one of"),
        std::make_pair(R"({"runtime":{"log_level":"verbose"}})", "must be one of"),
        // out of range: a typo meaning milliseconds must not disable a timeout
        std::make_pair(R"({"listen":{"port":70000}})", "out of range"),
        std::make_pair(R"({"timeouts":{"pool_idle_s":99999999}})", "out of range"),
        std::make_pair(R"({"runtime":{"workers":0}})", "out of range"),
        std::make_pair(R"({"runtime":{"prefault_mb":8192}})", "out of range"),
        // structure
        std::make_pair("[]", "top level must be an object"),
        std::make_pair("{", "not valid JSON"),
        std::make_pair(R"({"listen":{"port":1}} x)", "not valid JSON"),
        // First-wins would apply one copy silently; the other is a typo or a merge.
        std::make_pair(R"({"listen":{"port":1,"port":2}})", "appears twice"),
        std::make_pair(R"({"_c":"a","_c":"b"})", "appears twice"),
        // paths must not carry escapes the zero-copy DOM would hand over undecoded
        std::make_pair(R"({"listen":{"cert":"/a\\b.pem"}})", "backslash")));

// Comments are what let unknown keys be strict at all: without them an operator
// would have no way to annotate the file.
TEST(Config, UnderscoreKeysAreCommentsEverywhere)
{
    Settings c;
    std::string err;
    const std::string text = R"({
      "_why": "top level",
      "listen": { "_why": "nested too", "port": 1234 }
    })";
    ASSERT_TRUE(parse_config(text, c, err)) << err;
    EXPECT_EQ(c.listen_port, 1234);
}

// The shipped example claims every value in it is the built-in default. That claim
// rots the moment a default changes, and a reference file that lies is worse than
// none, so it is pinned here against the constants themselves. If this fails, either
// the example or the default moved and the other has to follow.
TEST(Config, ShippedExampleMatchesTheRealDefaults)
{
    std::ifstream in(LLMBRIDGE_EXAMPLE_CONFIG, std::ios::binary);
    ASSERT_TRUE(in) << "cannot read " << LLMBRIDGE_EXAMPLE_CONFIG;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    Settings c;
    std::string err;
    ASSERT_TRUE(parse_config(text, c, err)) << "the shipped example does not parse: " << err;

    // Mirrors the initialisers of Settings, which mirror the Gateway constants.
    EXPECT_EQ(c.listen_port, 8088);
    EXPECT_FALSE(c.listen_tls);
    EXPECT_TRUE(c.tls_cert.empty()) << "a certificate with tls:false is refused at startup";
    EXPECT_TRUE(c.tls_key.empty());
    EXPECT_TRUE(c.more_upstreams.empty());
    EXPECT_EQ(c.upstream, "127.0.0.1:9001");
    EXPECT_EQ(c.dialect, "openai");
    EXPECT_DOUBLE_EQ(c.upstream_s,
                     static_cast<double>(llmbridge::Gateway::kDefaultUpstreamIdleNs) / 1e9);
    EXPECT_DOUBLE_EQ(c.client_idle_s,
                     static_cast<double>(llmbridge::Gateway::kDefaultClientIdleNs) / 1e9);
    EXPECT_DOUBLE_EQ(c.pool_idle_s,
                     static_cast<double>(llmbridge::Gateway::kDefaultPoolIdleNs) / 1e9);
    EXPECT_DOUBLE_EQ(c.connect_s,
                     static_cast<double>(llmbridge::Gateway::kDefaultConnectNs) / 1e9);
    EXPECT_EQ(c.io, "auto");
    EXPECT_EQ(c.log_level, "info");
    EXPECT_EQ(c.management_port, 0);
    EXPECT_FALSE(c.request_log);
    EXPECT_EQ(c.workers, 1);
    EXPECT_FALSE(c.timing_headers);
    EXPECT_DOUBLE_EQ(c.duration_s, 0);
    EXPECT_DOUBLE_EQ(c.warmup_s, 0);
    EXPECT_DOUBLE_EQ(c.prefault_mb, 0);
    EXPECT_EQ(c.stream_body, "on");
    EXPECT_DOUBLE_EQ(c.body_s, static_cast<double>(llmbridge::Gateway::kDefaultBodyTimeoutNs) / 1e9);
    EXPECT_DOUBLE_EQ(c.body_grace_s, static_cast<double>(llmbridge::Gateway::kDefaultBodyGraceNs) / 1e9);
    EXPECT_EQ(c.body_min_rate, static_cast<int>(llmbridge::Gateway::kDefaultBodyMinRate));
    EXPECT_EQ(c.stream_body_max, static_cast<int>(llmbridge::Gateway::kDefaultStreamBodyMax));

    // Every key in the option table must appear, set or as its `_` comment (cert and
    // key cannot be set while tls is false), or the example stops documenting one.
    bool ok = false;
    const auto root = llmbridge::provider::json::parse(text, ok);
    ASSERT_TRUE(ok);
    for (const llmbridge::app::Option& o : llmbridge::app::options())
    {
        const std::string_view group = o.key.substr(0, o.key.find('.'));
        const std::string name(o.key.substr(o.key.find('.') + 1));
        const auto* g = root.find(group);
        ASSERT_NE(g, nullptr) << group;
        EXPECT_TRUE(g->find(name) != nullptr || g->find("_" + name) != nullptr)
            << "the example does not document " << o.key;
    }
}

TEST(Config, DemoDeploymentConfigParses)
{
    // 演示配置参与实际解析测试，避免部署样例在字段调整后悄悄失效。
    std::ifstream in(LLMBRIDGE_DEMO_CONFIG, std::ios::binary);
    ASSERT_TRUE(in) << "cannot read " << LLMBRIDGE_DEMO_CONFIG;
    std::ostringstream ss;
    ss << in.rdbuf();
    Settings c;
    std::string err;
    ASSERT_TRUE(parse_config(ss.str(), c, err)) << err;
    EXPECT_TRUE(c.listen_tls);
    EXPECT_TRUE(c.security_enabled);
    EXPECT_FALSE(c.allow_insecure_http);
    EXPECT_EQ(c.upstream, "127.0.0.1:9001");
    ASSERT_EQ(c.more_upstreams.size(), 1U);
    EXPECT_EQ(c.more_upstreams[0].url, "127.0.0.1:9002");
    EXPECT_EQ(c.management_port, 9091);
}

// ── The venue dialect: its name, its old name, and its old value ─────────────
//
// "translate" named an action the gateway may not even perform, since a matching
// pair byte-forwards, and "none" read as "do not translate" while meaning "this
// venue speaks OpenAI". That misreading is what made an Anthropic client
// unserviceable against a venue configured this way. Both are refused by name: the
// config and the binary ship together here, so an alias would buy nothing and keep
// the old model alive.
TEST(Config, DialectIsTheName)
{
    Settings c;
    std::string err;
    ASSERT_TRUE(parse_config(R"({"upstream": {"url": "127.0.0.1:9001", "dialect": "anthropic"}})",
                             c, err))
        << err;
    EXPECT_EQ(c.dialect, "anthropic");
}

TEST(Config, TheOldKeyIsRefusedByName)
{
    // Not accepted as an alias. The word said the field decided an action when it
    // names what the venue speaks, so a config still using it holds the old model,
    // and an alias would let that survive with nobody told.
    Settings c;
    std::string err;
    EXPECT_FALSE(parse_config(R"({"upstream": {"url": "127.0.0.1:9001", "translate": "anthropic"}})",
                              c, err));
    EXPECT_NE(err.find("dialect"), std::string::npos) << err;
}

TEST(Config, TheOldValueIsRefusedByName)
{
    Settings c;
    std::string err;
    EXPECT_FALSE(parse_config(R"({"upstream": {"url": "127.0.0.1:9001", "dialect": "none"}})",
                              c, err));
    // The pointed message, not the generic "must be one of" list: that one also
    // names openai, so asserting on it would pass with the guard removed entirely.
    EXPECT_NE(err.find("never an absence"), std::string::npos) << err;
}

TEST(Config, AnUnknownDialectIsRefused)
{
    Settings c;
    std::string err;
    EXPECT_FALSE(parse_config(
        R"({"upstream": {"url": "127.0.0.1:9001", "dialect": "anthropc"}})", c, err));
}

// ── Fixes from the option table ──────────────────────────────────────────────

// strip_headers is gateway-wide. The array form used to accept it per entry and drop
// it, so a header the operator meant to strip was still forwarded.
TEST(Config, ArrayFormStripHeadersIsHonoured)
{
    Settings c;
    std::string err;
    ASSERT_TRUE(parse_config(R"({"upstream":[{"url":"127.0.0.1:1","strip_headers":["authorization"]},
                                             {"url":"127.0.0.1:2"}]})",
                             c, err))
        << err;
    ASSERT_EQ(c.strip_headers.size(), 1u);
    EXPECT_EQ(c.strip_headers[0], "authorization");
}

TEST(Config, ArrayEntriesThatSetStripHeadersMustAgree)
{
    Settings c;
    std::string err;
    EXPECT_TRUE(parse_config(R"({"upstream":[{"url":"a:1","strip_headers":["x-a"]},
                                             {"url":"b:2","strip_headers":["x-a"]}]})",
                             c, err))
        << err;
    EXPECT_FALSE(parse_config(R"({"upstream":[{"url":"a:1","strip_headers":["x-a"]},
                                              {"url":"b:2","strip_headers":["x-b"]}]})",
                              c, err));
    EXPECT_NE(err.find("gateway-wide"), std::string::npos) << err;
}

// The flag accepted these two and the file did not, so a table with a Bedrock or an
// Azure venue past the first could not be written at all.
TEST(Config, BedrockAndAzureAreDialects)
{
    Settings c;
    std::string err;
    ASSERT_TRUE(parse_config(R"({"upstream":[{"url":"a:1","dialect":"bedrock"},
                                             {"url":"b:2","dialect":"azure"}]})",
                             c, err))
        << err;
    EXPECT_EQ(c.dialect, "bedrock");
    ASSERT_EQ(c.more_upstreams.size(), 1u);
    EXPECT_EQ(c.more_upstreams[0].dialect, "azure");
}

// A fraction used to be truncated: port 8088.9 served 8088 and workers 1.7 ran one.
TEST(Config, IntegersRefuseAFraction)
{
    Settings c;
    std::string err;
    EXPECT_FALSE(parse_config(R"({"listen":{"port":8088.9}})", c, err));
    EXPECT_NE(err.find("must be an integer"), std::string::npos) << err;
    EXPECT_FALSE(parse_config(R"({"runtime":{"workers":1.7}})", c, err));
    EXPECT_NE(err.find("must be an integer"), std::string::npos) << err;
}

// Half a second is half a second; main used to cast it to 0, which runs forever.
TEST(Config, DurationKeepsItsFraction)
{
    Settings c;
    std::string err;
    ASSERT_TRUE(parse_config(R"({"runtime":{"duration_s":0.5}})", c, err)) << err;
    EXPECT_DOUBLE_EQ(c.duration_s, 0.5);
}

TEST(Config, ManagementPortIsBounded)
{
    Settings c;
    std::string err;
    EXPECT_FALSE(parse_config(R"({"management":{"port":65536}})", c, err));
    EXPECT_NE(err.find("out of range"), std::string::npos) << err;
    ASSERT_TRUE(parse_config(R"({"management":{"port":0}})", c, err)) << err;
    EXPECT_EQ(c.management_port, 0);
}

// Every row's kind must match the field it writes, or its first use throws.
TEST(Config, EveryOptionAcceptsAValueOfItsKind)
{
    using llmbridge::app::Kind;
    for (const llmbridge::app::Option& o : llmbridge::app::options())
    {
        const std::string group(o.key.substr(0, o.key.find('.')));
        const std::string name(o.key.substr(o.key.find('.') + 1));
        const std::string value = o.kind == Kind::Bool     ? "true"
                                  : o.kind == Kind::Int    ? "1"
                                  : o.kind == Kind::Number ? "1.5"
                                  : o.kind == Kind::Choice ? "\"" + std::string(o.choices[0]) + "\""
                                  : o.kind == Kind::List   ? "[\"x\"]"
                                                           : "\"v\"";
        Settings c;
        std::string err;
        EXPECT_TRUE(parse_config("{\"" + group + "\":{\"" + name + "\":" + value + "}}", c, err))
            << o.key << ": " << err;
    }
}
