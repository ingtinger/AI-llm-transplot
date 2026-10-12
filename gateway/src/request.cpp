// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#include "request.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <charconv>
#include <string>

#include "net/http_write.hpp"
#include "net/sigv4.hpp"
#include "provider/json.hpp" // translate_failure re-parses a refused body

namespace llmbridge::detail
{
    namespace
    {
        // Anthropic's required versioning header. Pinned, not passthrough-only:
        // an OpenAI-SDK client has never heard of it, and without it the API
        // rejects the request outright. A client that does send its own value
        // wins (see auth_headers_for); the pin is a default, not an override.
        constexpr std::string_view kAnthropicVersionDefault = "2023-06-01";

        // Is this safe to re-emit as an HTTP header value?
        //
        // Do not trust find_header() to have made this safe. It splits on CRLF, so
        // a value may still contain a bare CR (or LF, NUL, any control byte), and
        // a lenient upstream parser that treats bare CR as a line terminator would
        // then see an injected header. That is not hypothetical: it was measured
        // reaching the upstream as `x-api-key: sk\rX-Smuggled: yes` before this
        // check existed. It matters more than a self-inflicted malformed request
        // because upstream connections are pooled and shared between clients, so
        // smuggling on one is a cross-client request-splitting vector.
        //
        // RFC 7230 field-value permits obs-text (0x80-0xFF); we are deliberately
        // stricter. A provider API key is printable ASCII, and narrowing the
        // charset costs us nothing real.
        bool header_value_safe(std::string_view v) noexcept
        {
            if (v.empty() || v.size() > 8192) return false;
            for (const unsigned char ch : v)
                if (ch < 0x20 || ch > 0x7E) return false; // CTLs (incl. CR/LF/NUL) and 8-bit
            return true;
        }

        // Trailing OWS is legal in a header line but is not part of the credential;
        // forwarding it verbatim has broken real provider auth before.
        std::string_view rtrim_ows(std::string_view v) noexcept
        {
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
            return v;
        }

        /// Whether the operator's strip list names this header. Entries are lower-case,
        /// with or without their colon.
        bool stripped(std::string_view name, const std::vector<std::string>& strip) noexcept
        {
            for (std::string_view s : strip)
            {
                if (!s.empty() && s.back() == ':') s.remove_suffix(1);
                if (net::http::detail::same_ci(name, s)) return true;
            }
            return false;
        }

        /// "Bearer K" -> K, the scheme in any case (RFC 9110 11.1); empty for any other.
        std::string_view bearer_token(std::string_view v) noexcept
        {
            if (v.size() <= 7 || !net::http::detail::same_ci(v.substr(0, 6), "bearer") ||
                (v[6] != ' ' && v[6] != '\t'))
                return {};
            return net::http::detail::ltrim(v.substr(7));
        }

        /// Whether a client's origin-form path is safe to forward under a venue's base
        /// path: no dot segment and no backslash, in plain or percent-encoded form, since
        /// the upstream resolves them and `/../` would leave the configured prefix.
        bool target_path_ok(std::string_view target)
        {
            const std::string_view path = target.substr(0, target.find('?'));
            for (size_t i = 0; i < path.size(); ++i)
            {
                const char c = path[i];
                if (c == '\\') return false;
                if (c == '%' && i + 2 < path.size())
                {
                    const char a = path[i + 1], b = static_cast<char>(path[i + 2] | 0x20);
                    if ((a == '2' && (b == 'e' || b == 'f')) || (a == '5' && b == 'c')) return false;
                }
                if (c == '/' && i + 1 < path.size() && path[i + 1] == '.')
                {
                    const size_t end = path.find('/', i + 1);
                    const std::string_view seg = path.substr(i + 1, end == std::string_view::npos
                                                                         ? std::string_view::npos
                                                                         : end - i - 1);
                    if (seg == "." || seg == "..") return false;
                }
            }
            return true;
        }

