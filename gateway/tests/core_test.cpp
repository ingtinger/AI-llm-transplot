// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The shared request lifecycle driven by a FakeIo instead of a socket: no event loop,
// no kernel, so a send that does not complete happens exactly when the test says so.

#include "core/conn.hpp"
#include "core/io.hpp"
#include "core/pool.hpp"
#include "core/registry.hpp"
#include "gateway/gateway.hpp"

#include <gtest/gtest.h>

#include <sys/eventfd.h>

#include <memory>
#include <string>
#include <vector>

namespace llmbridge
{
    using detail::IoCaps;
    using detail::Sent;

    /// Gateway's friend: reaches the shared lifecycle the backends call.
    struct GatewayCoreTest
    {
        Gateway& g;

        void attach(detail::IoOps& io, bool backpressure)
        {
            g.attach(&io, IoCaps{.stream_backpressure = backpressure});
        }
        Connection* client()
        {
            auto* c = new Connection();
            c->fd = ::eventfd(0, EFD_CLOEXEC);
            c->id = 1;
            g._clients->add(c);
            return c;
        }
        /// Connected and registered, as a pooled or freshly connected one would be.
        Connection* upstream(int slot = 0)
        {
            Connection* u = g.new_upstream(slot);
            u->fd = ::eventfd(0, EFD_CLOEXEC);
            u->connected = u->wire_ready = true;
            g._upconns->add(u);
            return u;
        }
        /// What a backend's close does to the bookkeeping; the fd closes at teardown.
        void forget(Connection& c)
        {
            if (c.doomed) return;
            Connection* other = unpair(&c);
            if (c.is_client) g._clients->remove(&c);
            else
            {
                g._pool->remove(c);
                g._upconns->remove(&c);
            }
            c.doomed = true;
            g._doomed->add(&c);
            if (other && c.is_client) forget(*other);
        }
        /// A request framed on `c` and dispatched on `u`, its bytes on `u`'s wire.
        void dispatch(Connection* c, Connection* u)
        {
            c->req.begin(7);
            c->upstream_slot = u->upstream_slot;
            c->msg.keep_alive = c->msg.http_1_1 = true;
            c->req.f.phase = Phase::Dispatched;
            u->out.stage() = "POST / HTTP/1.1\r\n\r\n";
            u->out.sent(u->out.wire().size());
            pair(c, u);
        }

        void error_respond(Connection* c, int code) { g.error_respond(c, code, "test"); }
        Connection* acquire(int slot) { return g.acquire_upstream(slot); }
        bool retry(Connection* u) { return g.retry_upstream(u); }
        void release(Connection* u, bool keep_alive) { g.release_upstream(u, keep_alive, true); }
        bool failover(Connection* c) { return g.upstream_failed(c, 502, "test"); }
        void begin_stream(Connection* u, const net::http::ResponseHead& h) { g.begin_stream(u, h); }
        void upstream_eof(Connection* u) { g.stream_on_upstream_eof(u); }
        void finalize(Connection* c) { g.finalize_stream(c); }
        void abort_pair(Connection* c) { g.abort_pair(c); }
        const Stats& stats() const { return g.stats(); }
        size_t pooled() const { return g.pooled_upstream_count(); }
    };
} // namespace llmbridge

namespace
{
    using llmbridge::Connection;
    using llmbridge::Gateway;
    using llmbridge::GatewayCoreTest;
    using llmbridge::Phase;
    using llmbridge::detail::Sent;

    /// Records every call; a send completes only when `next_send` says Done.
    class FakeIo final : public llmbridge::detail::IoOps
    {
      public:
        explicit FakeIo(GatewayCoreTest& t) noexcept : _t(t) {}
        std::vector<std::string> calls;
        Sent next_send = Sent::Done;
        bool connect_ok = true;

        Sent send(Connection& c) noexcept override
        {
            calls.push_back(c.is_client ? "send client" : "send upstream");
            if (next_send != Sent::Done) return next_send;
            const size_t n = c.out.wire().size();
            c.out.sent(n);
            c.sent_bytes += n;
            return Sent::Done;
        }
        bool connect(Connection& u) noexcept override
        {
            calls.push_back("connect");
            if (!connect_ok) return false;
            u.fd = ::eventfd(0, EFD_CLOEXEC);
            return true;
        }
        void set_read(Connection& c, bool on) noexcept override
        {
            calls.push_back(std::string(on ? "resume " : "pause ") + (c.is_client ? "client" : "upstream"));
        }
        void close(Connection& c) noexcept override
        {
            calls.push_back(c.is_client ? "close client" : "close upstream");
            _t.forget(c);
        }
        void forward(Connection&) noexcept override { calls.push_back("forward"); }
        void reframe(Connection&) noexcept override { calls.push_back("reframe"); }
        void finish(Connection&) noexcept override { calls.push_back("finish"); }
        void stream_pump(Connection&) noexcept override { calls.push_back("stream_pump"); }
        void stream_flush(Connection&) noexcept override { calls.push_back("stream_flush"); }

