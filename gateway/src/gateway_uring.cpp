// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The io_uring half of Gateway: every ur_ method and run_uring(). Its twin is
// gateway_epoll.cpp; the shared methods are in gateway.cpp. Empty without
// LLMBRIDGE_HAVE_URING, the same guard net/uring.hpp carries.

#include "gateway/gateway.hpp"

#include "core/io.hpp"
#include "core/limits.hpp"
#include "core/registry.hpp"
#include "net/secure.hpp"
#include "net/socket_util.hpp"
#include "request.hpp"
#include "response.hpp"
#include "scan.hpp"
#include "stream.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string_view>
#include "net/uring.hpp"

#ifdef LLMBRIDGE_HAVE_URING
namespace llmbridge
{
    using namespace detail;

    // ════════════════════════════════════════════════════════════════════════
    // io_uring backend (Phase 1): a completion-driven mirror of the epoll loop.
    // Each request advances a small per-connection state machine: every CQE says
    // "this op finished with N bytes," we act, and submit the next op. A conn is
    // freed only when its `inflight` SQEs have all completed (no use-after-free on
    // a completion that lands after we close).
    // ════════════════════════════════════════════════════════════════════════
    namespace
    {
        enum UOp : uint64_t { UAccept = 0, URecv = 1, USend = 2, UConnect = 3, UTimer = 4, UCancel = 5 };
        constexpr uint64_t kUrTagMask = 7;
        inline uint64_t make_ud(Connection* c, UOp op) { return reinterpret_cast<uintptr_t>(c) | op; }
        inline Connection* ud_conn(uint64_t d) { return reinterpret_cast<Connection*>(d & ~kUrTagMask); }
        inline UOp ud_op(uint64_t d) { return static_cast<UOp>(d & kUrTagMask); }

        constexpr unsigned kUrRingDepth = 4096;
        // Provided-buffer pool for multishot recv: plenty so a connection always has
        // a buffer to land in (we recycle each immediately after copying it out).
        constexpr unsigned kUrBufGroup = 1;
        constexpr unsigned kUrBufCount = 4096; // power of two
        constexpr unsigned kUrBufSize = 4096;

    } // namespace

    bool Gateway::ur_next_sqe(io_uring_sqe** out) noexcept
    {
        io_uring_sqe* s = _ring.get_sqe();
        if (!s) { _ring.submit(); s = _ring.get_sqe(); } // SQ full: flush, retry once
        *out = s;
        return s != nullptr;
    }