        /// The client's credential lines in the venue's terms, appended to `out`. Every
        /// credential the client sent is checked first, whether or not this venue reads
        /// it, so a malformed value cannot route around the check by picking another.
        const char* append_auth(UpstreamDialect mode, std::string_view head, const HeadFacts& f,
                                std::string& out)
        {
            const std::string_view auth = f.authorization.in(head), x_api_key = f.x_api_key.in(head),
                                   api_key = f.api_key.in(head), goog = f.x_goog_api_key.in(head),
                                   version = f.anthropic_version.in(head);
            for (const std::string_view v : {auth, x_api_key, api_key, goog, version})
                if (!v.empty() && !header_value_safe(v)) return "credential";
            const auto line = [&out](std::string_view name, std::string_view v) {
                if (v.empty()) return;
                out.append(name).append(": ").append(v).append("\r\n");
            };
            switch (mode)
            {
                case UpstreamDialect::Anthropic:
                    line("x-api-key", x_api_key.empty() ? bearer_token(auth) : x_api_key);
                    line("anthropic-version", version.empty() ? kAnthropicVersionDefault : version);
                    return nullptr;
                case UpstreamDialect::Gemini:
                    line("x-goog-api-key", goog.empty() ? bearer_token(auth) : goog);
                    return nullptr;
                case UpstreamDialect::Cohere:
                    line("Authorization", auth); // Cohere speaks Bearer natively
                    return nullptr;
                case UpstreamDialect::Azure:
                {
                    // Azure's own `api-key` first; the OpenAI SDK against an Azure
                    // endpoint sends Authorization, the normal case.
                    std::string_view key = !api_key.empty() ? api_key : x_api_key;
                    if (key.empty()) key = bearer_token(auth);
                    if (key.empty()) return "no credential";
                    line("api-key", key);
                    return nullptr;
                }
                case UpstreamDialect::Bedrock: // signed instead: sign_bedrock
                case UpstreamDialect::OpenAI:  // byte-forwarded, never rebuilt here
                    break;
            }
            return "credential";
        }

        /// An OpenAI body in the venue's dialect, into `out`, and the venue's path for it,
        /// into `target`. False refuses the request.
        bool xlate_req(UpstreamDialect mode, std::string_view body, bool* wants_stream_usage,
                       std::string& out, std::string& target)
        {
            out.clear();
            switch (mode)
            {
                case UpstreamDialect::Anthropic:
                    target = "/v1/messages";
                    return provider::openai_to_anthropic_request(body, out, wants_stream_usage);
                case UpstreamDialect::Gemini:
                    target = "/v1beta/models/gemini:generateContent";
                    out = provider::openai_to_gemini_request(body);
                    return !out.empty(); // Gemini and Cohere refuse by returning nothing
                case UpstreamDialect::Cohere:
                    target = "/v2/chat";
                    out = provider::openai_to_cohere_request(body);
                    return !out.empty();
                case UpstreamDialect::Bedrock:
                {
                    // The model moves from the body into the path, encoded as one
                    // segment, slashes included: an inference-profile id may hold them.
                    std::string model;
                    if (!provider::openai_to_bedrock_request(body, model, out) || model.empty())
                    { out.clear(); return false; }
                    target.assign("/model/").append(net::sigv4::uri_encode(model, true)).append("/invoke");
                    return true;
                }
                case UpstreamDialect::Azure:
                    // Azure serves the OpenAI dialect: the body goes as the client wrote it,
                    // so an option we do not model is not dropped on the way through.
                    out.assign(body);
                    target = "/chat/completions";
                    return !out.empty();
                case UpstreamDialect::OpenAI:
                    break;
            }
            return false;
        }