        bool called(const std::string& what) const
        {
            for (const std::string& c : calls)
                if (c == what) return true;
            return false;
        }

      private:
        GatewayCoreTest& _t;
    };

    /// Fails over every failure to venue 1.
    class ToSecond final : public llmbridge::Policy
    {
      public:
        llmbridge::Decision decide(const llmbridge::RequestFacts&) noexcept override { return {}; }
        llmbridge::Retry on_failure(const llmbridge::FailureFacts&) noexcept override
        {
            return {.retry = true, .upstream_index = 1};
        }
    };

    struct Core : ::testing::Test
    {
        std::unique_ptr<Gateway> gw = std::make_unique<Gateway>(0, "127.0.0.1", 9);
        GatewayCoreTest t{*gw};
        FakeIo io{t};
        void SetUp() override { t.attach(io, /*backpressure=*/true); }
    };

    TEST_F(Core, AnErrorReplyIsStagedOnceAndFinishesWhenItIsOnTheWire)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        t.error_respond(c, 502);
        EXPECT_EQ(c->req.f.phase, Phase::Replying);
        EXPECT_TRUE(c->close_after_resp);
        EXPECT_EQ(c->peer, nullptr);
        EXPECT_TRUE(u->doomed) << "an abandoned upstream is closed, never pooled";
        EXPECT_EQ(io.calls, (std::vector<std::string>{"close upstream", "send client", "finish"}));
        EXPECT_EQ(t.stats().errors, 1u);
    }

    // The client stopped reading: its reply is queued behind a full socket. A second
    // failure must not stage a second reply behind the first, and nothing finishes.
    TEST_F(Core, ASlowClientsQueuedReplyIsNeverFollowedByASecondOne)
    {
        Connection* c = t.client();
        io.next_send = Sent::Queued;
        t.error_respond(c, 400);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"send client"}));
        EXPECT_FALSE(c->out.idle());
        const std::string staged = c->out.bytes();

        t.error_respond(c, 502);
        EXPECT_TRUE(c->doomed) << "a reply already begun is aborted, not followed";
        EXPECT_EQ(c->out.bytes(), staged);
        EXPECT_FALSE(io.called("finish"));
        EXPECT_EQ(t.stats().errors, 2u);
    }

    TEST_F(Core, ASendThatFailsAbortsThePair)
    {
        Connection* c = t.client();
        io.next_send = Sent::Failed;
        t.error_respond(c, 400);
        EXPECT_TRUE(c->doomed);
        EXPECT_FALSE(io.called("finish"));
        EXPECT_EQ(t.stats().errors, 2u) << "the reply, then the abort";
    }

    TEST_F(Core, ARequestStillQueuedForTheUpstreamKeepsItOutOfThePool)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        u->out.stage() += "more request bytes";
        unpair(c);
        t.release(u, /*keep_alive=*/true);
        EXPECT_TRUE(u->doomed);
        EXPECT_EQ(t.pooled(), 0u);
        EXPECT_EQ(t.stats().upstream_unsent, 1u);
    }

    TEST_F(Core, APooledUpstreamIsReadableWhereABackendPausesStreams)
    {
        Connection* u = t.upstream();
        t.release(u, /*keep_alive=*/true);
        EXPECT_EQ(t.pooled(), 1u);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"resume upstream"}));

        FakeIo capped{t};
        t.attach(capped, /*backpressure=*/false);
        t.release(t.upstream(), /*keep_alive=*/true);
        EXPECT_EQ(t.pooled(), 2u);
        EXPECT_TRUE(capped.calls.empty()) << "a backend without a pause has nothing to resume";
    }

    TEST_F(Core, AcquireTakesThePooledConnectionBeforeOpeningOne)
    {
        Connection* u = t.upstream();
        t.release(u, true);
        io.calls.clear();
        EXPECT_EQ(t.acquire(0), u);
        EXPECT_TRUE(u->from_pool);
        EXPECT_TRUE(io.calls.empty());

        Connection* fresh = t.acquire(0);
        ASSERT_NE(fresh, nullptr);
        EXPECT_FALSE(fresh->from_pool);
        EXPECT_GE(fresh->fd, 0);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"connect"}));
        EXPECT_EQ(t.stats().upstream_conns_opened, 1u);
    }

    TEST_F(Core, AConnectThatFailsAtOnceLeavesNothingBehind)
    {
        const long live = Connection::s_live.load();
        io.connect_ok = false;
        EXPECT_EQ(t.acquire(0), nullptr);
        EXPECT_EQ(Connection::s_live.load(), live);
        EXPECT_EQ(t.stats().upstream_conns_opened, 0u);
    }

    TEST_F(Core, AStalePooledUpstreamIsRetriedOnceOnAFreshConnection)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.release(u, true);
        ASSERT_EQ(t.acquire(0), u);
        t.dispatch(c, u);
        const std::string request = u->out.bytes();

        ASSERT_TRUE(t.retry(u));
        Connection* fresh = c->peer;
        ASSERT_NE(fresh, nullptr);
        EXPECT_NE(fresh, u);
        EXPECT_TRUE(u->doomed);
        EXPECT_TRUE(fresh->retried);
        EXPECT_EQ(fresh->out.bytes(), request) << "the same bytes, resent";
        EXPECT_EQ(t.stats().upstream_retries, 1u);
        EXPECT_FALSE(t.retry(fresh)) << "one retry per request";
    }

    TEST_F(Core, NoRetryOnceAResponseByteArrived)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.release(u, true);
        ASSERT_EQ(t.acquire(0), u);
        t.dispatch(c, u);
        c->req.f.phase = Phase::Responding;
        EXPECT_FALSE(t.retry(u));
        EXPECT_EQ(c->peer, u);
    }

    TEST(CoreFailover, ARequestThatFailedBeforeAnyResponseIsForwardedToTheNextVenue)
    {
        ToSecond policy;
        std::vector<llmbridge::Upstream> ups{{.ip = "127.0.0.1", .port = 9}, {.ip = "127.0.0.1", .port = 10}};
        Gateway gw(0, ups, 0, llmbridge::IoBackend::Epoll, Gateway::kDefaultUpstreamIdleNs, {}, false,
                   &policy, {});
        GatewayCoreTest t{gw};
        FakeIo io{t};
        t.attach(io, true);
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        c->req.saved = "POST / HTTP/1.1\r\n\r\n";
        ASSERT_TRUE(t.failover(c));
        EXPECT_EQ(c->upstream_slot, 1);
        EXPECT_EQ(c->rbuf, c->req.saved);
        EXPECT_TRUE(u->doomed);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"close upstream", "forward"}));

        c->req.f.phase = Phase::Responding;
        EXPECT_FALSE(t.failover(c)) << "the venue may have acted";
    }

    TEST_F(Core, AStreamStagesItsHeadAndPumps)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        u->rbuf = "HTTP/1.1 200 OK\r\n\r\ndata: x\n\n";
        llmbridge::net::http::ResponseHead h;
        h.header_len = 19;
        h.keep_alive = true;
        t.begin_stream(u, h);
        EXPECT_EQ(c->req.f.phase, Phase::Streaming);
        EXPECT_TRUE(c->req.f.stream_chunked_out);
        EXPECT_EQ(u->rbuf, "data: x\n\n");
        EXPECT_EQ(c->out.bytes().rfind("HTTP/1.1 200", 0), 0u);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"stream_pump"}));
    }

    TEST_F(Core, AnUpstreamEofEndsACloseDelimitedStreamAndClosesTheUpstream)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        c->req.f.streaming = true;
        c->req.f.phase = Phase::Streaming;
        u->rbuf = "data: [DONE]\n\n";
        t.upstream_eof(u);
        EXPECT_TRUE(c->req.f.stream_ended);
        EXPECT_FALSE(c->close_after_resp);
        EXPECT_TRUE(u->doomed);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"close upstream", "stream_flush"}));
    }

    TEST_F(Core, AChunkedStreamCutByEofIsTruncated)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        c->req.f.streaming = c->req.f.stream_chunked = true;
        c->req.f.phase = Phase::Streaming;
        u->rbuf = "5\r\ndata:";
        t.upstream_eof(u);
        EXPECT_TRUE(c->close_after_resp) << "no terminal chunk: truncated, never clean";
        EXPECT_EQ(t.stats().errors, 1u);
    }

    TEST_F(Core, AFinishedStreamPoolsItsUpstreamAndReframes)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        c->req.f.streaming = c->req.f.stream_chunked = c->req.f.stream_chunked_out = true;
        c->req.f.stream_keep_alive = c->req.f.stream_ended = true;
        std::string sse;
        ASSERT_TRUE(c->req.chunkdec.feed(std::string("0\r\n\r\n"), sse));
        t.finalize(c);
        EXPECT_EQ(c->peer, nullptr);
        EXPECT_EQ(t.pooled(), 1u);
        EXPECT_EQ(c->req.f.phase, Phase::Idle);
        EXPECT_EQ(t.stats().requests, 1u);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"resume upstream", "reframe"}));
    }

    TEST_F(Core, ATruncatedStreamClosesTheClient)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        c->req.f.streaming = c->req.f.stream_chunked_out = c->req.f.stream_ended = true;
        c->close_after_resp = true;
        t.finalize(c);
        EXPECT_TRUE(c->doomed);
        EXPECT_TRUE(u->doomed) << "an aborted exchange is never pooled";
        EXPECT_EQ(t.stats().requests, 0u);
        EXPECT_FALSE(io.called("reframe"));
    }

    TEST_F(Core, AbortClosesBothLegsAndCountsOneError)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        t.abort_pair(c);
        EXPECT_TRUE(c->doomed);
        EXPECT_TRUE(u->doomed);
        EXPECT_EQ(t.stats().errors, 1u);
        t.abort_pair(nullptr);
        EXPECT_EQ(t.stats().errors, 1u);
    }
} // namespace
