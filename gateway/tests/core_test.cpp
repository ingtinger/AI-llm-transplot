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
#include "engine.hpp"
#include "gateway/gateway.hpp"
#include "net/socket_util.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

namespace llmbridge
{
    using detail::IoCaps;
    using detail::Sent;

    /// Gateway's friend: reaches the Engine, whose lifecycle the backends call.
    struct GatewayCoreTest
    {
        Gateway& g;
        detail::Engine& e = *g._e;

        void attach(detail::IoOps& io, bool backpressure)
        {
            // Without a pause the cap is the only bound, so it is set exactly then.
            e.attach(&io, IoCaps{.stream_backpressure = backpressure,
                                 .stream_cap = backpressure ? 0 : detail::kStreamBufCap});
        }
        void attach(detail::IoOps& io, const IoCaps& caps) { e.attach(&io, caps); }
        ClientConn* client()
        {
            auto* c = new ClientConn();
            c->fd = ::eventfd(0, EFD_CLOEXEC);
            c->id = 1;
            e._clients->add(c);
            return c;
        }
        /// Connected and registered, as a pooled or freshly connected one would be.
        UpstreamConn* upstream(int slot = 0)
        {
            UpstreamConn* u = e.new_upstream(slot);
            u->fd = ::eventfd(0, EFD_CLOEXEC);
            u->connected = u->wire_ready = true;
            e._upconns->add(u);
            return u;
        }
        /// A backend's close, less the fd, which closes at teardown.
        void forget(ConnBase& c)
        {
            if (c.doomed) return;
            if (ConnBase* other = e.forget(c)) forget(*other);
        }
        /// A request framed on `c` and dispatched on `u`, its bytes on `u`'s wire.
        void dispatch(ClientConn* c, UpstreamConn* u)
        {
            c->req.begin(7);
            c->upstream_slot = u->venue;
            c->msg.keep_alive = c->msg.http_1_1 = true;
            c->req.f.phase = Phase::Dispatched;
            u->out.stage() = "POST / HTTP/1.1\r\n\r\n";
            u->out.sent(u->out.wire().size());
            pair(c, u);
        }

        void error_respond(ClientConn* c, int code) { e.error_respond(c, code, "test"); }
        UpstreamConn* acquire(int slot) { return e.acquire_upstream(slot); }
        bool retry(UpstreamConn* u) { return e.retry_upstream(u); }
        void release(UpstreamConn* u, bool keep_alive) { e.release_upstream(u, keep_alive, true); }
        bool failover(ClientConn* c) { return e.upstream_failed(c, 502, "test"); }
        void begin_stream(UpstreamConn* u, const net::http::ResponseHead& h) { e.begin_stream(u, h); }
        void upstream_eof(UpstreamConn* u) { e.stream_on_upstream_eof(u); }
        void finalize(ClientConn* c) { e.finalize_stream(c); }
        void abort_pair(ClientConn* c) { e.abort_pair(c); }
        void bytes(ClientConn* c) { e.on_client_bytes(c); }
        void upstream_bytes(UpstreamConn* u) { e.on_upstream_bytes(u); }
        void flush(ClientConn* c) { e.stream_flush(c); }
        void pump(UpstreamConn* u) { e.stream_pump(u); }
        void gone(ClientConn* c) { e.client_gone(c); }
        const Stats& stats() const { return g.stats(); }
        size_t pooled() const { return g.pooled_upstream_count(); }
        size_t bodies_streaming() const { return e._bodies_streaming; }
        void sweep(ClientConn* c, int64_t now) { e.sweep_client(c, now, /*busy=*/true); }
    };
} // namespace llmbridge

namespace
{
    using llmbridge::ClientConn;
    using llmbridge::ConnBase;
    using llmbridge::ConnKind;
    using llmbridge::UpstreamConn;
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
        bool connect_ok = true;          ///< false: every connect is refused with connect_errno
        int connect_errno = ENETUNREACH;
        std::vector<int> refusals;       ///< the next connects refused, in order, before one starts
        std::string client_wire;         ///< every byte a client was sent

