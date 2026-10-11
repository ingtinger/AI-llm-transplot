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
#include "core/limits.hpp"
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
        void bytes(Connection* c) { g.on_client_bytes(c); }
        void upstream_bytes(Connection* u) { g.on_upstream_bytes(u); }
        void flush(Connection* c) { g.stream_flush(c); }
        void pump(Connection* u) { g.stream_pump(u); }
        void gone(Connection* c) { g.client_gone(c); }
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
        EXPECT_TRUE(u->doomed) << "an abandoned upstream is closed, never pooled";
        // On the wire at once: finished, and an error reply closes the client.
        EXPECT_EQ(io.calls, (std::vector<std::string>{"close upstream", "send client", "close client"}));
        EXPECT_TRUE(c->doomed);
        EXPECT_EQ(t.stats().errors, 1u);
        EXPECT_EQ(t.stats().requests, 0u);
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
        EXPECT_EQ(t.stats().errors, 2u);
    }

    TEST_F(Core, ASendThatFailsAbortsThePair)
    {
        Connection* c = t.client();
        io.next_send = Sent::Failed;
        t.error_respond(c, 400);
        EXPECT_TRUE(c->doomed);
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
        c->req.saved = "POST /v1/chat/completions HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}";
        ASSERT_EQ(llmbridge::net::http::parse_request(c->req.saved, c->msg),
                  llmbridge::net::http::FrameStatus::Complete);
        ASSERT_TRUE(t.failover(c));
        EXPECT_TRUE(u->doomed);
        ASSERT_NE(c->peer, nullptr);
        EXPECT_EQ(c->peer->upstream_slot, 1) << "the request is dispatched again, to venue 1";
        EXPECT_EQ(c->peer->out.bytes().rfind("POST /v1/chat/completions", 0), 0u);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"close upstream", "connect"}));
        EXPECT_EQ(t.stats().upstream_failovers, 1u);

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
        h.body = llmbridge::net::http::Body::UntilClose;
        io.next_send = Sent::Queued;
        t.begin_stream(u, h);
        EXPECT_EQ(c->req.f.phase, Phase::Streaming);
        EXPECT_TRUE(c->req.f.stream_chunked_out);
        EXPECT_TRUE(u->rbuf.empty()) << "the body was pumped";
        EXPECT_EQ(c->out.bytes().rfind("HTTP/1.1 200", 0), 0u);
        EXPECT_NE(c->out.bytes().find("data: x"), std::string::npos);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"send client", "pause upstream"}));
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
        // Flushed at once and ended: finalized, and a close-delimited reply closes.
        EXPECT_EQ(io.calls, (std::vector<std::string>{"close upstream", "send client", "close client"}));
        EXPECT_EQ(t.stats().requests, 1u);
    }

    TEST_F(Core, AChunkedStreamCutByEofIsTruncated)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        c->req.f.streaming = true;
        c->req.f.stream_body = llmbridge::net::http::Body::Chunked;
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
        c->req.f.streaming = c->req.f.stream_chunked_out = true;
        c->req.f.stream_body = llmbridge::net::http::Body::Chunked;
        c->req.f.stream_keep_alive = c->req.f.stream_ended = true;
        std::string sse;
        ASSERT_TRUE(c->req.chunkdec.feed(std::string("0\r\n\r\n"), sse));
        t.finalize(c);
        EXPECT_EQ(c->peer, nullptr);
        EXPECT_EQ(t.pooled(), 1u);
        EXPECT_EQ(c->req.f.phase, Phase::Idle);
        EXPECT_EQ(t.stats().requests, 1u);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"resume upstream"}));
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
    // A client pipelining past the cap behind a request in flight is paused, not
    // closed, and resumes once its request finishes and the buffer drains.
    TEST_F(Core, PipeliningPastTheCapPausesReadsInsteadOfClosing)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        c->rbuf.assign(llmbridge::detail::kMaxClientBuffered + 1, 'x');
        t.bytes(c);
        EXPECT_FALSE(c->doomed);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"pause client"}));
        EXPECT_EQ(t.stats().client_read_pauses, 1u);
        c->read_paused = true; // what the backend's set_read records
        t.bytes(c);
        EXPECT_EQ(io.calls.size(), 1u) << "one pause per episode";

        unpair(c);
        c->req.f.phase = Phase::Idle;
        c->rbuf.clear();
        t.bytes(c);
        EXPECT_EQ(io.calls.back(), "resume client");
        EXPECT_EQ(t.stats().errors, 0u);
    }

    TEST_F(Core, AnIdleClientIsNeverPausedWhateverItBuffered)
    {
        Connection* c = t.client();
        c->rbuf = "POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\n";
        t.bytes(c);
        EXPECT_FALSE(io.called("pause client"));
    }

    // Interim 100 Continue goes out as a staged reply through the backend's send; it
    // finishes nothing, because the request is not framed yet.
    TEST_F(Core, AnInterimContinueIsStagedAndSentAndFinishesNothing)
    {
        Connection* c = t.client();
        io.next_send = Sent::Queued;
        c->rbuf = "POST / HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 10\r\n\r\n";
        t.bytes(c);
        EXPECT_EQ(c->out.bytes(), "HTTP/1.1 100 Continue\r\n\r\n");
        EXPECT_EQ(io.calls, (std::vector<std::string>{"send client"}));
        EXPECT_EQ(c->req.f.phase, Phase::Idle);
        t.bytes(c);
        EXPECT_EQ(io.calls.size(), 1u) << "sent once per request";
    }

    // A client that goes away mid-request is an error on both backends, wherever
    // its request stood; one that leaves between requests is not.
    TEST_F(Core, AClientGoneMidReplyIsAnErrorAndAnIdleOneIsNot)
    {
        Connection* idle = t.client();
        t.gone(idle);
        EXPECT_TRUE(idle->doomed);
        EXPECT_EQ(t.stats().errors, 0u);

        Connection* c = t.client();
        c->req.f.phase = Phase::Replying; // reply staged, upstream already released
        t.gone(c);
        EXPECT_TRUE(c->doomed);
        EXPECT_EQ(t.stats().errors, 1u);
    }

    TEST_F(Core, AFramedRequestIsDispatchedWithT0StampedBeforeTheParse)
    {
        Connection* c = t.client();
        c->rbuf = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}";
        t.bytes(c);
        ASSERT_NE(c->peer, nullptr);
        EXPECT_EQ(c->req.f.phase, Phase::Dispatched);
        EXPECT_GT(c->req.f.ts_req_recvd, 0);
        EXPECT_LE(c->req.f.ts_req_recvd, c->req.f.ts_req_built);
        EXPECT_TRUE(c->rbuf.empty());
        EXPECT_EQ(io.calls, (std::vector<std::string>{"connect"})) << "a cold upstream sends on connect";
    }
    /// A streaming request in flight on `c` and `u`, the SSE head already sent.
    void streaming(Connection* c, Connection* u, llmbridge::net::http::Body body, uint64_t len = 0)
    {
        c->req.f.streaming = true;
        c->req.f.phase = Phase::Streaming;
        c->req.f.stream_body = body;
        c->req.f.stream_left = len;
        c->req.f.stream_keep_alive = true;
        (void)u;
    }

    // The client stops reading: the backend that can pause stops reading the upstream,
    // once per episode, and resumes when the client drains. T1 without a kernel.
    TEST_F(Core, ASlowClientPausesItsStreamsUpstreamAndADrainResumesIt)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        streaming(c, u, llmbridge::net::http::Body::UntilClose);
        io.next_send = Sent::Queued;
        u->rbuf = "data: 1\n\n";
        t.pump(u);
        u->read_paused = true; // what set_read records in a backend
        u->rbuf = "data: 2\n\n";
        t.pump(u);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"send client", "pause upstream", "send client"}));
        EXPECT_EQ(t.stats().stream_pauses, 1u);

        io.next_send = Sent::Done; // the client drained
        t.flush(c);
        EXPECT_EQ(io.calls.back(), "resume upstream");
        EXPECT_TRUE(c->out.idle());
        EXPECT_FALSE(c->doomed);
    }

    // Where reads cannot be paused, the stream's buffer is capped instead.
    TEST_F(Core, WithoutAPauseASlowClientsStreamIsCappedNotPaused)
    {
        FakeIo capped{t};
        t.attach(capped, /*backpressure=*/false);
        capped.next_send = Sent::Queued;
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        streaming(c, u, llmbridge::net::http::Body::UntilClose);
        u->rbuf.assign(1 << 20, 'x');
        t.pump(u);
        EXPECT_FALSE(capped.called("pause upstream"));
        EXPECT_FALSE(c->doomed);
        for (int i = 0; i < 9 && !c->doomed; ++i)
        {
            u->rbuf.assign(1 << 20, 'x');
            t.pump(u);
        }
        EXPECT_TRUE(c->doomed) << "past kStreamBufCap the stream is dropped";
        EXPECT_EQ(t.stats().errors, 1u);
    }

    // A Content-Length-framed stream ends at its last byte, with the upstream still
    // open, and the upstream is pooled: nothing waits for an EOF that never comes.
    TEST_F(Core, AContentLengthStreamEndsAtItsLastByteWithoutEof)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        const std::string body = "data: x\n\ndata: [DONE]\n\n";
        streaming(c, u, llmbridge::net::http::Body::Length, body.size());
        c->req.f.stream_chunked_out = true;
        u->rbuf = body.substr(0, 10);
        t.pump(u);
        EXPECT_FALSE(c->req.f.stream_ended);
        u->rbuf = body.substr(10);
        t.pump(u);
        EXPECT_TRUE(c->req.f.stream_ended);
        EXPECT_FALSE(c->close_after_resp);
        EXPECT_EQ(t.pooled(), 1u) << "the body ended at a message boundary";
        EXPECT_EQ(t.stats().requests, 1u);
        EXPECT_EQ(c->req.f.phase, Phase::Idle);
    }

    TEST_F(Core, BytesPastAContentLengthStreamKeepTheUpstreamOutOfThePool)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        streaming(c, u, llmbridge::net::http::Body::Length, 9);
        c->req.f.stream_chunked_out = true;
        u->rbuf = "data: x\n\nHTTP/1.1 200 OK\r\n";
        t.pump(u);
        EXPECT_TRUE(c->req.f.stream_ended);
        EXPECT_EQ(t.pooled(), 0u);
        EXPECT_TRUE(u->doomed);
    }

    // NP1: a response arriving over many reads has its head parsed once.
    TEST_F(Core, AResponseHeadIsParsedOnceHoweverManyReadsTheBodyTakes)
    {
        Connection* c = t.client();
        Connection* u = t.upstream();
        t.dispatch(c, u);
        const std::string body(1000, 'a');
        u->rbuf = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 1000\r\n\r\n";
        t.upstream_bytes(u);
        for (size_t at = 0; at < body.size(); at += 100)
        {
            u->rbuf.append(body, at, 100);
            t.upstream_bytes(u);
        }
        EXPECT_EQ(u->framer.head_parses(), 1u);
        EXPECT_EQ(c->req.f.phase, Phase::Idle) << "the reply finished";
        EXPECT_EQ(t.stats().requests, 1u);
        EXPECT_EQ(t.pooled(), 1u);
    }
} // namespace
