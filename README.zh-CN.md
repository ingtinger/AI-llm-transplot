# AI-llm-transplot 高性能 AI 网关

本项目以 Apache-2.0 许可的 `kottos-ai/llmbridge` 为网络与协议转换基础，目标是实现一个可由多个用户连接、能够管理多个 AI 上游的高性能网关服务器。

上游项目提供成熟的 C++20 HTTP/SSE 数据面、OpenAI 兼容入口、Anthropic/Gemini/Cohere 等协议转换、`epoll`/`io_uring`、TLS 以及完善的边界测试。本分支在此基础上增加多上游路由和面向实际部署的网关能力。

## 当前能力

- OpenAI 兼容请求入口和 SSE 流式响应。
- OpenAI、Anthropic、Gemini、Cohere 等上游协议适配。
- 多上游配置。
- 顺序优先、轮询和会话粘性三种路由模式。
- 请求尚未发给上游的连接故障自动切换；可能已执行的请求默认不重放。
- 连续失败计数和冷却期，暂时避开异常上游。
- 每个上游独立的模型名称映射。
- 安全模式下的虚拟网关 Key、用户模型/上游权限、服务端 Provider Key 注入。
- 单机每用户请求速率、并发和基于上游报告用量的 UTC 日/月 Token 配额。
- 原版 `X-Kottos-Key-*` 客户端传密钥方式只在显式 `security.legacy_mode=true` 或 `--legacy-mode` 时启用，不适合公网多用户部署。
- 多工作线程共享同一套路由健康状态。
- `SIGHUP` 热更新上游摘除列表；在途请求继续使用旧连接和不可变配置。
- 独立回环管理端口：`/health`、`/ready`、Prometheus `/metrics`。
- 结构化请求日志：安全模式强制启用，兼容模式可选；包含请求序号、用户索引、上游、状态和用量，不含密钥或正文。

## 构建

网关运行在 Linux。Windows 用户建议使用 WSL2 Ubuntu 24.04。

Ubuntu/WSL 首次构建 HTTPS 版本时需安装 OpenSSL 开发包：`sudo apt-get install libssl-dev`。运行库中的 `openssl` 命令不能代替此开发包。

```bash
git clone https://github.com/ingtinger/AI-llm-transplot.git
cd AI-llm-transplot
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLLMBRIDGE_WERROR=ON -DLLMBRIDGE_TLS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

仅运行隔离的本地 HTTP Mock 时，可以暂时使用 `-DLLMBRIDGE_TLS=OFF`，并在测试配置中显式设置 `security.allow_insecure_http=true`；此配置不能用于公网服务。

## 安全模式配置

下面的配置把一个逻辑模型映射到两个不同的 AI 服务。配置文件只记录环境变量名，不记录真实密钥。

```json
{
  "listen": {
    "port": 8088,
    "tls": true,
    "cert": "/path/to/cert.pem",
    "key": "/path/to/key.pem"
  },
  "upstream": [
    {
      "url": "https://api.openai.com",
      "dialect": "openai",
      "credential_env": "PROVIDER_OPENAI_KEY",
      "model": "gpt-4.1-mini"
    },
    {
      "url": "https://api.anthropic.com",
      "dialect": "anthropic",
      "credential_env": "PROVIDER_ANTHROPIC_KEY",
      "model": "claude-sonnet-4-5"
    }
  ],
  "security": {
    "enabled": true,
    "users": [{
      "id": "alice", "tenant": "team-a", "key_env": "GATEWAY_ALICE_KEY",
      "models": ["chat-default"], "upstreams": [0, 1],
      "requests_per_minute": 60, "max_concurrent": 4,
      "daily_tokens": 100000, "monthly_tokens": 1000000
    }]
  },
  "routing": {
    "mode": "sticky",
    "max_attempts": 2,
    "failure_threshold": 3,
    "cooldown_s": 10,
    "sticky_header": "x-conversation-id"
  },
  "timeouts": {
    "connect_s": 5,
    "upstream_s": 120
  }
}
```

启动服务器：

```bash
PROVIDER_OPENAI_KEY=... PROVIDER_ANTHROPIC_KEY=... GATEWAY_ALICE_KEY=... \
  ./build/bin/llmbridge --config /path/to/your-config.json
```

需要摘除某个上游时，在配置的 `routing` 中设置 `"drained_upstreams": ["0"]`（索引从 0 开始），原子替换配置文件后向进程发送 `SIGHUP`。新请求不再分配到索引 0，已经分配的请求继续完成；清空列表并再次发送 `SIGHUP` 可恢复。仅此字段支持热更新；其他字段变更会被拒绝，旧配置继续生效，新增/删除上游或修改密钥须重启进程。`/ready` 会在全部上游摘除时变为不可用。热加载不会中断现有 SSE。

`Retry-After` 支持秒数和标准 HTTP 日期，最多采信 300 秒。默认只在请求尚未发给上游时自动切换；上游可能已执行的请求不会透明重放。需要对完整 `408`、`429`、`5xx` 或发送后的连接故障继续切换时，可显式设置 `routing.allow_unsafe_replay=true`，但非幂等聊天请求可能在不同 Provider 重复执行/计费，即使客户端只收到一个回答；没有跨 Provider 的端到端幂等保证，请勿将此选项用于精确账单。单机内存限额不跨实例共享，Redis 尚未接入。

## 请求方式

客户端必须为每段对话生成稳定的 `X-Conversation-Id`。聊天历史仍通过完整的 `messages` 发送；会话 ID 只负责让同一对话优先进入同一上游，不能代替上下文本身。

```bash
curl --cacert /path/to/ca.pem https://127.0.0.1:8088/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H 'X-Conversation-Id: conversation-1001' \
  -H 'Authorization: Bearer <你的网关虚拟Key>' \
  -d '{
    "model": "chat-default",
    "stream": true,
    "messages": [
      {"role":"user","content":"你好"}
    ]
  }'
