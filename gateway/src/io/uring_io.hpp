// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// The io_uring backend: completion-driven, multishot accept and recv into a shared
// provided-buffer ring, one send in flight per connection. Empty without
// LLMBRIDGE_HAVE_URING, the guard net/uring.hpp carries.

#include "net/uring.hpp"

#ifdef LLMBRIDGE_HAVE_URING

#include <cstdint>

#include "core/io.hpp"

namespace llmbridge::detail
{
    class Engine;

    class UringIo final : public IoOps
    {
      public:
        [[nodiscard]] static bool available() noexcept { return net::uring::available(); }
        explicit UringIo(Engine& e) noexcept; // sets up the rings; ready() says if it could
        [[nodiscard]] bool ready() const noexcept { return _ready; }
        int run(); // until Engine::_stop, then drains every operation in flight

        Sent send(ConnBase& c) noexcept override;
        bool connect(UpstreamConn& u) noexcept override;
        /// Cancels and re-arms the multishot recv. Streams do not use it: kStreamBufCap
        /// bounds them (IoCaps).
        void set_read(ConnBase& c, bool on) noexcept override;
        void close(ConnBase& c) noexcept override;

      private:
        bool next_sqe(struct io_uring_sqe** out) noexcept; // flushes once when the SQ is full
        [[nodiscard]] bool submit_accept() noexcept;
        void pause_accept(const char* why) noexcept;
        [[nodiscard]] bool submit_timer() noexcept;
        void drop_sq_full(ConnBase* c) noexcept;
        bool arm_recv(ConnBase* c) noexcept;
        [[nodiscard]] bool cancel_recv(ConnBase* c) noexcept;
        bool submit_send(ConnBase* c) noexcept;
        void client_send(ConnBase* c) noexcept; // a TLS conn goes through its Session first
        void submit_cancel(int fd) noexcept;    // every op on the fd
#ifdef LLMBRIDGE_HAVE_TLS
        void tls_flush(ConnBase* u) noexcept; // one send in flight, so flights never interleave
#endif
        void maybe_free(ConnBase* c) noexcept; // once no completion can reach it
        void on_cqe(uint64_t user_data, int res, uint32_t flags) noexcept;
        void on_accept(int res, uint32_t flags) noexcept;
        template <class C> void on_recv(C* c, int res, uint32_t flags) noexcept;
        template <class C> void on_send(C* c, int res) noexcept;
        void on_connect(UpstreamConn* u, int res) noexcept;
        // What each kind does once on_recv and on_send have handled the transport.
        void lost(ClientConn* c, bool tls_failed) noexcept;
        void lost(UpstreamConn* u, bool tls_failed) noexcept;
        void arrived(ClientConn* c) noexcept;
        void arrived(UpstreamConn* u) noexcept;
        void send_failed(ClientConn* c) noexcept;
        void send_failed(UpstreamConn* u) noexcept;
        void progressed(ClientConn* c) noexcept;
        void progressed(UpstreamConn* u) noexcept;
        void drained(ClientConn* c) noexcept;
        void drained(UpstreamConn* u) noexcept;

        Engine& _e;
        net::uring::Ring _ring;
        net::uring::BufRing _bufring;
        struct __kernel_timespec _ts{};
        bool _ready = false;
        bool _timer_lost = false; ///< a re-arm found the SQ full; the loop retries it
        bool _draining = false;   ///< after stop: completions only release
        long _inflight = 0;       ///< SQEs in flight, the drain barrier
    };
} // namespace llmbridge::detail

#endif // LLMBRIDGE_HAVE_URING
