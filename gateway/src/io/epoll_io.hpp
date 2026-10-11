// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// The epoll backend: level-triggered readiness, optimistic writes, a read budget.

#include <cstddef>
#include <cstdint>

#include "core/io.hpp"

namespace llmbridge::detail
{
    class Engine;

    class EpollIo final : public IoOps
    {
      public:
        explicit EpollIo(Engine& e) noexcept;
        ~EpollIo();
        EpollIo(const EpollIo&) = delete;
        EpollIo& operator=(const EpollIo&) = delete;
        int run(); // until Engine::_stop

        Sent send(Connection& c) noexcept override;
        bool connect(Connection& u) noexcept override;
        void set_read(Connection& c, bool on) noexcept override;
        void close(Connection& c) noexcept override;

      private:
        void sync_interest(Connection* c) noexcept; // the one writer of the interest mask
        void arm_write(Connection* c) noexcept;
        void disarm_write(Connection* c) noexcept;
        void pause_accept() noexcept;
        void resume_accept() noexcept;
        void on_accept() noexcept;
        void on_client_readable(Connection* c) noexcept;
        void on_client_writable(Connection* c) noexcept;
        void on_upstream_writable(Connection* u) noexcept;
        void on_upstream_readable(Connection* u) noexcept;
        bool drain_read(Connection* c) noexcept;
        bool pump_write(Connection* c, bool* done) noexcept;
#ifdef LLMBRIDGE_HAVE_TLS
        bool tls_flush(Connection* u, bool* done) noexcept; // arms EPOLLOUT on a partial write
        bool tls_drain_read(Connection* u) noexcept;
#endif

        /// Most bytes one readable event pulls off a socket. A drain to EAGAIN once staged
        /// 33 MB for a client that never read; level-triggered epoll re-notifies the rest.
        static constexpr size_t kMaxReadPerEvent = 1 << 20;
        static constexpr int kMaxEvents = 1024;

        Engine& _e;
        int _epfd = -1;
    };
} // namespace llmbridge::detail
