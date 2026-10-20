# AI-llm-transplot 扩展：带 OpenSSL 的网关运行镜像，遵循 Apache-2.0 许可证。
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S /src -B /out -DCMAKE_BUILD_TYPE=Release \
        -DLLMBRIDGE_WERROR=ON -DLLMBRIDGE_TLS=ON \
        -DLLMBRIDGE_BUILD_TESTS=OFF -DLLMBRIDGE_BUILD_TOOLS=OFF \
    && cmake --build /out -j"$(nproc)" --target llmbridge

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates libssl3t64 libstdc++6 libgcc-s1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/bin/llmbridge /usr/local/bin/llmbridge
COPY LICENSE NOTICE /usr/share/doc/ai-llm-transplot/
USER 10001:10001
EXPOSE 8088
ENTRYPOINT ["/usr/local/bin/llmbridge"]