    bool Gateway::ur_submit_accept() noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return false;
        s->opcode = IORING_OP_ACCEPT;
        s->fd = _listen_fd;
        s->accept_flags = SOCK_NONBLOCK | SOCK_CLOEXEC;
        s->ioprio = IORING_ACCEPT_MULTISHOT; // one SQE, a completion per accepted fd
        s->user_data = make_ud(_listen_conn, UAccept);
        return true;
    }

    // The timer tick re-arms the accept. Without it, an EMFILE ends the multishot and an
    // immediate re-arm fails again at once, and a failed re-arm left the listener dead.
    void Gateway::ur_pause_accept(const char* why) noexcept
    {
        _accept_resume_ns = now_ns() + kAcceptBackoffNs;
        ++_stats.accept_backoffs;
        LB_WARN("CAP ", why, "; accepting paused for 100 ms, total=", _stats.accept_backoffs);
    }

    bool Gateway::ur_submit_timer() noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return false;
        s->opcode = IORING_OP_TIMEOUT;
        s->addr = reinterpret_cast<uint64_t>(&_uring_ts);
        s->len = 1;
        s->user_data = UTimer; // conn = nullptr
        return true;
    }

    // The SQ is full even after a flush, so this connection cannot make progress: end it
    // and its peer together. Closing only `c` left the peer pointing at a freed object.
    void Gateway::ur_drop_sq_full(Connection* c) noexcept
    {
        LB_WARN("CAP io_uring submission queue full; closing ", *c);
        Connection* client = c->is_client ? c : c->peer;
        if (client) abort_pair(client);
        else ur_close(c);
    }

    bool Gateway::ur_arm_recv(Connection* c) noexcept
    {
        // Multishot recv drawing from the provided-buffer pool: one submission keeps
        // delivering a completion per data arrival (each naming the buffer it used),
        // so we never re-submit a recv per read. Armed once per connection; only
        // re-armed if the kernel ends the multishot (e.g. pool exhaustion).
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) { ur_drop_sq_full(c); return false; }
        s->opcode = IORING_OP_RECV;
        s->fd = c->fd;
        s->addr = 0;
        s->len = 0;
        s->flags |= IOSQE_BUFFER_SELECT;
        s->buf_group = kUrBufGroup;
        s->ioprio |= IORING_RECV_MULTISHOT;
        s->user_data = make_ud(c, URecv);
        ++c->inflight;
        ++_uring_inflight;
        c->recv_armed = true;
        return true;
    }

    bool Gateway::ur_cancel_recv(Connection* c) noexcept
    {
        // Its last completion, -ECANCELED, re-arms if reads resumed meanwhile. Bytes the
        // kernel already placed still arrive: bounded by the shared buffer ring.
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return false;
        s->opcode = IORING_OP_ASYNC_CANCEL;
        s->addr = make_ud(c, URecv);
        s->user_data = UCancel;
        return true;
    }

    bool Gateway::ur_submit_send(Connection* c) noexcept
    {
        io_uring_sqe* s = nullptr;
#ifdef LLMBRIDGE_HAVE_TLS
        if (!tls_invariant_ok(c)) { ur_drop_sq_full(c); return false; } // never plaintext
#endif
        if (!ur_next_sqe(&s)) { ur_drop_sq_full(c); return false; }
        s->opcode = IORING_OP_SEND;
        s->fd = c->fd;
        // TLS on either leg: the wire sees ciphertext, and `out` is fed to the Session.
        OutBuf& w = c->wire();
        s->addr = reinterpret_cast<uint64_t>(w.wire().data());
        s->len = static_cast<unsigned>(w.wire().size());
        s->user_data = make_ud(c, USend);
        // The only pin: the kernel reads w's front until ur_on_send, so new bytes stage
        // behind it. Nothing else may pin; GATEWAY-INTERNALS.md 5b.
        w.pin();
        ++c->inflight;
        ++_uring_inflight;
        return true;
    }

    bool Gateway::ur_submit_connect(Connection* u) noexcept
    {
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return false; // the opener closes the fd
        s->opcode = IORING_OP_CONNECT;
        s->fd = u->fd;
        s->addr = reinterpret_cast<uint64_t>(&u->up_addr);
        s->off = sizeof(u->up_addr); // connect addrlen rides in `off`
        s->user_data = make_ud(u, UConnect);
        ++u->inflight;
        ++_uring_inflight;
        return true;
    }

    void Gateway::ur_submit_cancel(int fd) noexcept
    {
        // Cancel every in-flight op on `fd` (notably the armed multishot recv, which
        // shutdown() does not terminate). The cancelled ops complete with -ECANCELED,
        // releasing their inflight slots so the conn can be freed / the drain finishes.
        io_uring_sqe* s = nullptr;
        if (!ur_next_sqe(&s)) return;
        s->opcode = IORING_OP_ASYNC_CANCEL;
        s->fd = fd;
        s->cancel_flags = IORING_ASYNC_CANCEL_FD | IORING_ASYNC_CANCEL_ALL; // a recv and a send
        s->user_data = UCancel; // sentinel: not inflight-counted, completion ignored
    }

#ifdef LLMBRIDGE_HAVE_TLS

    void Gateway::ur_tls_flush(Connection* u) noexcept
    {
        // One send at a time: the completion calls back here for whatever staged
        // behind the pinned front meanwhile.
        if (u->tls_out.pinned()) return;
        tls_pump_out(u);
        if (u->tls_out.idle()) return;
        ur_submit_send(u);
    }

#endif // LLMBRIDGE_HAVE_TLS

    // Send `out` to a client connection, whatever the transport.
    //
    // The plaintext path submits a send straight out of `out`. A TLS conn cannot:
    // the SQE points at tls_out, so the plaintext has to go through the Session
    // first or the kernel is handed a zero-length send. That is exactly the bug
    // this helper exists to make unrepeatable. It was found by running curl
    // against both backends, and never by reading the code.
    void Gateway::ur_client_send(Connection* c) noexcept
    {
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->tls)
        {
            if (c->tls->handshake_done()) tls_push_out(c);
            ur_tls_flush(c);
            return;
        }