        /// Sign a rebuilt Bedrock request into `out`, or refuse it. Here and not at the
        /// client: the target and body are both rewritten, so any earlier signature is void.
        bool sign_bedrock(const Upstream& up, std::string_view authorization,
                          std::string_view target, std::string_view body, std::string& out,
                          const char*& why)
        {
            why = "credential";
    #ifdef LLMBRIDGE_HAVE_TLS
            if (up.aws_region.empty()) return false;
            if (!header_value_safe(authorization)) return false;
            why = "bedrock credential";
            const std::string_view bearer = bearer_token(authorization);
            if (bearer.empty()) return false;

            net::sigv4::Credentials cred{};
            if (!net::sigv4::parse_credentials(bearer, cred)) return false;
            why = "credential";

            char stamp[17];
            const std::time_t now = std::time(nullptr);
            std::tm utc{};
            if (::gmtime_r(&now, &utc) == nullptr) return false;
            if (std::strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &utc) != 16) return false;

            net::sigv4::Request r{};
            r.method = "POST";
            r.path = target;
            r.host = up.host_hdr;
            r.content_type = "application/json";
            r.body = body;
            r.region = up.aws_region;
            // The signing service name is `bedrock` for the runtime endpoint too; it
            // does not follow the hostname, which is one of the ways this returns 403.
            r.service = "bedrock";
            r.amz_date = std::string_view(stamp, 16);