        Sent send(ConnBase& c) noexcept override
        {
            calls.push_back(c.kind == ConnKind::Client ? "send client" : "send upstream");
            if (next_send != Sent::Done) return next_send;
            const size_t n = c.out.wire().size();
            if (c.kind == ConnKind::Client) client_wire.append(c.out.wire().data(), n);
            c.out.sent(n);
            c.sent_bytes += n;
            return Sent::Done;
        }
        int connect(UpstreamConn& u) noexcept override
        {
            calls.push_back("connect");
            if (!connect_ok) return connect_errno;
            if (!refusals.empty())
            {
                const int err = refusals.front();
                refusals.erase(refusals.begin());
                return err;
            }
            u.fd = ::eventfd(0, EFD_CLOEXEC);
            return 0;
        }
        void set_read(ConnBase& c, bool on) noexcept override
        {
            calls.push_back(std::string(on ? "resume " : "pause ") +
                            (c.kind == ConnKind::Client ? "client" : "upstream"));
        }
        void close(ConnBase& c) noexcept override
        {
            calls.push_back(c.kind == ConnKind::Client ? "close client" : "close upstream");
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

    // Either side of an unpair leaves both links clear: a link is never one-sided.
    TEST_F(Core, PairAndUnpairWriteBothDirections)
    {
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
        llmbridge::pair(c, u);
        EXPECT_EQ(c->up, u);
        EXPECT_EQ(u->client, c);
        EXPECT_EQ(llmbridge::unpair(c), u);
        EXPECT_EQ(c->up, nullptr);
        EXPECT_EQ(u->client, nullptr);
        llmbridge::pair(c, u);
        EXPECT_EQ(llmbridge::unpair(u), c);
        EXPECT_EQ(c->up, nullptr);
        EXPECT_EQ(u->client, nullptr);
    }

    TEST_F(Core, AnErrorReplyIsStagedOnceAndFinishesWhenItIsOnTheWire)
    {
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
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
        ClientConn* c = t.client();
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
        ClientConn* c = t.client();
        io.next_send = Sent::Failed;
        t.error_respond(c, 400);
        EXPECT_TRUE(c->doomed);
        EXPECT_EQ(t.stats().errors, 2u) << "the reply, then the abort";
    }

    TEST_F(Core, ARequestStillQueuedForTheUpstreamKeepsItOutOfThePool)
    {
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
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
        UpstreamConn* u = t.upstream();
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
        UpstreamConn* u = t.upstream();
        t.release(u, true);
        io.calls.clear();
        EXPECT_EQ(t.acquire(0), u);
        EXPECT_TRUE(u->from_pool);
        EXPECT_TRUE(io.calls.empty());

        UpstreamConn* fresh = t.acquire(0);
        ASSERT_NE(fresh, nullptr);
        EXPECT_FALSE(fresh->from_pool);
        EXPECT_GE(fresh->fd, 0);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"connect"}));
        EXPECT_EQ(t.stats().upstream_conns_opened, 1u);
    }

    TEST_F(Core, AConnectThatFailsAtOnceLeavesNothingBehind)
    {
        const long live = ConnBase::s_live.load();
        io.connect_ok = false;
        EXPECT_EQ(t.acquire(0), nullptr);
        EXPECT_EQ(ConnBase::s_live.load(), live);
        EXPECT_EQ(t.stats().upstream_conns_opened, 0u);
    }

    TEST_F(Core, AStalePooledUpstreamIsRetriedOnceOnAFreshConnection)
    {
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
        t.release(u, true);
        ASSERT_EQ(t.acquire(0), u);
        t.dispatch(c, u);
        const std::string request = u->out.bytes();

        ASSERT_TRUE(t.retry(u));
        UpstreamConn* fresh = c->up;
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
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
        t.release(u, true);
        ASSERT_EQ(t.acquire(0), u);
        t.dispatch(c, u);
        c->req.f.phase = Phase::Responding;
        EXPECT_FALSE(t.retry(u));
        EXPECT_EQ(c->up, u);
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
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
        t.dispatch(c, u);
        c->req.saved = "POST /v1/chat/completions HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}";
        ASSERT_EQ(llmbridge::net::http::parse_request(c->req.saved, c->msg),
                  llmbridge::net::http::FrameStatus::Complete);
        ASSERT_TRUE(t.failover(c));
        EXPECT_TRUE(u->doomed);
        ASSERT_NE(c->up, nullptr);
        EXPECT_EQ(c->up->venue, 1) << "the request is dispatched again, to venue 1";
        EXPECT_EQ(c->up->out.bytes().rfind("POST /v1/chat/completions", 0), 0u);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"close upstream", "connect"}));
        EXPECT_EQ(t.stats().upstream_failovers, 1u);

        c->req.f.phase = Phase::Responding;
        EXPECT_FALSE(t.failover(c)) << "the venue may have acted";
    }

    /// Sends every request to venue 0 tagged 0xBEEF, fails each failure over to the next
    /// venue, and keeps what it was told.
    class Walk final : public llmbridge::Policy
    {
      public:
        llmbridge::Decision decide(const llmbridge::RequestFacts&) noexcept override
        {
            return {.allow = true, .upstream_index = 0, .tag = 0xBEEF};
        }
        llmbridge::Retry on_failure(const llmbridge::FailureFacts& f) noexcept override
        {
            seen.push_back(f);
            return {.retry = true, .upstream_index = f.attempt + 1};
        }
        std::vector<llmbridge::FailureFacts> seen;
    };

    /// One venue per address, ports 9, 10, ..., behind a Walk policy.
    struct Venues
    {
        explicit Venues(const std::vector<std::string>& ips)
            : gw(0, table(ips), 0, llmbridge::IoBackend::Epoll, Gateway::kDefaultUpstreamIdleNs, {}, false,
                 &policy, {})
        {
            t.attach(io, true);
        }
        static std::vector<llmbridge::Upstream> table(const std::vector<std::string>& ips)
        {
            std::vector<llmbridge::Upstream> ups;
            for (const std::string& ip : ips)
                ups.push_back({.ip = ip, .port = static_cast<uint16_t>(9 + ups.size())});
            return ups;
        }
        /// The status line's code of what the client was sent, 0 for nothing.
        int client_status() const
        {
            const std::string& w = io.client_wire;
            return w.size() >= 12 ? std::stoi(w.substr(9, 3)) : 0;
        }
        Walk policy;
        Gateway gw;
        GatewayCoreTest t{gw};
        FakeIo io{t};
    };

    const std::string kChat = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}";

    // A first attempt whose connect fails at once, as epoll learns an unreachable network
    // inside connect(), had kept nothing for a failover to resend: a 502 where the same
    // answer arriving later, as io_uring learns it, failed over.
    TEST(CoreFailover, AFirstAttemptWhoseConnectFailsAtOnceFailsOver)
    {
        Venues v({"127.0.0.1", "127.0.0.1"});
        v.io.refusals = {ENETUNREACH};
        ClientConn* c = v.t.client();
        c->rbuf = kChat + "GET /next"; // the start of a pipelined request, behind it
        v.t.bytes(c);
        ASSERT_NE(c->up, nullptr);
        EXPECT_EQ(c->up->venue, 1);
        ASSERT_EQ(v.policy.seen.size(), 1u);
        EXPECT_EQ(v.policy.seen[0].upstream_index, 0);
        EXPECT_STREQ(v.policy.seen[0].reason, "no upstream (connect failed)");
        EXPECT_EQ(c->up->out.bytes().rfind("POST /v1/chat/completions", 0), 0u);
        EXPECT_EQ(c->rbuf, "GET /next") << "what was pipelined stays, once";
        EXPECT_EQ(v.io.calls, (std::vector<std::string>{"connect", "connect"}));
        EXPECT_EQ(v.t.stats().upstream_failovers, 1u);
    }

    // A venue whose name has not resolved has no address to dial; it is passed over as
    // any venue that cannot be reached is.
    TEST(CoreFailover, AFirstChoiceWithNoAddressFailsOver)
    {
        Venues v({"", "127.0.0.1"});
        ClientConn* c = v.t.client();
        c->rbuf = kChat;
        v.t.bytes(c);
        ASSERT_NE(c->up, nullptr);
        EXPECT_EQ(c->up->venue, 1);
        ASSERT_EQ(v.policy.seen.size(), 1u);
        EXPECT_STREQ(v.policy.seen[0].reason, "no upstream (venue unresolved)");
        EXPECT_EQ(v.io.calls, (std::vector<std::string>{"connect"})) << "venue 0 was never dialled";
        EXPECT_TRUE(c->rbuf.empty());
    }

    // This host out of descriptors, local ports or memory is no venue's fault, and every
    // other venue would need the same: a 503, and the policy hears nothing.
    TEST(CoreFailover, AConnectThatFailsForWantOfALocalResourceFailsNothingOver)
    {
        for (const int err : {EMFILE, ENFILE, ENOBUFS, ENOMEM, EAGAIN, EADDRNOTAVAIL})
        {
            Venues v({"127.0.0.1", "127.0.0.1"});
            v.io.refusals = {err};
            ClientConn* c = v.t.client();
            c->rbuf = kChat;
            v.t.bytes(c);
            EXPECT_TRUE(v.policy.seen.empty()) << err;
            EXPECT_EQ(v.t.stats().upstream_failovers, 0u) << err;
            EXPECT_EQ(v.client_status(), 503) << err;
            EXPECT_TRUE(c->doomed) << err;
        }
    }

    // On a failover attempt too: the copy put back for it is dropped, never left to run
    // as a second request.
    TEST(CoreFailover, ALocalFailureOnAFailoverAttemptAnswers503Once)
    {
        Venues v({"127.0.0.1", "127.0.0.1"});
        // The first refusal is the venue's and fails over; the second is this host's.
        v.io.refusals = {ENETUNREACH, EMFILE};
        ClientConn* c = v.t.client();
        c->rbuf = kChat;
        v.t.bytes(c);
        ASSERT_EQ(v.policy.seen.size(), 1u);
        EXPECT_EQ(v.client_status(), 503);
        EXPECT_TRUE(c->rbuf.empty()) << "the failover's copy of the request was left behind";
    }

    // What a policy keys its own state on: the request's sequencer, the one its decision
    // and its sink record carry, and the decision's tag.
    TEST(CoreFailover, TheFailureCarriesTheRequestsSequencerAndTag)
    {
        Venues v({"127.0.0.1", "127.0.0.1"});
        v.io.refusals = {ENETUNREACH};
        ClientConn* c = v.t.client();
        c->rbuf = kChat;
        v.t.bytes(c);
        ASSERT_EQ(v.policy.seen.size(), 1u);
        const llmbridge::FailureFacts& f = v.policy.seen[0];
        EXPECT_EQ(f.seq, c->req.f.req_seq);
        EXPECT_EQ(f.tag, 0xBEEFu);
        EXPECT_EQ(f.attempt, 0);
        EXPECT_TRUE(f.retryable);
    }

    // The budget spent: the third venue's failure is still the policy's to count, though
    // nothing more is tried.
    TEST(CoreFailover, TheThirdVenuesFailureIsReportedButNotRetried)
    {
        Venues v({"127.0.0.1", "127.0.0.1", "127.0.0.1", "127.0.0.1"});
        v.io.refusals = {ENETUNREACH, ENETUNREACH, ENETUNREACH};
        ClientConn* c = v.t.client();
        c->rbuf = kChat;
        v.t.bytes(c);
        ASSERT_EQ(v.policy.seen.size(), 3u);
        EXPECT_TRUE(v.policy.seen[0].retryable);
        EXPECT_TRUE(v.policy.seen[1].retryable);
        EXPECT_FALSE(v.policy.seen[2].retryable);
        EXPECT_EQ(v.policy.seen[2].attempt, 2);
        EXPECT_EQ(v.policy.seen[2].upstream_index, 2);
        EXPECT_EQ(v.t.stats().upstream_failovers, 2u);
        EXPECT_EQ(v.client_status(), 502);
    }

    // One venue: nowhere to fail over to, and still a failure to count.
    TEST(CoreFailover, AOneVenueTablesFailureIsReportedButNotRetried)
    {
        Venues v({"127.0.0.1"});
        v.io.refusals = {ENETUNREACH};
        ClientConn* c = v.t.client();
        c->rbuf = kChat;
        v.t.bytes(c);
        ASSERT_EQ(v.policy.seen.size(), 1u);
        EXPECT_FALSE(v.policy.seen[0].retryable);
        EXPECT_EQ(v.client_status(), 502);
    }

    /// A request dispatched on `u`, kept for a failover as dispatch keeps one.
    void dispatched(GatewayCoreTest& t, ClientConn* c, UpstreamConn* u)
    {
        t.dispatch(c, u);
        c->req.saved = kChat;
        ASSERT_EQ(llmbridge::net::http::parse_request(c->req.saved, c->msg),
                  llmbridge::net::http::FrameStatus::Complete);
    }

    // A venue that answered part of a response and then failed (EOF, silence) may have
    // acted on the request: reported, never redone.
    TEST(CoreFailover, AFailureAfterAResponseByteIsReportedButNotRetried)
    {
        Venues v({"127.0.0.1", "127.0.0.1"});
        ClientConn* c = v.t.client();
        UpstreamConn* u = v.t.upstream();
        dispatched(v.t, c, u);
        c->req.f.phase = Phase::Responding;
        EXPECT_FALSE(v.t.failover(c));
        ASSERT_EQ(v.policy.seen.size(), 1u);
        EXPECT_FALSE(v.policy.seen[0].retryable);
        EXPECT_EQ(v.t.stats().upstream_failovers, 0u);
    }

    // Not this kind of failure: a reply already begun (a stream's head, an interim 100
    // Continue still owed), or a parked body no venue holds.
    TEST(CoreFailover, NothingIsReportedOnceTheClientWasSentBytesOrNoVenueHadIt)
    {
        {
            Venues v({"127.0.0.1", "127.0.0.1"});
            ClientConn* c = v.t.client();
            dispatched(v.t, c, v.t.upstream());
            c->req.f.phase = Phase::Streaming;
            EXPECT_FALSE(v.t.failover(c));
            EXPECT_TRUE(v.policy.seen.empty()) << "streaming";
        }
        {
            Venues v({"127.0.0.1", "127.0.0.1"});
            ClientConn* c = v.t.client();
            dispatched(v.t, c, v.t.upstream());
            c->req.f.body_parked = true;
            EXPECT_FALSE(v.t.failover(c));
            EXPECT_TRUE(v.policy.seen.empty()) << "parked";
        }
        {
            Venues v({"127.0.0.1", "127.0.0.1"});
            ClientConn* c = v.t.client();
            dispatched(v.t, c, v.t.upstream());
            c->out.stage() = "HTTP/1.1 100 Continue\r\n\r\n";
            EXPECT_FALSE(v.t.failover(c));
            EXPECT_TRUE(v.policy.seen.empty()) << "100 Continue owed";
        }
    }