#endif
        ur_submit_send(c);
    }

    void Gateway::ur_close(Connection* c) noexcept
    {
        if (c->doomed) return;
        // Every close unpairs: the other leg never keeps a pointer to a freed object.
        Connection* other = unpair(c);
        if (c->is_client)
        {
            _clients->remove(c);
            LB_DEBUG("close ", *c, " clients=", _clients->size());
        }
        else
        {
            // This backend closes both kinds in one function where epoll has two, so
            // the upstream branch lives here. After the erase, as in the ep_ twin.
            _pool->remove(*c);
            _upconns->remove(c);
            LB_DEBUG("upstream close ", *c, " pool=", pooled_upstream_count());
        }
        // Force any in-flight op on this fd to complete so its inflight count drains
        // (the fd is closed at free time). shutdown alone does not end an armed
        // multishot recv, so also cancel everything on the fd.
        if (c->fd >= 0) { ::shutdown(c->fd, SHUT_RDWR); ur_submit_cancel(c->fd); }
        c->doomed = true;
        _doomed->add(c);
        // An upstream mid-response is of no use to anyone else.
        if (other && c->is_client) ur_close(other);
        ur_maybe_free(c);
    }

    void Gateway::ur_maybe_free(Connection* c) noexcept
    {
        if (!c->doomed || c->inflight > 0) return; // a completion still references it
        if (c->fd >= 0) { ::close(c->fd); c->fd = -1; }
        _doomed->remove(c);
        retire_wbuf(c);
        delete c;
    }

    void Gateway::ur_on_cqe(uint64_t user_data, int res, uint32_t flags) noexcept
    {
        const UOp op = ud_op(user_data);
        if (op == UTimer)
        {
            if (!_draining && !_stop)
            {
                sweep_idle();
                if (_accept_resume_ns && now_ns() >= _accept_resume_ns)
                {
                    _accept_resume_ns = 0;
                    if (!ur_submit_accept()) ur_pause_accept("accept re-arm found the SQ full");
                }
                if (!ur_submit_timer()) _uring_timer_lost = true;
            }
            return;
        }
        if (op == UCancel) return; // control op (cancel-by-fd); not inflight-counted
        if (op == UAccept) { ur_on_accept(res, flags); return; } // not inflight-counted

        Connection* c = ud_conn(user_data);
        // A multishot recv stays armed only while it keeps delivering data (F_MORE
        // set and res > 0). Its terminal completion (EOF/error, res <= 0) can still
        // carry F_MORE, so it must release the inflight slot, or the drain
        // never reaches zero. Only the consuming (terminal) completion decrements.
        const bool armed = (op == URecv) && (flags & IORING_CQE_F_MORE) && res > 0;
        if (!armed) { --c->inflight; --_uring_inflight; }
        if (op == URecv && !armed) c->recv_armed = false;

        if (_draining || c->doomed)
        {
            if (op == USend) c->wire().sent(0);
            if (op == URecv && (flags & IORING_CQE_F_BUFFER))
                _bufring.recycle(flags >> IORING_CQE_BUFFER_SHIFT); // return the provided buffer
            if (!armed) ur_maybe_free(c);
            return;
        }
        switch (op)
        {
            case URecv: ur_on_recv(c, res, flags); break;
            case USend: ur_on_send(c, res); break;
            case UConnect: ur_on_connect(c, res); break;
            default: break;
        }
    }

    void Gateway::ur_on_accept(int res, uint32_t flags) noexcept
    {
        if (_stop || _draining)
        {
            if (res >= 0) ::close(res); // shutting down: don't take new work
            return;
        }
        if (!(flags & IORING_CQE_F_MORE)) // multishot ended -> re-arm, unless that would spin
        {
            if (res == -EMFILE || res == -ENFILE) ur_pause_accept("out of file descriptors");
            else if (!ur_submit_accept()) ur_pause_accept("accept re-arm found the SQ full");
        }
        if (res < 0) return;                                 // transient accept error
        const int fd = res;
        net::set_nodelay(fd);
        if (_client_sndbuf > 0)
            ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &_client_sndbuf, sizeof(_client_sndbuf));
        Connection* c = new Connection();
        c->fd = fd;
        c->is_client = true;
        c->id = _next_client_id++;
        c->ts_accepted = now_ns();
        c->ts_client_activity = c->ts_accepted;
        c->rbuf.reserve(kInitialBuf);
