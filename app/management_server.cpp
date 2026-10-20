// AI-llm-transplot 扩展：管理请求在专用线程处理，不进入网关事件循环。

#include "management_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace llmbridge::app
{
    namespace
    {
        constexpr size_t kMaxRequestHead = 2048;

        void send_reply(int fd, int code, std::string_view type, std::string_view body)
        {
            const char* status = code == 200 ? "200 OK" : code == 503 ? "503 Service Unavailable"
                                 : code == 405 ? "405 Method Not Allowed"
                                 : code == 431 ? "431 Request Header Fields Too Large"
                                               : "404 Not Found";
            const std::string head = "HTTP/1.1 " + std::string(status) +
                                     "\r\nContent-Type: " + std::string(type) +
                                     "\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                                     std::to_string(body.size()) + "\r\n\r\n";
            for (const std::string_view part : {std::string_view(head), body})
            {
                size_t sent = 0;
                while (sent < part.size())
                {
                    const ssize_t n = ::send(fd, part.data() + sent, part.size() - sent, MSG_NOSIGNAL);
                    if (n <= 0) return;
                    sent += static_cast<size_t>(n);
                }
            }
        }
    } // namespace

    ManagementServer::ManagementServer(uint16_t port, const RoutingPolicy& policy,
                                       const Observability& observability)
        : policy_(policy), observability_(observability)
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener_ < 0) throw std::runtime_error("management socket: " + std::string(std::strerror(errno)));
        int reuse = 1;
        (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener_, 32) != 0)
        {
            const std::string err = "management listen on 127.0.0.1:" + std::to_string(port) +
                                    ": " + std::strerror(errno);
            ::close(listener_);
            listener_ = -1;
            throw std::runtime_error(err);
        }
    }

    ManagementServer::~ManagementServer()
    {
        stop();
        if (listener_ >= 0) ::close(listener_);
    }

    void ManagementServer::start()
    {
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { serve(); });
    }

    void ManagementServer::stop() noexcept
    {
        running_.store(false, std::memory_order_release);
        stopping_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    void ManagementServer::serve() noexcept
    {
        while (!stopping_.load(std::memory_order_acquire))
        {
            pollfd pfd{listener_, POLLIN, 0};
            if (::poll(&pfd, 1, 100) <= 0 || !(pfd.revents & POLLIN)) continue;
            const int client = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) continue;
            timeval timeout{1, 0};
            (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            (void)::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            handle(client);
            ::close(client);
        }
    }

    void ManagementServer::handle(int client) const noexcept
    {
        try
        {
            std::string head;
            while (head.find("\r\n\r\n") == std::string::npos && head.size() < kMaxRequestHead)
            {
                char buf[512];
                const ssize_t n = ::recv(client, buf, sizeof(buf), 0);
                if (n <= 0) return;
                head.append(buf, static_cast<size_t>(n));
            }
            if (head.size() >= kMaxRequestHead)
            {
                send_reply(client, 431, "text/plain; charset=utf-8", "request headers too large\n");
                return;
            }
            const size_t end = head.find("\r\n");
            if (end == std::string::npos) return;
            const std::string_view line(head.data(), end);
            if (!line.starts_with("GET "))
            {
                send_reply(client, 405, "text/plain; charset=utf-8", "method not allowed\n");
                return;
            }
            if (line == "GET /health HTTP/1.1" || line == "GET /health HTTP/1.0")
                send_reply(client, 200, "text/plain; charset=utf-8", "ok\n");
            else if (line == "GET /ready HTTP/1.1" || line == "GET /ready HTTP/1.0")
            {
                const bool ready = running_.load(std::memory_order_acquire) &&
                                   policy_.available_count() > 0;
                send_reply(client, ready ? 200 : 503, "text/plain; charset=utf-8",
                           ready ? "ready\n" : "not ready\n");
            }
            else if (line == "GET /metrics HTTP/1.1" || line == "GET /metrics HTTP/1.0")
            {
                const std::string body = metrics();
                send_reply(client, 200, "text/plain; version=0.0.4; charset=utf-8", body);
            }
            else send_reply(client, 404, "text/plain; charset=utf-8", "not found\n");
        }
        catch (...)
        {
            // 管理线程不能因格式异常或内存不足终止网关数据面。
        }
    }

    std::string ManagementServer::metrics() const
    {
        std::ostringstream out;
        out << "# HELP ai_gateway_upstream_route_selections_total Policy selections, including retries and requests later rejected.\n"
            << "# TYPE ai_gateway_upstream_route_selections_total counter\n"
            << "# HELP ai_gateway_upstream_transport_failures_total Transport failures before a complete reply.\n"
            << "# TYPE ai_gateway_upstream_transport_failures_total counter\n"
            << "# HELP ai_gateway_upstream_available Whether the upstream is outside its cooldown window.\n"
            << "# TYPE ai_gateway_upstream_available gauge\n"
            << "# HELP ai_gateway_upstream_responses_total Parsed upstream response status classes.\n"
            << "# TYPE ai_gateway_upstream_responses_total counter\n"
            << "# HELP ai_gateway_upstream_response_latency_seconds Time to first upstream response byte for final attempts.\n"
            << "# TYPE ai_gateway_upstream_response_latency_seconds summary\n"
            << "# HELP ai_gateway_request_log_dropped_total Structured logs dropped by the bounded queue.\n"
            << "# TYPE ai_gateway_request_log_dropped_total counter\n";
        for (size_t i = 0; i < policy_.upstream_count(); ++i)
        {
            const auto s = policy_.snapshot(i);
            const auto l = observability_.latency(i);
            const std::string label = "{upstream=\"" + std::to_string(i) + "\"}";
            out << "ai_gateway_upstream_route_selections_total" << label << ' ' << s.attempts << '\n'
                << "ai_gateway_upstream_transport_failures_total" << label << ' '
                << s.transport_failures << '\n'
                << "ai_gateway_upstream_available" << label << ' ' << (s.available ? 1 : 0) << '\n'
                << "ai_gateway_upstream_responses_total{upstream=\"" << i
                << "\",class=\"2xx\"} " << s.responses_2xx << '\n'
                << "ai_gateway_upstream_responses_total{upstream=\"" << i
                << "\",class=\"4xx\"} " << s.responses_4xx << '\n'
                << "ai_gateway_upstream_responses_total{upstream=\"" << i
                << "\",class=\"5xx\"} " << s.responses_5xx << '\n'
                << "ai_gateway_upstream_response_latency_seconds_count" << label << ' ' << l.count << '\n'
                << "ai_gateway_upstream_response_latency_seconds_sum" << label << ' '
                << std::setprecision(9) << static_cast<double>(l.total_ns) / 1e9 << '\n';
        }
        out << "ai_gateway_request_log_dropped_total " << observability_.dropped_logs() << '\n';
        return out.str();
    }
} // namespace llmbridge::app
