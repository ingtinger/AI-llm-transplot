// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

// Response heads the gateway writes itself: one status table, and the upstream
// headers a relayed response keeps.

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

#include "net/http.hpp"

namespace llmbridge::net::http
{
    /// The reason phrase for `status`; a code the table does not list gets its class's.
    constexpr std::string_view reason(int status) noexcept
    {
        switch (status)
        {
            case 200: return "OK";
            case 201: return "Created";
            case 202: return "Accepted";
            case 204: return "No Content";
            case 206: return "Partial Content";
            case 304: return "Not Modified";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 402: return "Payment Required";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 408: return "Request Timeout";
            case 409: return "Conflict";
            case 413: return "Content Too Large";
            case 415: return "Unsupported Media Type";
            case 422: return "Unprocessable Content";
            case 429: return "Too Many Requests";
            case 451: return "Unavailable For Legal Reasons";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            case 504: return "Gateway Timeout";
            case 529: return "Overloaded";
            default:
                if (status < 200) return "Informational";
                if (status < 300) return "OK";
                if (status < 400) return "Redirection";
                return status < 500 ? "Client Error" : "Server Error";
        }
    }

    /// `n` in decimal onto `out`, with no temporary string.
    inline void append_decimal(std::string& out, size_t n)
    {
        char d[24];
        out.append(d, static_cast<size_t>(std::to_chars(d, d + sizeof d, n).ptr - d));
    }

    /// `HTTP/1.1 <status> <reason>` and its CRLF.
    inline void append_status_line(std::string& out, int status)
    {
        out.append("HTTP/1.1 ");
        append_decimal(out, static_cast<size_t>(status < 0 ? 0 : status));
        out += ' ';
        out.append(reason(status));
        out.append("\r\n");
    }

    /// `Content-Length: <n>` and its CRLF.
    inline void append_content_length(std::string& out, size_t n)
    {
        out.append("Content-Length: ");
        append_decimal(out, n);
        out.append("\r\n");
    }

    namespace detail
    {
        inline bool starts_ci(std::string_view s, std::string_view lower) noexcept
        {
            return s.size() >= lower.size() && same_ci(s.substr(0, lower.size()), lower);
        }
    } // namespace detail

    /// Whether a relayed response keeps this upstream header: when to retry, the
    /// rate limits, and the venue's id for the request.
    inline bool relayed(const HeaderLine& h) noexcept
    {
        switch (h.field)
        {
            case Field::RetryAfter:
            case Field::RequestId:
            case Field::RequestsRemaining:
            case Field::InputTokensRemaining:
            case Field::OutputTokensRemaining:
            case Field::TokensRemaining:
                return true;
            case Field::Other:
                return detail::same_ci(h.name, "retry-after-ms") ||
                       detail::starts_ci(h.name, "x-ratelimit-") ||
                       detail::starts_ci(h.name, "anthropic-ratelimit-") ||
                       detail::starts_ci(h.name, "ratelimit");
            default:
                return false;
        }
    }

    /// Copies the lines of a response `head` (start line included, the final CRLFCRLF
    /// not) that relayed() keeps, verbatim; with `own_body`, the body is the upstream's,
    /// so its Content-Type and Content-Encoding too. True when a Content-Type was copied.
    inline bool append_relayed(std::string& out, std::string_view head, bool own_body)
    {
        bool typed = false;
        (void)walk_headers(head, [&](const HeaderLine& h) noexcept {
            const bool own = own_body && (h.field == Field::ContentType || h.field == Field::ContentEncoding);
            if (!own && !relayed(h)) return true;
            for (const char c : h.value)
                if ((static_cast<unsigned char>(c) < 0x20 && c != '\t') || c == 0x7f) return true;
            typed = typed || h.field == Field::ContentType;
            out.append(h.name.data(), static_cast<size_t>(h.value.data() + h.value.size() - h.name.data()));
            out.append("\r\n");
            return true;
        });
        return typed;
    }
} // namespace llmbridge::net::http