#ifdef LLMBRIDGE_HAVE_TLS
        if (_tls.client_tls && !tls_attach_client(c))
        {
            // Fail closed, exactly as the epoll twin does. No recv is armed yet, so
            // there is nothing in flight and the Connection can be freed directly
            // instead of going on the doomed list.
            ::close(fd);
            delete c;
            ++_stats.errors;
            return;
        }
#endif
        _clients->add(c);
        LB_DEBUG("accept ", *c, " clients=", _clients->size());
        ur_arm_recv(c); // multishot recv stays armed for the connection's life
    }

    void Gateway::ur_on_recv(Connection* c, int res, uint32_t flags) noexcept
    {
        const bool armed = flags & IORING_CQE_F_MORE;

        // -ENOBUFS is not a connection error: the provided-buffer pool was momentarily
        // empty, so the kernel declined to pick a buffer and ended the multishot without
        // consuming one. The cure is to re-arm, not to tear down. Without this branch the
        // fallthrough below would kill the client pair or, mid-stream, report a premature
        // stream end.
        //
        // Measured scope, so nobody mistakes this for a fix to a live bug: on this kernel
        // (7.0) with kUrBufCount=4096, `uring_enobufs` stayed at **0** even at 8192
        // concurrent streams (16384 armed recvs, 4x the pool). The kernel ends the
        // multishot with res>0 and F_MORE clear under pool pressure, which the re-arm at
        // the bottom of this function already handles. So this is defensive hardening for
        // a path that is reachable by contract but was never observed in practice; it is
        // Not the explanation for io_uring's high-concurrency tail latency (that was
        // hypothesised from the kUrBufCount correlation and disproved by this counter).
        // The regression test forces the condition by shrinking the pool.
        if (res == -ENOBUFS)
        {
            ++_stats.uring_enobufs;
            LB_WARN("CAP io_uring provided buffers exhausted; recv re-armed"
                    " total=", _stats.uring_enobufs);
            if (!c->read_paused) ur_arm_recv(c);
            return;
        }
        // A pause's cancel, or a resume that raced it.
        if (res == -ECANCELED)
        {
            if (!c->read_paused) ur_arm_recv(c);
            return;
        }

        if (res <= 0) // multishot ended: EOF (0) or error (<0)
        {
            if (flags & IORING_CQE_F_BUFFER) _bufring.recycle(flags >> IORING_CQE_BUFFER_SHIFT);
            if (c->is_client) client_gone(c);
            else on_upstream_eof(c, false);
            return;
        }

        // Copy the bytes out of the kernel-selected buffer, then return it to the
        // pool. For a TLS upstream the bytes are ciphertext: they go through the
        // Session (tls_feed), whose plaintext lands in rbuf, so the parse logic
        // below sees only plaintext either way.
        bool tls_ok = true;
        if (flags & IORING_CQE_F_BUFFER)
        {
            const unsigned bid = flags >> IORING_CQE_BUFFER_SHIFT;
#ifdef LLMBRIDGE_HAVE_TLS
            if (c->tls)
                tls_ok = tls_feed(c, _bufring.data(bid), static_cast<size_t>(res));
            else
#endif
            {
                if (c->ts_first_byte == 0) c->ts_first_byte = now_ns();
                c->rbuf.append(_bufring.data(bid), static_cast<size_t>(res));
            }
            _bufring.recycle(bid);
            if (!c->is_client) note_response_bytes(c);
        }
        // Kernel ended the multishot (pool pressure): re-arm. A failed arm may free c.
        if (!armed && !c->read_paused && !ur_arm_recv(c)) return;
#ifdef LLMBRIDGE_HAVE_TLS
        if (!tls_ok)
        {
            // Fatal (bad record, MAC failure, refused certificate). A client's is its
            // connection dying, never a retryable upstream fault.
            if (c->is_client) return client_gone(c);
            return on_upstream_eof(c, true);
        }
        if (c->tls)
            ur_tls_flush(c); // handshake replies / newly-pushed request bytes
#else
        (void)tls_ok;
#endif

        if (c->is_client) on_client_bytes(c);
        else on_upstream_bytes(c);
    }

    void Gateway::ur_on_connect(Connection* u, int res) noexcept
    {
        if (res < 0)
        {
            note_connect_failure(u->upstream_slot, std::strerror(-res));
            Connection* cl = unpair(u);
            ur_close(u);
            if (cl) fail_request(cl, 502, "upstream connect refused");
            else ++_stats.errors;
            return;
        }
        u->connected = true;
        // t2 for a plaintext upstream only; see the epoll twin and tls_feed().
        if (!upstream_is_tls(u))
        {
            u->wire_ready = true;
            if (u->peer) u->peer->req.f.ts_wire_ready = now_ns();
        }
        if (!ur_arm_recv(u)) return; // arm the multishot recv for this upstream's life; may free u
#ifdef LLMBRIDGE_HAVE_TLS
        if (u->tls)
        {
            // The request waits in `out` (plaintext) until the handshake completes;
            // what goes out now is the ClientHello.
            u->tls->start_handshake();
            ur_tls_flush(u);
            return;
        }
#endif
        ur_submit_send(u); // then send the request
    }

    void Gateway::ur_on_send(Connection* c, int res) noexcept
    {
        // The SQE is done with the buffer either way: unpin, and fold in what staged.
        c->wire().sent(res > 0 ? static_cast<size_t>(res) : 0);
        if (res <= 0)
        {
            if (c->is_client) client_gone(c);
            else upstream_lost(c, UpFail::Write);
            return;
        }
        c->sent_bytes += static_cast<uint64_t>(res);
        // Bytes either peer took are progress: a client draining a stream the provider
        // has finished is not an idle request. A finished reply needs no stamp.
        if (c->is_client ? c->req.f.streaming : c->peer != nullptr)
            (c->is_client ? c : c->peer)->req.f.ts_progress = now_ns();
#ifdef LLMBRIDGE_HAVE_TLS
        if (c->tls)
        {
            // TLS on either leg: this send moved ciphertext; `out` tracks plaintext
            // fed to the Session and is not touched here.
            if (!c->tls_out.idle()) { ur_submit_send(c); return; } // partial, or staged behind
            // The Session may hold more: later handshake flights, or plaintext that
            // could not be pushed before the handshake finished.
            if (c->tls->handshake_done() && !c->out.idle()) tls_push_out(c);
            ur_tls_flush(c); // refill from the write BIO; resubmit if non-empty
            if (c->send_inflight()) return; // more ciphertext went out; wait for it

            if (!c->is_client)
            {
                if (c->peer && tls_out_flushed(c)) c->peer->req.f.ts_up_sent = now_ns();
                return;
            }
            // Inbound leg: the response is fully encrypted and fully on the wire,
            // the same completion point the plaintext path reaches below.
            if (!tls_out_flushed(c)) return;
            if (c->req.f.streaming) stream_flush(c);
            else finish(c);
            return;
        }
#endif
        if (!c->out.idle()) { ur_submit_send(c); return; } // partial, or staged behind: re-arms

        if (!c->is_client)
        {
            // Request fully sent. The response arrives via the already-armed multishot
            // recv. `out` keeps it, for a resend on a stale-connection failure.
            if (c->peer) c->peer->req.f.ts_up_sent = now_ns();
        }
        else if (c->req.f.streaming) stream_flush(c); // finalize if the stream ended
        else finish(c);
    }

    // The seam the shared lifecycle drives: today's io_uring primitives, one call each.
    class Gateway::UringIo final : public IoOps
    {
      public:
        explicit UringIo(Gateway& g) noexcept : _g(g) {}
        Sent send(Connection& c) noexcept override
        {
            if (c.send_inflight()) return Sent::Queued; // its completion sends the rest
#ifdef LLMBRIDGE_HAVE_TLS
            if (c.tls ? _g.tls_out_flushed(&c) : c.out.idle()) return Sent::Done;
#else
            if (c.out.idle()) return Sent::Done;
#endif
            _g.ur_client_send(&c); // on SQ exhaustion it has already torn `c` down
            return Sent::Queued;
        }
        bool connect(Connection& u) noexcept override
        {
            const int fd = net::make_client_socket();
            if (fd < 0) return false;
            u.fd = fd;
            if (_g.ur_submit_connect(&u)) return true;
            ::close(fd);
            u.fd = -1;
            return false;
        }
        // Cancels and re-arms the multishot recv. Streams do not use it: kStreamBufCap
        // bounds them (IoCaps).
        void set_read(Connection& c, bool on) noexcept override
        {
            if (c.read_paused == !on) return;
            if (on)
            {
                c.read_paused = false;
                if (!c.recv_armed) _g.ur_arm_recv(&c); // else its cancel's completion does
            }
            else if (!c.recv_armed || _g.ur_cancel_recv(&c))
                c.read_paused = true;
        }
        void close(Connection& c) noexcept override { _g.ur_close(&c); }

      private:
        Gateway& _g;
    };

    int Gateway::run_uring()
    {
        _t_start = now_ns();
        _active_backend = 2;

        unsigned flags = 0;
#if defined(IORING_SETUP_SINGLE_ISSUER) && defined(IORING_SETUP_DEFER_TASKRUN)
        flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
#endif
        if (!_ring.init(kUrRingDepth, flags) && !_ring.init(kUrRingDepth, 0))
        {
            LB_WARN("io_uring init failed; falling back to epoll");
            return run_epoll();
        }
        const unsigned bufcount = _uring_buf_count ? _uring_buf_count : kUrBufCount;
        if (!_bufring.init(_ring, kUrBufGroup, bufcount, kUrBufSize))
        {
            LB_WARN("io_uring provided-buffer ring unavailable (", _bufring.init_stage(),
                    " errno=", _bufring.init_errno(), " ",
                    std::strerror(_bufring.init_errno()), "); falling back to epoll");
            return run_epoll();
        }
        _uring_ts.tv_sec = kPollTickMs / 1000;
        _uring_ts.tv_nsec = static_cast<long long>(kPollTickMs % 1000) * 1000000LL;

        UringIo io(*this);
        attach(&io, IoCaps{.stream_backpressure = false});
        if (!ur_submit_accept()) ur_pause_accept("accept arm found the SQ full");
        if (!ur_submit_timer()) _uring_timer_lost = true;

        auto reap = [this] {
            _ring.for_each_cqe([this](const io_uring_cqe* cqe) {
                ur_on_cqe(cqe->user_data, cqe->res, cqe->flags);
            });
        };

        while (!_stop)
        {
            const int r = _ring.submit_and_wait(1);
            // EBUSY (CQ overflow backlog) and EAGAIN are transient: reaping clears them.
            if (r < 0 && r != -EINTR && r != -ETIME && r != -EBUSY && r != -EAGAIN)
            {
                LB_ERROR("io_uring_enter failed (", -r, "); this worker stops accepting work");
                break;
            }
            reap();
            // A lost timer disables every timeout and the accept backoff: re-arm it now.
            if (_uring_timer_lost && ur_submit_timer()) _uring_timer_lost = false;
        }

        // Graceful drain: stop taking new work, force every live fd's in-flight ops
        // to complete, and reap until nothing is outstanding, so no kernel op
        // writes into a buffer we're about to free.
        _draining = true;
        auto stop_conn = [this](Connection* c) {
            if (c->fd >= 0) { ::shutdown(c->fd, SHUT_RDWR); ur_submit_cancel(c->fd); }
        };
        for (Connection* c : *_clients) stop_conn(c);
        for (Connection* u : *_upconns) stop_conn(u); // pooled and in flight alike
        // doomed conns were already shut down + cancelled by ur_close().

        while (_uring_inflight > 0)
        {
            const int r = _ring.submit_and_wait(1);
            if (r < 0 && r != -EINTR && r != -ETIME) break;
            reap();
        }
        attach(nullptr, IoCaps{});
        return 0; // ~Gateway frees what the registries still hold

    }
} // namespace llmbridge
#endif // LLMBRIDGE_HAVE_URING
