// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// The seam in front of the running backend: a hedge leg's connection has no socket, so it
// stops here (GATEWAY-INTERNALS.md 13), and everything else passes through untouched.

#include "engine.hpp"

#include "core/conn.hpp"

namespace llmbridge::detail
{
    namespace
    {
        bool is_shadow(const ConnBase& c) noexcept
        {
            return c.kind == ConnKind::Client && static_cast<const ClientConn&>(c).shadow;
        }
    } // namespace

    // A leg's reply goes nowhere: written, it only feeds the leg's record.
    Sent ShadowIo::send(ConnBase& c) noexcept
    {
        if (!is_shadow(c)) return inner->send(c);
        c.out.sent(c.out.wire().size());
        return Sent::Done;
    }

    int ShadowIo::connect(UpstreamConn& u) noexcept
    {
        return inner->connect(u);
    }

    void ShadowIo::set_read(ConnBase& c, bool on) noexcept
    {
        if (!is_shadow(c)) inner->set_read(c, on);
    }

    void ShadowIo::close(ConnBase& c) noexcept
    {
        if (is_shadow(c)) return _e.close_shadow(&static_cast<ClientConn&>(c));
        inner->close(c);
    }
} // namespace llmbridge::detail
