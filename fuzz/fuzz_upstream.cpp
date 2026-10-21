// AI-llm-transplot 扩展：对上游 URL 配置边界执行覆盖引导模糊测试。

#include "net/upstream.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    using llmbridge::net::parse_upstream;
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const auto parsed = parse_upstream(input);
    if (!parsed.ok())
    {
        assert(!parsed.error.empty());
        return 0;
    }

    assert(size <= 2048);
    assert(!parsed.host.empty() && parsed.host.size() <= 253);
    assert(parsed.port != 0);
    assert(parsed.path.empty() || parsed.path.front() == '/');
    assert(parsed.path.empty() || parsed.path.back() != '/');

    std::string canonical = parsed.tls ? "https://" : "http://";
    canonical += parsed.host;
    const uint16_t default_port = parsed.tls ? 443 : 80;
    if (parsed.port != default_port)
    {
        canonical += ':';
        canonical += std::to_string(parsed.port);
    }
    canonical += parsed.path;
    if (!parsed.query.empty())
    {
        if (parsed.path.empty()) canonical += '/';
        canonical += '?';
        canonical += parsed.query;
    }

    const auto round_trip = parse_upstream(canonical);
    assert(round_trip.ok());
    assert(round_trip.host == parsed.host);
    assert(round_trip.port == parsed.port);
    assert(round_trip.tls == parsed.tls);
    assert(round_trip.path == parsed.path);
    assert(round_trip.query == parsed.query);
    return 0;
}
