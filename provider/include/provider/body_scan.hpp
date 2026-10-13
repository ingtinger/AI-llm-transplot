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
    ///
    /// With `members`, a mask of kServiceTier, kSpeed and kInferenceGeo, the root must be an
    /// object, those top-level members may not repeat and their values must be strings, and
    /// each top-level member is held back from safe() with the separator before it (the
    /// byte after the root's `{`, else its `,`) until its key closes, and a member the mask
    /// names until its value closes too, so a gateway can insert its own after the `{` and
    /// blank the client's whole (GATEWAY-INTERNALS.md 12e).
    class BodyScan
    {
      public:
        enum class Verdict : uint8_t { More, Done, Refused };
        enum : uint8_t { kServiceTier = 1, kSpeed = 2, kInferenceGeo = 4 };
        /// A member the mask names, once closed: [at, end) runs from the separator before
        /// it (from the byte after the root's `{`, for the first member) through its value,
        /// a string whose opening quote is at `value_at`.
        struct Member
        {
            size_t at, value_at, end;
            uint8_t bit;
        };

        void reset() noexcept { reset(true, false); }
        void reset(bool all_keys, bool track_model, uint8_t members = 0) noexcept;
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
        /// With `members`: the byte after the root's `{` (SIZE_MAX before it), whether the
        /// root has a member, and the members the mask names that have closed, in body order.
        [[nodiscard]] size_t open_at() const noexcept { return _open_at; }
        [[nodiscard]] bool has_member() const noexcept { return _has_member; }
        [[nodiscard]] const Member* members() const noexcept { return _found; }
        [[nodiscard]] size_t member_count() const noexcept { return _nfound; }

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
        size_t _open_at = SIZE_MAX, _member_at = 0, _member_value = 0;
        Member _found[3]{};
        uint8_t _nfound = 0, _members = 0, _members_seen = 0, _member_bit = 0;
        bool _has_member = false, _member_next = false, _member_open = false;
        uint32_t _depth = 0, _options_depth = 0;
        St _st{};
        uint8_t _lit = 0, _hex = 0;
        bool _key = false, _tracked = false, _options_next = false;
        bool _all_keys = true, _track_model = false, _model_next = false, _model_open = false, _model_seen = false;
        const char* _why = nullptr;
        Verdict _verdict = Verdict::More;
    };
} // namespace llmbridge::provider