            const auto headers = net::sigv4::sign(cred, r);
            if (headers.empty()) return false;
            for (const auto& hdr : headers)
                out.append(hdr.name).append(": ").append(hdr.value).append("\r\n");
            return true;
    #else
            (void)up; (void)authorization; (void)target; (void)body; (void)out; (void)why;
            return false;   // no OpenSSL, no signature, and never an unsigned request
    #endif
        }

    } // namespace

    bool HeadReader::operator()(const net::http::HeaderLine& h) noexcept
    {
        using net::http::Field;
        const auto span = [this](std::string_view v) {
            return HeadFacts::Span{static_cast<uint32_t>(v.data() - head.data()),
                                   static_cast<uint32_t>(v.size())};
        };
        // First occurrence wins (anti-smuggling: a repeated credential must resolve the
        // same way for us and the upstream). A stripped one is invisible, or a tenant's
        // Authorization would still be mapped onto the provider credential.
        const auto cred = [&](HeadFacts::Span& s) {
            if (s.empty() && !stripped(h.name, strip))
                s = span(rtrim_ows(net::http::detail::ltrim(h.value)));
        };
        switch (h.field)
        {
            case Field::Authorization: cred(f.authorization); break;
            case Field::XApiKey: cred(f.x_api_key); break;
            case Field::ApiKey: cred(f.api_key); break;
            case Field::XGoogApiKey: cred(f.x_goog_api_key); break;
            case Field::AnthropicVersion: cred(f.anthropic_version); break;
            case Field::ContentLength: f.content_length = true; break;
            case Field::Expect:
                // RFC 9110 10.1.1: answer a 100 or a final status before reading the body.
                if (f.expect == 0)
                {
                    const std::string_view v = net::http::detail::ltrim(h.value);
                    f.expect = v.size() >= 12 && net::http::detail::line_is(v, "100-continue") ? 2 : 1;
                }
                break;
            case Field::Connection:
                if (f.connections < HeadFacts::kConnections) f.connection[f.connections] = span(h.value);
                if (f.connections <= HeadFacts::kConnections) ++f.connections;
                break;
            default: break;
        }
        return true;
    }

    // A test seam over one header block: the facts a framing walk would read.
    bool auth_headers_for(UpstreamDialect mode, std::string_view client_headers,
                          const std::vector<std::string>& strip, std::string& out)
    {
        out.clear();
        while (client_headers.size() >= 2 && client_headers.substr(client_headers.size() - 2) == "\r\n")
            client_headers.remove_suffix(2);
        const std::string head = "POST / HTTP/1.1\r\n" + std::string(client_headers);
        HeadFacts f;
        if (!net::http::walk_headers(head, HeadReader{head, strip, f})) return false;
        return append_auth(mode, head, f, out) == nullptr;
    }

    Dialect client_dialect_of(std::string_view head) noexcept
    {
        const std::string_view rl = head.substr(0, head.find("\r\n"));
        const size_t sp1 = rl.find(' '), sp2 = rl.rfind(' ');
        if (sp1 == std::string_view::npos || sp2 <= sp1) return Dialect::OpenAI;
        return client_wire(rl.substr(0, sp1), rl.substr(sp1 + 1, sp2 - sp1 - 1));
    }

    /// The translation for a request, from the client's dialect (read off the request
    /// line) and the venue's. Shared by both backends; the caller handles the refusal
    /// so each can use its own error path. A free function, not a Gateway method,
    /// because TranslationPlan lives in dialect.hpp which already includes gateway.hpp.
    provider::TopLevelFacts body_facts(std::string_view head, std::string_view body) noexcept
    {
        if (client_dialect_of(head) == Dialect::OpenAI) return provider::top_level_facts(body);
        provider::TopLevelFacts f;
        f.model = provider::model_of(body);
        return f;
    }

    TranslationPlan resolve_dialect(const ClientConn* c, const Upstream& venue) noexcept
    {
        const std::string_view head(c->rbuf.data(), c->msg.header_len);
        const std::string_view body(c->rbuf.data() + c->msg.header_len, c->msg.body_len);
        // Whether the client asked to stream is consulted for one venue only,
        // because Bedrock cannot stream from this endpoint, and answering it means walking
        // the body to its top-level `stream` key.
        const bool stream_matters = venue.dialect == UpstreamDialect::Bedrock;
        return resolve_translation(client_dialect_of(head), venue.dialect,
                                   stream_matters && provider::wants_stream(body));
    }

    /// Why a request translation failed, for the log only. The translator returns
    /// an empty string on any failure and keeps no reason, so this re-parses the
    /// body to separate the two cases a caller can actually act on: bytes that
    /// are not JSON at all, against JSON whose shape we cannot translate. Costs a
    /// parse, but only on a path that has already failed and is about to answer
    /// 400, so it is never on the hot path.
    ///
    /// It exists because "request translate" told an operator nothing: a client
    /// sending Python's `True` instead of `true` and a client sending a model we
    /// do not support produced the identical line.
    const char* translate_failure(std::string_view body) noexcept
    {
        bool ok = false;
        const provider::json::Value v = provider::json::parse(body, ok);
        if (!ok)
        {
            (void)provider::json::parse(body, ok, provider::json::Keys::Any);
            return ok ? refuse::kRepeatedKey : refuse::kNotJson;
        }
        if (!v.is_object()) return refuse::kNotObject;
        if (!v.find("model")) return refuse::kNoModel;
        const provider::json::Value* msgs = v.find("messages");
        if (!msgs) return refuse::kNoMessages;

        // Name the part we could not carry. A caller who sends an image gets
        // "vision is not implemented" instead of "unsupported request shape",
        // which is the difference between reading the README and filing a bug.
        if (msgs->is_array())
            for (const provider::json::Value& m : msgs->arr)
            {
                const provider::json::Value* c = m.find("content");
                if (!c || !c->is_array()) continue;
                for (const provider::json::Value& part : c->arr)
                {
                    const std::string_view t = part.str_or("type");
                    if (t == "text") continue;
                    if (t == "image_url" || t == "image")
                        return refuse::kImage;
                    if (t == "input_audio" || t == "audio")
                        return refuse::kAudio;
                    if (t == "file" || t == "document")
                        return refuse::kFile;
                    return refuse::kPart;
                }
            }

        // Tool arguments are a JSON string that must decode to one object, and
        // nothing else: see the splice in translate_body.cpp.
        if (msgs->is_array())
            for (const provider::json::Value& m : msgs->arr)
            {
                const provider::json::Value* tcs = m.find("tool_calls");
                if (!tcs || !tcs->is_array()) continue;
                for (const provider::json::Value& call : tcs->arr)
                {
                    const provider::json::Value* fn = call.find("function");
                    if (!fn) continue;
                    const std::string args =
                        provider::json::unescape_string(fn->str_or("arguments"));
                    if (args.empty()) continue;
                    bool aok = false;
                    const provider::json::Value parsed = provider::json::parse(args, aok);
                    if (!aok || !parsed.is_object()) return refuse::kToolArgs;
                }
            }
        return refuse::kShape;
    }

    const char* dialect_name(UpstreamDialect m) noexcept
    {
        switch (m)
        {
            case UpstreamDialect::OpenAI: return "openai";
            case UpstreamDialect::Anthropic: return "anthropic";
            case UpstreamDialect::Gemini: return "gemini";
            case UpstreamDialect::Cohere: return "cohere";
            case UpstreamDialect::Bedrock: return "bedrock";
            case UpstreamDialect::Azure: return "azure";
        }
        return "?";
    }

    // A minimal HTTP/1.1 message: start line, JSON type, keep-alive, length, `extra`
    // (zero or more complete "Name: value\r\n" lines), then the body.
    void append_http(std::string& out, std::string_view start_line, std::string_view body,
                     std::string_view extra)
    {
        const size_t need = out.size() + start_line.size() + body.size() + extra.size() + 96;
        if (out.capacity() < need) out.reserve(need);
        out.append(start_line);
        out.append("\r\nContent-Type: application/json\r\nConnection: keep-alive\r\n");
        net::http::append_content_length(out, body.size());
        out.append(extra);
        out.append("\r\n");
        out.append(body);
    }

    // Kept so the framing has one test; build_translated_request writes the same shape.
    void build_http_request(std::string_view start_line, std::string_view body,
                            std::string_view host, std::string_view extra, std::string& into)
    {
        into.clear();
        const size_t need = start_line.size() + body.size() + host.size() + extra.size() + 128;
        if (into.capacity() < need) into.reserve(need + need / 8);
        into.append(start_line).append("\r\nHost: ").append(host).append("\r\n").append(extra);
        into.append("Content-Type: application/json\r\nConnection: keep-alive\r\n");
        net::http::append_content_length(into, body.size());
        into.append("\r\n").append(body);
    }

    /// A byte-forwarded request, rebuilt in one pass into `into`: Host names the venue,
    /// because the request goes to a different origin than the client addressed; the
    /// Content-Length is ours, from the bytes actually sent; Expect asks the upstream
    /// for an interim reply we cannot relay; Accept-Encoding goes so the provider
    /// answers in a body we can measure; and the hop-by-hop headers and any the client's
    /// Connection names are this hop's, so the venue alone decides its own keep-alive.
    /// The request line is copied, never tested as a header, and an HTTP/1.0 one goes
    /// as 1.1 so the upstream connection can be pooled.
    const char* build_forward(std::string_view msg, size_t header_len, const HeadFacts& f,
                              const std::vector<std::string>& strip, std::string_view host_hdr,
                              std::string_view base_path, std::string_view body_override,
                              std::string& into)
    {
        using net::http::Field;
        into.clear();
        if (f.connections > HeadFacts::kConnections) return "too many Connection headers";
        const std::string_view head = msg.substr(0, header_len < 4 ? 0 : header_len - 4);
        const size_t eol = std::min(head.find("\r\n"), head.size());
        const std::string_view rl = head.substr(0, eol);
        // Origin-form only: absolute-form would pick the upstream vhost, authority-form is
        // CONNECT's, and a dot segment could resolve outside the base. Never normalized.
        const size_t sp1 = rl.find(' ');
        const size_t sp2 = sp1 == std::string_view::npos ? sp1 : rl.find(' ', sp1 + 1);
        if (sp2 == std::string_view::npos || sp2 == sp1 + 1 || rl[sp1 + 1] != '/')
            return "request target not origin-form";
        const std::string_view target = rl.substr(sp1 + 1, sp2 - sp1 - 1);
        if (!target_path_ok(target)) return "request target not origin-form";
        const std::string_view version = rl.substr(sp2 + 1);
        const std::string_view body = body_override.empty() ? msg.substr(header_len) : body_override;

        const size_t need = header_len + host_hdr.size() + base_path.size() + body.size() + 64;
        if (into.capacity() < need) into.reserve(need + need / 8);
        into.append(rl.substr(0, sp1 + 1)).append(base_path).append(target).append(" ");
        into.append(version == "HTTP/1.0" ? std::string_view("HTTP/1.1") : version).append("\r\n");
        if (!host_hdr.empty()) into.append("Host: ").append(host_hdr).append("\r\n");
        // A bodyless request that carried no length keeps carrying none.
        if (f.content_length || !body.empty()) net::http::append_content_length(into, body.size());
        const auto listed = [&](std::string_view name) {
            for (uint8_t i = 0; i < f.connections; ++i)
                if (net::http::has_token(f.connection[i].in(msg), name)) return true;
            return false;
        };
        // Copied in runs: a flush happens only where a dropped line interrupts kept ones.
        size_t run = eol + 2;
        const bool ok = net::http::walk_headers(head, [&](const net::http::HeaderLine& h) noexcept {
            const bool drop = (h.field == Field::Host && !host_hdr.empty()) ||
                              h.field == Field::ContentLength || h.field == Field::Expect ||
                              h.field == Field::AcceptEncoding || net::http::hop_by_hop(h.field) ||
                              stripped(h.name, strip) || (h.field == Field::Other && listed(h.name));
            if (!drop) return true;
            const size_t at = static_cast<size_t>(h.name.data() - head.data());
            if (at > run) into.append(head.substr(run, at - run));
            run = static_cast<size_t>(h.value.data() + h.value.size() - head.data()) + 2;
            return true;
        });
        if (!ok) { into.clear(); return "request head"; }
        if (run < head.size()) into.append(head.substr(run)).append("\r\n");
        into.append("\r\n").append(body);
        return nullptr;
    }

    bool request_without(std::string_view msg, size_t header_len,
                         const std::vector<std::string>& strip,
                         std::string_view host_hdr, std::string_view base_path,
                         std::string_view body_override, std::string& into)
    {
        HeadFacts f;
        const std::string_view head = msg.substr(0, header_len < 4 ? 0 : header_len - 4);
        if (!net::http::walk_headers(head, HeadReader{msg, strip, f})) { into.clear(); return false; }
        return build_forward(msg, header_len, f, strip, host_hdr, base_path, body_override, into) == nullptr;
    }

    /// Everything between a client request and the bytes for a translated venue, written
    /// straight into `out`. The body and target come first, because a Bedrock signature
    /// covers both; the credential is the client's, from the framing walk, in the
    /// venue's terms.
    bool build_translated_request(const Upstream& up, UpstreamDialect mode,
                                  std::string_view body, std::string_view head,
                                  const HeadFacts& f, std::string& out,
                                  std::string& body_scratch, const char*& why,
                                  std::string_view model_override, bool* wants_stream_usage)
    {
        // One rewrite before translating serves every dialect: the translators read the
        // model out of the body, and Bedrock then puts it in the path.
        std::string rewritten;
        if (!model_override.empty())
        {
            rewritten = provider::rewrite_model(body, model_override);
            if (rewritten.empty()) { why = "translate"; return false; }
            body = rewritten;
        }
        std::string target;
        if (!xlate_req(mode, body, wants_stream_usage, body_scratch, target))
        { why = "translate"; return false; }
        const std::string_view tbody = body_scratch;

        out.clear();
        const size_t need = tbody.size() + up.base_path.size() + target.size() + up.query.size() +
                            up.host_hdr.size() + 512;
        if (out.capacity() < need) out.reserve(need + need / 8);
        // The venue's query, not the client's: Azure's api-version rides here.
        out.append("POST ").append(up.base_path).append(target);
        if (!up.query.empty()) out.append("?").append(up.query);
        out.append(" HTTP/1.1\r\nHost: ").append(up.host_hdr).append("\r\n");
        if (mode == UpstreamDialect::Bedrock)
        {
            // Signed over exactly the target written above, between "POST " and " HTTP/1.1".
            const std::string_view signed_target(out.data() + 5, out.find(" HTTP/1.1") - 5);
            const std::string path(signed_target);
            if (!sign_bedrock(up, f.authorization.in(head), path, tbody, out, why))
            { out.clear(); return false; }
        }
        else if (const char* bad = append_auth(mode, head, f, out))
        { why = bad; out.clear(); return false; }
        out.append("Content-Type: application/json\r\nConnection: keep-alive\r\n");
        net::http::append_content_length(out, tbody.size());
        out.append("\r\n").append(tbody);
        return true;
    }

    // Translate an upstream response body back to the OpenAI shape into `out`, with
    // the counts it states in `u`. False: the body could not be translated.
    bool xlate_resp(UpstreamDialect mode, std::string_view body, std::string& out,
                    provider::openai::Usage& u)
    {
        switch (mode)
        {
            // Bedrock answers with Anthropic's Messages envelope, so the response
            // leg is the same translator; only the request leg differs.
            case UpstreamDialect::Bedrock:
            case UpstreamDialect::Anthropic:
                return provider::anthropic_to_openai_response(body, out, u);
            // Azure answers in the OpenAI shape it was asked in.
            case UpstreamDialect::Azure:
                out.assign(body);
                u = provider::openai::scan_usage(body);
                return !out.empty();
            case UpstreamDialect::Gemini: return provider::gemini_to_openai_response(body, out, u);
            case UpstreamDialect::Cohere: return provider::cohere_to_openai_response(body, out, u);
            case UpstreamDialect::OpenAI: break;
        }
        out.clear();
        u = {};
        return false;
    }

    /// `Host:` for a rebuilt request. The parsed hostname when there is one, since
    /// providers route and verify on it; ip:port otherwise. Default ports omitted.
    std::string host_header_for(const Upstream& u)
    {
        if (u.sni_host.empty()) return u.ip + ":" + std::to_string(u.port);
        const bool default_port = (u.tls && u.port == 443) || (!u.tls && u.port == 80);
        return default_port ? u.sni_host : u.sni_host + ":" + std::to_string(u.port);
    }

    /// The AWS region inside an endpoint name, or empty.
    ///
    /// `bedrock-runtime.us-east-1.amazonaws.com` -> `us-east-1`. Derived and not
    /// configured because the endpoint already states it and two sources of one
    /// fact drift; empty is a refusal, never a default, since signing with the
    /// wrong region returns a 403 whose body explains nothing.
    std::string aws_region_for(const Upstream& u)
    {
        const std::string& h = u.sni_host;
        const size_t first = h.find('.');
        if (first == std::string::npos) return {};
        const size_t second = h.find('.', first + 1);
        if (second == std::string::npos) return {};
        const std::string_view region(h.data() + first + 1, second - first - 1);
        // Shape check, not a list: regions are added faster than any list is
        // updated, and "letters, digits and hyphens with a digit in it" separates
        // `us-east-1` from `amazonaws` without pinning us to a snapshot of AWS.
        if (region.size() < 5 || region.find_first_of("0123456789") == std::string_view::npos)
            return {};
        for (const char c : region)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                return {};
        return std::string(region);
    }

    /// The client's address, for a refusal we are about to log. Empty when the
    /// socket cannot answer, which is normal on one already torn down.
    std::string peer_of(int fd) noexcept
    {
        sockaddr_in a{};
        socklen_t len = sizeof a;
        if (fd < 0 || ::getpeername(fd, reinterpret_cast<sockaddr*>(&a), &len) != 0)
            return {};
        if (a.sin_family != AF_INET) return {};
        char buf[INET_ADDRSTRLEN];
        if (!::inet_ntop(AF_INET, &a.sin_addr, buf, sizeof buf)) return {};
        return std::string(buf) + ":" + std::to_string(ntohs(a.sin_port));
    }

} // namespace llmbridge::detail
