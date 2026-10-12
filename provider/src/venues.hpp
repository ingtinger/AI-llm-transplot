// Copyright 2026 Kottos AI, Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "provider/dialect.hpp"

namespace llmbridge::provider::detail // each venue's request translator; false refuses
{
    bool anthropic_request(const json::Value& in, RequestOut& out);
    bool bedrock_request(const json::Value& in, RequestOut& out);
    bool gemini_request(const json::Value& in, RequestOut& out);
    bool cohere_request(const json::Value& in, RequestOut& out);
    bool azure_request(const json::Value& in, RequestOut& out);
} // namespace llmbridge::provider::detail