```

网关验证虚拟 Key 后仅向该用户许可的上游发送对应的服务端 Provider Key。客户端的 `Authorization` 和 `X-Kottos-Key-*` 不会作为 Provider 凭证转发。缺失/错误/禁用/过期 Key 返回 401，模型越权返回 403，限额返回 429。用户必须继续携带完整 `messages`；相同会话 ID 不构成跨用户授权。

默认不启动无认证服务：必须配置 `security.enabled=true`，或仅做本地兼容测试时显式启用 `security.legacy_mode=true` / `--legacy-mode`；两者不能同时开启。安全模式还默认要求数据端口启用 TLS，并拒绝非回环地址的明文上游。仅隔离的本地 Mock 测试可显式设置 `security.allow_insecure_http=true`；此开关仍会监听所有网卡，**不能用于公网部署**。虚拟 Key 至少 16 个可打印 ASCII 字符。创建时生成随机 Key 并通过环境变量注入；撤销可把用户设为 `disabled: true`，轮换可更换对应环境变量值，均需重启进程。`expires_unix_s` 可设置 UTC Unix 秒时间戳。不要把真实 Key 写入 JSON、仓库或日志。

速率为固定 60 秒窗口，超限立即拒绝、不排队。当前认证在完整请求体到达后执行，因此限额不能约束尚在上传请求体的未认证连接；公网部署还需要前置连接/上传限额。Token 配额按上游返回的输入/输出用量在响应完成时累计；已发往上游但没有完整用量的请求会使有配额用户在本 UTC 日/月剩余时间拒绝。重试只结算最终记录，无法可靠还原前次尝试的账单；单个在途请求可能越过额度。所有计数仅在当前进程内，重启清零，多实例不共享，**不可用于精确扣费**。

兼容模式也仍监听所有网卡；即使只用于本地开发，也必须通过防火墙或隔离网络限制访问。

## 路由模式

- `first`：优先第一个上游，发生可重试的传输故障后依次切换。
- `round_robin`：在当前健康上游之间轮询。
- `sticky`：优先根据会话请求头做稳定哈希；缺少该请求头时使用请求前缀哈希。

配置 `upstream` 数组时，可给**每个**上游设置 `"models": ["chat-default"]`，声明它可服务的客户端模型；一旦启用，所有上游都必须配置非空列表。`round_robin` 为每个模型维护独立游标，故障切换仅在同模型池内进行。单数的 `"model"` 是发给 Provider 的模型映射名，不是模型池。

当某模型池内所有上游都处于普通故障冷却期时，网关允许一次探测请求，以便发现已经恢复的服务；`Retry-After` 限制期内不会探测。连续失败的冷却时间最多增加至基础值的 16 倍。

## 管理端点与请求日志

在配置文件中设置 `"management": {"port": 9091}`，或使用 `--management-port 9091`。默认端口为 `0`，即关闭。管理服务**仅绑定 `127.0.0.1`**，与对外提供 `/v1/chat/completions` 的数据端口分开；部署反向代理时不要把管理端口公开转发到互联网。

```bash
curl http://127.0.0.1:9091/health
curl http://127.0.0.1:9091/ready
curl http://127.0.0.1:9091/metrics
```

`/health` 只确认进程管理线程可响应；`/ready` 在至少一个上游既不处于冷却期、也不处于 `Retry-After` 限制期时返回 200，否则返回 503。它**不主动连接** Provider，因此不是外部服务可用性保证。`/metrics` 使用固定的上游数组索引作为标签，包含路由选择次数、传输失败数、上游响应状态类别、冷却状态和最终尝试的上游首字节延迟。路由选择次数也包括后来因校验失败而未真正发送的请求；延迟样本不代表整个 SSE 完成时间。

安全模式强制启用 JSON 请求日志；兼容模式可设置 `"runtime": {"request_log": true}` 或 `--request-log`。日志由有界队列转交专用线程写出；队列满时丢弃日志并增加 `ai_gateway_request_log_dropped_total`，不会阻塞网关工作线程。日志中的 `request_id` 是进程内请求序号，可关联用户索引、初始/最终上游、重试次数、状态和上游报告的 Token/费用字段；缺失值为 `-1`。它不是可用于扣费的持久化审计。日志不写 Provider Key、Authorization、提示词或回复正文。

## 当前边界

目前仍需要继续实现：

- 持久化/跨实例共享额度、运行时密钥轮换与可靠账单。
- 按 Provider/模型价格核算费用、处理重试中先前尝试的费用。
- 跨 Provider 的请求幂等与每次尝试的可靠计费；显式开启不安全重放后仍可能重复执行。
- Redis 共享的多实例限流、会话粘性和健康状态。
- 完整配置热替换、持久化审计日志和经过容器实测的生产部署方案；当前仅上游摘除列表可热加载，仓库提供本机部署示例。

第一次阅读代码可从 [开发者导读](docs/development.md) 开始；详细设计见 [docs/design.md](docs/design.md)，测试方法见 [可靠性验证](docs/reliability.md)，本机双 Mock 与 systemd 示例见 [部署说明](docs/deployment.md)，任务进度见 [docs/tasks.md](docs/tasks.md)。上游原始设计资料仍保留在 `DESIGN.md`、`GATEWAY-INTERNALS.md` 和 `LATENCY.md` 中。