#ifdef LLMBRIDGE_HAVE_TLS
    // Neither leg moved and the client still owes a read, here only of TLS records: the
    // sweep calls that a client stall, and the venue is not reported for it.
    TEST(CoreFailover, AClientStalledOnItsTlsRecordsIsNotTheVenuesFailure)
    {
        Venues v({"127.0.0.1", "127.0.0.1"});
        ClientConn* c = v.t.client();
        dispatched(v.t, c, v.t.upstream());
        c->ever_framed = true;
        c->req.f.ts_progress = 1;
        c->tls_out.stage() = "a record the client has not read";
        v.t.sweep(c, 1 + Gateway::kDefaultUpstreamIdleNs + 1);
        EXPECT_TRUE(v.policy.seen.empty());
        EXPECT_EQ(v.client_status(), 504);
    }
#endif

    TEST_F(Core, AStreamStagesItsHeadAndPumps)
    {
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
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
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
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
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
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
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
        t.dispatch(c, u);
        c->req.f.streaming = c->req.f.stream_chunked_out = true;
        c->req.f.stream_body = llmbridge::net::http::Body::Chunked;
        c->req.f.stream_keep_alive = c->req.f.stream_ended = true;
        std::string sse;
        ASSERT_TRUE(c->req.chunkdec.feed(std::string("0\r\n\r\n"), sse));
        t.finalize(c);
        EXPECT_EQ(c->up, nullptr);
        EXPECT_EQ(t.pooled(), 1u);
        EXPECT_EQ(c->req.f.phase, Phase::Idle);
        EXPECT_EQ(t.stats().requests, 1u);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"resume upstream"}));
    }

    TEST_F(Core, ATruncatedStreamClosesTheClient)
    {
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
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
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
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
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
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
        ClientConn* c = t.client();
        c->rbuf = "POST / HTTP/1.1\r\nContent-Length: 10\r\n\r\n";
        t.bytes(c);
        EXPECT_FALSE(io.called("pause client"));
    }

    // Interim 100 Continue goes out as a staged reply through the backend's send; it
    // finishes nothing, because the request is not framed yet.
    TEST_F(Core, AnInterimContinueIsStagedAndSentAndFinishesNothing)
    {
        ClientConn* c = t.client();
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
        ClientConn* idle = t.client();
        t.gone(idle);
        EXPECT_TRUE(idle->doomed);
        EXPECT_EQ(t.stats().errors, 0u);

        ClientConn* c = t.client();
        c->req.f.phase = Phase::Replying; // reply staged, upstream already released
        t.gone(c);
        EXPECT_TRUE(c->doomed);
        EXPECT_EQ(t.stats().errors, 1u);
    }

    TEST_F(Core, AFramedRequestIsDispatchedWithT0StampedBeforeTheParse)
    {
        ClientConn* c = t.client();
        c->rbuf = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Length: 2\r\n\r\n{}";
        t.bytes(c);
        ASSERT_NE(c->up, nullptr);
        EXPECT_EQ(c->req.f.phase, Phase::Dispatched);
        EXPECT_GT(c->req.f.ts_req_recvd, 0);
        EXPECT_LE(c->req.f.ts_req_recvd, c->req.f.ts_req_built);
        EXPECT_TRUE(c->rbuf.empty());
        EXPECT_EQ(io.calls, (std::vector<std::string>{"connect"})) << "a cold upstream sends on connect";
    }
    /// A streaming request in flight on `c` and `u`, the SSE head already sent.
    void streaming(ClientConn* c, UpstreamConn* u, llmbridge::net::http::Body body, uint64_t len = 0)
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
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
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

    // A backend whose sends are always queued while in flight (io_uring) pauses only
    // once the stream owes more than its threshold, resumes when the client drains, and
    // still drops a stream past the cap: bytes read before the pause keep landing.
    TEST_F(Core, AQueuedSendPausesOnlyPastTheThresholdAndTheCapStaysACeiling)
    {
        FakeIo uring{t};
        t.attach(uring, llmbridge::detail::IoCaps{.stream_backpressure = true,
                                                  .stream_pause_above = 1000,
                                                  .stream_cap = 4000});
        uring.next_send = Sent::Queued;
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
        t.dispatch(c, u);
        streaming(c, u, llmbridge::net::http::Body::UntilClose);
        u->rbuf.assign(600, 'x');
        t.pump(u);
        EXPECT_FALSE(uring.called("pause upstream")) << "a send in flight is not a slow client";
        u->rbuf.assign(600, 'x');
        t.pump(u);
        EXPECT_TRUE(uring.called("pause upstream")) << "1200 bytes owed is past the threshold";
        EXPECT_EQ(t.stats().stream_pauses, 1u);
        u->read_paused = true; // what set_read records in a backend
        u->rbuf.assign(2000, 'x');
        t.pump(u);
        EXPECT_EQ(t.stats().stream_pauses, 1u);
        EXPECT_FALSE(c->doomed) << "3200 owed is under the cap";

        uring.next_send = Sent::Done; // the client drained
        t.flush(c);
        EXPECT_EQ(uring.calls.back(), "resume upstream");
        EXPECT_FALSE(c->doomed);

        u->read_paused = false;
        uring.next_send = Sent::Queued;
        u->rbuf.assign(1200, 'x');
        t.pump(u);
        EXPECT_EQ(t.stats().stream_pauses, 2u) << "the next slow stretch pauses again";
        u->read_paused = true;
        u->rbuf.assign(3000, 'x'); // read before the pause took effect
        t.pump(u);
        EXPECT_TRUE(c->doomed) << "past the cap the stream is dropped, paused or not";
    }

    // Where reads cannot be paused, the stream's buffer is capped instead.
    TEST_F(Core, WithoutAPauseASlowClientsStreamIsCappedNotPaused)
    {
        FakeIo capped{t};
        t.attach(capped, /*backpressure=*/false);
        capped.next_send = Sent::Queued;
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
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
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
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
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
        t.dispatch(c, u);
        streaming(c, u, llmbridge::net::http::Body::Length, 9);
        c->req.f.stream_chunked_out = true;
        u->rbuf = "data: x\n\nHTTP/1.1 200 OK\r\n";
        t.pump(u);
        EXPECT_TRUE(c->req.f.stream_ended);
        EXPECT_EQ(t.pooled(), 0u);
        EXPECT_TRUE(u->doomed);
    }

    // A response arriving over many reads has its head parsed once.
    TEST_F(Core, AResponseHeadIsParsedOnceHoweverManyReadsTheBodyTakes)
    {
        ClientConn* c = t.client();
        UpstreamConn* u = t.upstream();
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

    /// An OpenAI client's JSON body, declared whole, of which `part` bytes are in `c`:
    /// it streams, on an upstream still connecting.
    const std::string kBody = R"({"model":"m","messages":")" + std::string(20000, 'p') + R"("})";
    UpstreamConn* stream_part(GatewayCoreTest& t, ClientConn* c, size_t part)
    {
        c->rbuf = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
                  "Content-Length: " + std::to_string(kBody.size()) + "\r\n\r\n" + kBody.substr(0, part);
        t.bytes(c);
        return c->up;
    }

    // A streamed body holds a slot from its head to whatever ends it, and no longer: its
    // last byte, a response byte, a refusal, the client leaving, or an error reply.
    TEST_F(Core, EveryWayAStreamedBodyEndsFreesItsSlot)
    {
        ClientConn* whole = t.client();
        ASSERT_NE(stream_part(t, whole, 100), nullptr);
        ASSERT_TRUE(whole->req.f.body_streaming);
        EXPECT_EQ(t.bodies_streaming(), 1u);
        whole->rbuf.append(kBody.substr(100));
        t.bytes(whole);
        EXPECT_FALSE(whole->req.f.body_streaming);
        EXPECT_EQ(t.bodies_streaming(), 0u) << "its last byte";

        UpstreamConn* u = stream_part(t, t.client(), 100);
        ASSERT_NE(u, nullptr);
        u->rbuf = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
        t.upstream_bytes(u);
        EXPECT_EQ(t.bodies_streaming(), 0u) << "a response byte";

        ClientConn* refused = t.client();
        stream_part(t, refused, 100);
        refused->rbuf.append(R"(","model":"x")");
        t.bytes(refused);
        EXPECT_EQ(t.stats().streamed_body_refusals, 1u);
        EXPECT_EQ(t.bodies_streaming(), 0u) << "a refusal";

        ClientConn* gone = t.client();
        stream_part(t, gone, 100);
        t.gone(gone);
        EXPECT_EQ(t.bodies_streaming(), 0u) << "the client leaving";

        ClientConn* failed = t.client();
        stream_part(t, failed, 100);
        io.next_send = Sent::Queued; // the reply waits on the client, which stays open
        t.error_respond(failed, 502);
        EXPECT_FALSE(failed->doomed);
        EXPECT_FALSE(failed->req.f.body_streaming);
        EXPECT_EQ(t.bodies_streaming(), 0u) << "an error reply";
        EXPECT_EQ(t.stats().streamed_bodies, 5u);
    }

    // Past the cap the next body is held, framed only at its last byte; a freed slot streams again.
    TEST_F(Core, PastTheCapABodyIsHeldUntilItsLastByte)
    {
        gw->set_stream_body_max(1);
        ClientConn* first = t.client();
        ASSERT_NE(stream_part(t, first, 100), nullptr);
        ClientConn* held = t.client();
        EXPECT_EQ(stream_part(t, held, 100), nullptr);
        EXPECT_FALSE(held->req.f.body_streaming);
        EXPECT_EQ(held->req.f.phase, Phase::Idle) << "nothing framed before the last byte";
        EXPECT_EQ(t.stats().streamed_body_capped, 1u);
        held->rbuf.append(kBody.substr(100));
        t.bytes(held);
        ASSERT_NE(held->up, nullptr);
        EXPECT_FALSE(held->req.f.body_streamed);
        EXPECT_EQ(held->req.f.phase, Phase::Dispatched);

        t.gone(first);
        EXPECT_NE(stream_part(t, t.client(), 100), nullptr) << "the freed slot";
        EXPECT_EQ(t.stats().streamed_bodies, 2u);
        EXPECT_EQ(t.stats().streamed_body_capped, 1u);
    }

    // A cap of 0 is no cap: every body streams, never one held.
    TEST_F(Core, ACapOfZeroHoldsNoBody)
    {
        gw->set_stream_body_max(0);
        for (int i = 0; i < 3; ++i) EXPECT_NE(stream_part(t, t.client(), 100), nullptr) << i;
        EXPECT_EQ(t.bodies_streaming(), 3u);
        EXPECT_EQ(t.stats().streamed_bodies, 3u);
        EXPECT_EQ(t.stats().streamed_body_capped, 0u);
    }

    // The 100 Continue for a body that would stream finds the client gone: the close dooms
    // it, so nothing is dispatched, no venue is contacted and no slot is taken.
    TEST_F(Core, AClientThe100ContinueFoundGoneIsNeverDispatched)
    {
        ClientConn* c = t.client();
        c->rbuf = "POST /v1/chat/completions HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
                  "Expect: 100-continue\r\nContent-Length: " + std::to_string(kBody.size()) + "\r\n\r\n";
        io.next_send = Sent::Failed;
        t.bytes(c);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"send client", "close client"}));
        EXPECT_TRUE(c->doomed);
        EXPECT_EQ(c->up, nullptr);
        EXPECT_EQ(t.bodies_streaming(), 0u);
        EXPECT_EQ(t.stats().streamed_bodies, 0u);
    }

    // The idle sweep found neither leg moving mid-body. The venue had every byte that came,
    // so the client stopped sending: a client stall, a 408, the venue reset. The stalled
    // body rode a pooled connection inside the stale-retry window and is not resent.
    TEST_F(Core, ABodyTheClientStoppedSendingIsAClientStall)
    {
        gw->set_body_min_rate(0); // the idle sweep alone, past any upload deadline
        t.release(t.upstream(), /*keep_alive=*/true);
        ClientConn* c = t.client();
        UpstreamConn* u = stream_part(t, c, 100);
        ASSERT_NE(u, nullptr);
        ASSERT_TRUE(u->from_pool);
        ASSERT_TRUE(u->out.idle()) << "the venue has all that came";
        io.calls.clear();
        io.next_send = Sent::Queued; // the reply stays to be read
        t.sweep(c, c->req.f.ts_progress + Gateway::kDefaultUpstreamIdleNs + 1);
        EXPECT_EQ(io.calls, (std::vector<std::string>{"close upstream", "send client"}));
        EXPECT_EQ(c->out.bytes().rfind("HTTP/1.1 408 Request Timeout\r\n", 0), 0u) << c->out.bytes();
        EXPECT_TRUE(u->doomed);
        EXPECT_TRUE(u->reset_on_close) << "the venue is reset, not sent a FIN";
        EXPECT_EQ(t.bodies_streaming(), 0u);
        EXPECT_EQ(t.stats().client_idle_timeouts, 1u);
        EXPECT_EQ(t.stats().upstream_timeouts, 0u);
        EXPECT_EQ(t.stats().upstream_retries, 0u);
    }

    // The same sweep with request bytes the venue has not taken: it stopped reading, so it
    // is the venue's timeout, a 504, and the venue is reset all the same.
    TEST_F(Core, ABodyTheVenueStoppedTakingIsTheVenuesTimeout)
    {
        gw->set_body_min_rate(0); // the idle sweep alone, past any upload deadline
        ClientConn* c = t.client();
        UpstreamConn* u = stream_part(t, c, 100);
        ASSERT_NE(u, nullptr);
        u->connected = u->wire_ready = true; // connected, and the head never taken
        ASSERT_FALSE(u->out.idle());
        io.next_send = Sent::Queued;
        t.sweep(c, c->req.f.ts_progress + Gateway::kDefaultUpstreamIdleNs + 1);
        EXPECT_EQ(c->out.bytes().rfind("HTTP/1.1 504 Gateway Timeout\r\n", 0), 0u) << c->out.bytes();
        EXPECT_TRUE(u->doomed);
        EXPECT_TRUE(u->reset_on_close);
        EXPECT_EQ(t.stats().upstream_timeouts, 1u);
        EXPECT_EQ(t.stats().client_idle_timeouts, 0u);
    }

    // Staging a part is not the venue's progress; sending it is.
    TEST_F(Core, OnlyASendThatMovesBytesIsProgress)
    {
        ClientConn* c = t.client();
        UpstreamConn* u = stream_part(t, c, 100);
        ASSERT_NE(u, nullptr);
        const int64_t before = c->req.f.ts_progress;
        c->rbuf.append(kBody.substr(100, 1000));
        t.bytes(c); // still connecting: staged only
        EXPECT_EQ(c->req.f.ts_progress, before);
        u->connected = u->wire_ready = true;
        c->rbuf.append(kBody.substr(1100, 1000));
        t.bytes(c); // sent
        EXPECT_GT(c->req.f.ts_progress, before);
    }

    // A venue that stopped reading can have every byte we sent in our kernel, unacknowledged:
    // nothing is left for us to send, and still the venue is the one that stopped.
    TEST_F(Core, BytesTheKernelHoldsUnacknowledgedAreTheVenues)
    {
        gw->set_body_min_rate(0); // the idle sweep alone, past any upload deadline
        const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        int small = 4096;
        ::setsockopt(lfd, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof a;
        ASSERT_EQ(::bind(lfd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&a), &len);
        ::listen(lfd, 1);
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
        const int venue = ::accept(lfd, nullptr, nullptr); // and never read
        const std::string junk(1 << 16, 'j');
        while (::send(fd, junk.data(), junk.size(), MSG_DONTWAIT | MSG_NOSIGNAL) > 0) {}
        ASSERT_GT(llmbridge::net::unacked_bytes(fd), 0u);

        ClientConn* c = t.client();
        UpstreamConn* u = stream_part(t, c, 100);
        ASSERT_NE(u, nullptr);
        u->connected = u->wire_ready = true;
        u->out.sent(u->out.wire().size()); // ours is empty: all of it is in the kernel
        ::close(u->fd);
        u->fd = fd; // closed, with a reset, at teardown
        io.next_send = Sent::Queued;
        t.sweep(c, c->req.f.ts_progress + Gateway::kDefaultUpstreamIdleNs + 1);
        EXPECT_EQ(c->out.bytes().rfind("HTTP/1.1 504 Gateway Timeout\r\n", 0), 0u) << c->out.bytes();
        EXPECT_EQ(t.stats().upstream_timeouts, 1u);
        EXPECT_EQ(t.stats().client_idle_timeouts, 0u);
        ::close(venue);
        ::close(lfd);
    }

    // The defaults, a 10 s grace and 1 KiB a second, against a body as large as one can be:
    // uploaded at 2 KiB a second, slower than any real link, it is never cut. At 512 bytes a
    // second it is cut once its time runs out, 20 s in. Driven by the sweep alone, the venue
    // taking every byte as it comes.
    TEST_F(Core, TheDefaultsNeverCutASteadySlowBody)
    {
        for (const uint64_t rate : {2048u, 512u})
        {
            ClientConn* c = t.client();
            UpstreamConn* u = stream_part(t, c, 0);
            ASSERT_NE(u, nullptr);
            u->connected = u->wire_ready = true;
            const int64_t t0 = c->req.f.ts_req_recvd;
            int64_t cut_at = 0;
            for (int64_t s = 1; s <= 16 * 1024 * 1024 / 2048 && !cut_at; ++s)
            {
                c->req.f.body_in = static_cast<uint32_t>(rate * static_cast<uint64_t>(s));
                c->req.f.ts_progress = t0 + s * 1'000'000'000;
                t.sweep(c, t0 + s * 1'000'000'000 + 1); // just after its bytes for second s
                if (c->doomed || c->req.f.phase == Phase::Replying) cut_at = s;
            }
            if (rate == 2048) EXPECT_EQ(cut_at, 0) << "a steady 2 KiB a second was cut";
            else EXPECT_EQ(cut_at, 20) << "512 bytes a second runs out of time at 20 s";
        }
        EXPECT_EQ(t.stats().streamed_body_timeouts, 1u);
    }

    // The bytes that come after the deadline passed are never forwarded, even when they
    // are the body's last: the venue holds part of the body and a reset.
    TEST_F(Core, BytesAfterTheDeadlineAreNeverForwarded)
    {
        gw->set_body_timeout_ns(1'000'000'000);
        ClientConn* c = t.client();
        UpstreamConn* u = stream_part(t, c, 100);
        ASSERT_NE(u, nullptr);
        u->connected = u->wire_ready = true;
        const size_t staged = u->out.bytes().size();
        c->req.f.ts_req_recvd -= 2'000'000'000; // its head came 2 s ago
        io.next_send = Sent::Queued;
        c->rbuf.append(kBody.substr(100));
        t.bytes(c);
        EXPECT_EQ(u->out.bytes().size(), staged) << "the rest went up";
        EXPECT_TRUE(u->doomed);
        EXPECT_TRUE(u->reset_on_close);
        EXPECT_EQ(c->out.bytes().rfind("HTTP/1.1 408 Request Timeout\r\n", 0), 0u) << c->out.bytes();
        EXPECT_EQ(t.stats().streamed_body_timeouts, 1u);
        EXPECT_EQ(t.bodies_streaming(), 0u);
    }

    // The part of the body already in at its head earns its time like any other.
    TEST_F(Core, TheBodyInAtItsHeadEarnsItsTime)
    {
        ClientConn* c = t.client();
        UpstreamConn* u = stream_part(t, c, 10 * 1024);
        ASSERT_NE(u, nullptr);
        u->connected = u->wire_ready = true;
        const int64_t t0 = c->req.f.ts_req_recvd;
        c->req.f.ts_progress = t0 + 15'000'000'000;
        t.sweep(c, t0 + 15'000'000'000); // 10 s of grace and 10 s for its 10 KiB
        EXPECT_EQ(t.stats().streamed_body_timeouts, 0u);
        t.sweep(c, t0 + 20'000'000'001);
        EXPECT_EQ(t.stats().streamed_body_timeouts, 1u);
    }
} // namespace
