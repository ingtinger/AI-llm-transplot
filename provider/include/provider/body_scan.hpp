// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// A JSON request body checked as its bytes arrive, so a gateway can forward each
// chunk only once it is known to be good.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace llmbridge::provider
{
    /// RFC 8259 syntax, nesting to json::detail::kMaxDepth, and the top-level keys and
    /// `stream_options`' keys as top_level_facts checks them: none repeated, none
    /// escaped, at most 256. A refusal is final until reset().
    ///
    /// Reset with `all_keys` false, for another dialect's body, only the top-level `model`
    /// may not repeat (none may be escaped, still). With `track_model`, the top-level
    /// `model` value is held back from safe() until it closes, then its span is told, so a
    /// gateway can forward something else in its place (GATEWAY-INTERNALS.md 12d).
    class BodyScan
    {
      public:
        enum class Verdict : uint8_t { More, Done, Refused };

        void reset() noexcept { reset(true, false); }
        void reset(bool all_keys, bool track_model) noexcept;
        /// Scans `body` from where the last call stopped; `body` holds every byte so far,
        /// which keys are read back from. `last`: nothing follows, so it must be whole.
        Verdict feed(std::string_view body, bool last) noexcept;
        /// The prefix of `body` that may be forwarded: everything scanned, less a key
        /// not yet closed, so a repeated key never leaves.
        [[nodiscard]] size_t safe() const noexcept { return _hold < _pos ? _hold : _pos; }
        /// Why it refused, for a log line: a literal.
        [[nodiscard]] const char* why() const noexcept { return _why; }
        /// Every top-level key so far, as offsets into the body.
        [[nodiscard]] size_t keys() const noexcept { return _top.size(); }
        /// Tracking: the top-level `model` value, raw, as [model_at, model_end) of the body,
        /// once it has closed; model_end() is SIZE_MAX before, and when there is none.
        [[nodiscard]] size_t model_at() const noexcept { return _model_at; }
        [[nodiscard]] size_t model_end() const noexcept { return _model_end; }

      private:
        enum class St : uint8_t;
        struct Span
        {
            uint32_t at, len;
        };
        bool step(std::string_view body, size_t& i) noexcept;
        bool value_done(size_t end) noexcept;
        bool key_done(std::string_view body, size_t end) noexcept;
        Verdict refuse(const char* why) noexcept;

        std::vector<Span> _top, _options; // keys of the root object and of stream_options
        uint64_t _kinds = 0;              // bit d: the container at depth d+1 is an object
        size_t _pos = 0, _hold = SIZE_MAX, _str = 0;
        size_t _model_at = SIZE_MAX, _model_end = SIZE_MAX;
        uint32_t _depth = 0, _options_depth = 0;
        St _st{};
        uint8_t _lit = 0, _hex = 0;
        bool _key = false, _tracked = false, _options_next = false;
        bool _all_keys = true, _track_model = false, _model_next = false, _model_open = false, _model_seen = false;
        const char* _why = nullptr;
        Verdict _verdict = Verdict::More;
    };
} // namespace llmbridge::provider
