// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// A large request body through a running gateway, sent at a set rate: the time from
// the client's first byte to the venue's first byte, to the venue's last byte, and to
// the reply. The venue is a mock in this process, so all three use one clock.
//
//   bigbody --gateway 8088 --venue 9001 --shape transcript --mb 10 --mbit 100 --reps 9
//           [--reply anthropic]
//
// Start the gateway first, pointed at --venue; it connects lazily.

#include "large_bodies.hpp"
#include "net/http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
    int64_t now_ns()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    bool send_all(int fd, std::string_view s)
    {
        while (!s.empty())
        {
            const ssize_t n = ::send(fd, s.data(), s.size(), MSG_NOSIGNAL);
            if (n <= 0) return false;
            s.remove_prefix(static_cast<size_t>(n));
        }
        return true;
    }

    std::atomic<int64_t> g_first{0}, g_done{0};

    void venue(int lfd, std::string reply)
    {
        for (;;)
        {
            const int c = ::accept(lfd, nullptr, nullptr);
            if (c < 0) return;
            std::thread([c, reply] {
                std::string buf;
                std::vector<char> tmp(1 << 20);
                for (;;)
                {
                    llmbridge::net::http::Message m;
                    bool first = true;
                    while (llmbridge::net::http::parse_request(buf, m) != llmbridge::net::http::FrameStatus::Complete)
                    {
                        const ssize_t n = ::read(c, tmp.data(), tmp.size());
                        if (n <= 0) { ::close(c); return; }
                        if (first && buf.empty()) g_first.store(now_ns());
                        first = false;
                        buf.append(tmp.data(), static_cast<size_t>(n));
                    }
                    g_done.store(now_ns());
                    buf.erase(0, m.total_len);
                    if (!send_all(c, reply)) { ::close(c); return; }
                }
            }).detach();
        }
    }

    double pct(std::vector<double> v, double p)
    {
        std::sort(v.begin(), v.end());
        return v[static_cast<size_t>(p / 100 * static_cast<double>(v.size() - 1))];
    }
} // namespace

int main(int argc, char** argv)
{
    int gw_port = 8088, venue_port = 9001, reps = 9;
    double mb = 10, mbit = 100;
    std::string shape_s = "transcript", reply_s = "openai";
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--gateway") gw_port = std::atoi(v.c_str());
        else if (k == "--venue") venue_port = std::atoi(v.c_str());
        else if (k == "--reps") reps = std::atoi(v.c_str());
        else if (k == "--mb") mb = std::atof(v.c_str());
        else if (k == "--mbit") mbit = std::atof(v.c_str());
        else if (k == "--shape") shape_s = v;
        else if (k == "--reply") reply_s = v;
    }
    using llmbridge::bench::Shape;
    const Shape shape = shape_s == "schemas" ? Shape::Schemas : shape_s == "images" ? Shape::Images : Shape::Transcript;
    const std::string body = llmbridge::bench::body(shape, static_cast<size_t>(mb * (1 << 20)));
    const std::string rbody =
        reply_s == "anthropic"
            ? R"({"id":"msg_1","type":"message","role":"assistant","model":"m","content":[{"type":"text","text":"ok"}],"stop_reason":"end_turn","usage":{"input_tokens":5,"output_tokens":1}})"
            : R"({"id":"c1","object":"chat.completion","created":1,"model":"m","choices":[{"index":0,"message":{"role":"assistant","content":"ok"},"finish_reason":"stop"}],"usage":{"prompt_tokens":5,"completion_tokens":1,"total_tokens":6}})";
    const std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(rbody.size()) + "\r\n\r\n" + rbody;

    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<uint16_t>(venue_port));
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd, 16) != 0)
    {
        std::perror("venue bind");
        return 1;
    }
    std::thread(venue, lfd, reply).detach();

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    a.sin_port = htons(static_cast<uint16_t>(gw_port));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
    {
        std::perror("gateway connect");
        return 1;
    }
    const std::string head = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
                             "Authorization: Bearer k\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    const double bytes_per_ns = mbit > 0 ? mbit * 1e6 / 8 / 1e9 : 0;
    std::vector<double> ttfb, upload, e2e;
    std::vector<char> tmp(1 << 16);
    for (int r = -1; r < reps; ++r) // one warm-up
    {
        g_first.store(0);
        const int64_t t0 = now_ns();
        if (!send_all(fd, head)) return 1;
        for (size_t off = 0; off < body.size();)
        {
            // Each chunk waits until the link would have carried the ones before it, and
            // nothing waits after the last: the reply is read as soon as it is sent.
            if (bytes_per_ns > 0)
            {
                const int64_t due = t0 + static_cast<int64_t>(static_cast<double>(off) / bytes_per_ns);
                while (now_ns() < due) std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            const size_t n = std::min<size_t>(64 << 10, body.size() - off);
            if (!send_all(fd, std::string_view(body).substr(off, n))) return 1;
            off += n;
        }
        std::string resp;
        llmbridge::net::http::Message m;
        while (llmbridge::net::http::parse_request(resp, m) != llmbridge::net::http::FrameStatus::Complete)
        {
            const ssize_t n = ::read(fd, tmp.data(), tmp.size());
            if (n <= 0) { std::fprintf(stderr, "gateway closed\n"); return 1; }
            resp.append(tmp.data(), static_cast<size_t>(n));
        }
        const int64_t t_end = now_ns();
        if (resp.compare(9, 3, "200") != 0)
        {
            std::fprintf(stderr, "status %s\n", resp.substr(0, 12).c_str());
            return 1;
        }
        if (r < 0) continue;
        ttfb.push_back(static_cast<double>(g_first.load() - t0) / 1e6);
        upload.push_back(static_cast<double>(g_done.load() - t0) / 1e6);
        e2e.push_back(static_cast<double>(t_end - t0) / 1e6);
    }
    std::printf("bigbody %-10s %5.1f MB %6.0f Mbit/s reply=%-9s | venue first byte p50 %8.2f p99 %8.2f ms | "
                "venue last byte p50 %8.2f p99 %8.2f ms | reply p50 %8.2f p99 %8.2f ms\n",
                shape_s.c_str(), static_cast<double>(body.size()) / (1 << 20), mbit, reply_s.c_str(),
                pct(ttfb, 50), pct(ttfb, 99), pct(upload, 50), pct(upload, 99), pct(e2e, 50), pct(e2e, 99));
    ::close(fd);
    return 0;
}
