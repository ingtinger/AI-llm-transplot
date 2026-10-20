# AI-llm-transplot 开发者导读

本文面向第一次接触本工程的开发者，说明代码在哪里、一次请求怎样经过网关、如何在 WSL 中验证改动。项目目标与取舍见 [技术设计](design.md)，待办进度见 [任务清单](tasks.md)。本文描述的是**当前代码**，不是最终产品承诺。

## 1. 先分清两个工程

`/home/zzf/AI-llm-transplot` 是当前独立 Git 仓库，基于 `kottos-ai/llmbridge` 继续开发。当前构建、测试和本文的路径均以该目录为根目录。

本分支保留上游的 Apache-2.0 许可和网络、协议转换实现，在其上增加多上游配置与内置路由策略。上游 `README.md` 仍介绍原版单上游产品；本分支的能力与限制以 [中文 README](../README.zh-CN.md)、本文和代码为准。

## 2. 代码地图

| 路径 | 职责 | 建议先看什么 |
| --- | --- | --- |
| `app/main.cpp` | 进程入口；解析配置、解析和预解析上游、创建共享策略与工作线程，处理停止信号 | `run()` |
| `app/options.hpp`、`app/options.cpp` | 配置结构、命令行/JSON 解析、默认值与校验 | `Settings`、`options()`、`parse_config()`、`parse_cli()` |
| `app/routing_policy.hpp`、`app/routing_policy.cpp` | 本分支的多上游选择、失败计数、冷却与重试决策 | `RoutingPolicy::decide()`、`on_failure()`、`on_reply()` |
| `app/management_server.*`、`app/observability.*` | 回环管理 HTTP 服务、指标快照和有界异步请求日志 | `ManagementServer::handle()`、`Observability::on_request()` |
| `gateway/include/gateway/policy.hpp` | 上游定义的策略扩展契约；请求事实、选择结果、故障与回复回调 | `RequestFacts`、`Decision`、`Retry`、`Policy` |
| `gateway/include/gateway/gateway.hpp`、`gateway/src/facade.cpp` | 对外的 `Gateway` 接口和实现封装 | 构造函数、`run()`、`request_stop()` |
| `gateway/src/engine.cpp`、`gateway/src/core/` | 事件循环与连接、请求体、上游连接、响应、流式传输等状态机 | 从 `engine.cpp` 进入，再按问题找 `core/` 对应文件 |
| `gateway/src/request.cpp`、`gateway/src/response.cpp` | 请求/响应处理与协议边界 | 对照 `gateway/src/request.hpp`、`response.hpp` 阅读 |
| `net/include/net/`、`net/src/` | HTTP 解析、Socket、TLS、上游地址解析及网络基础设施 | `upstream.hpp`、`http.hpp`、`tls.hpp` |
| `provider/include/provider/`、`provider/src/` | OpenAI 与其他 Provider 的请求/响应及 SSE 协议转换 | `translate.hpp`、`registry.cpp`、各 `translate_*.cpp` |
| `app/tests/` | 配置、CLI、路由策略单测，以及真实进程的多上游/双用户端到端测试 | `routing_policy_test.cpp`、`multi_upstream_e2e.py`、`secure_users_e2e.py` |
| `gateway/tests/`、`net/tests/`、`provider/tests/` | 上游数据面、网络和协议转换的回归测试 | 与改动文件同名或相近的测试 |
| `app/ai-gateway.example.json` | 本分支双上游示例配置，地址是本地占位服务 | 先读注释字段和路由参数 |

头文件一般表示模块契约，`src/` 是实现，`tests/` 负责约束行为。阅读状态机前，建议先看上游的 `DESIGN.md` 和 `GATEWAY-INTERNALS.md`；它们比逐行阅读事件循环更容易建立整体模型。

## 3. 一次请求如何走完

1. `app/main.cpp` 调用 `parse_cli()`。配置文件先加载，命令行参数随后覆盖对应项；非法字段会拒绝启动。进程在启动阶段解析上游地址，构建上游表和一个共享的 `RoutingPolicy`。
2. 每个 `Gateway` 工作线程运行自己的事件循环并接收客户端连接。HTTP 请求头和请求体经 `net` 与 `gateway` 解析，形成 `RequestFacts`，其中包含模型名、是否流式、请求头等必要事实。
3. `gateway` 调用 `Policy::decide()`；安全模式先校验 Bearer 虚拟 Key 和模型权限，再按客户端模型池、用户上游白名单及 `first`、`round_robin` 或 `sticky` 选上游，返回 Provider 模型映射和服务端密钥视图。`Policy` 不保存聊天消息。
4. `gateway` 根据选择结果准备上游请求；`provider` 在需要时转换协议和模型相关字段，`net` 负责连接与 TLS。安全模式只注入所选上游的 Provider Key；旧兼容模式才从客户端 `X-Kottos-Key-*` 读取密钥。
5. 出现符合条件的传输故障时，`gateway` 调用 `on_failure()`；完整收到 `408`、`429`、`5xx` 时，先调用 `on_reply()` 记账，再根据 `routing.allow_unsafe_replay` 决定是否询问 `on_status_retry()` 切换。内置策略默认不重放可能已执行的请求；开启风险选项后仍受 `max_attempts` 限制，并按秒数或标准 HTTP 日期形式的 `Retry-After` 暂停选用该上游。响应最终经 `gateway` 返回客户端；SSE 按流转发。

`RoutingPolicy` 被多个工作线程共享。每模型轮询序号、连续失败数和冷却截止时间使用原子变量；新增共享状态时必须考虑并发和生命周期。`Gateway` 不拥有策略指针，策略必须比所有使用它的 `Gateway` 活得更久，创建和销毁顺序可在 `app/main.cpp` 中核对。

### 会话与故障切换的边界

- `X-Conversation-Id` 只是 `sticky` 模式选择上游的稳定键，**不是**聊天记录存储，也不是用户身份凭证。客户端每轮仍须发送完整 `messages`；切换模型后，文本上下文来自这些消息。
- 可重试的传输错误只在响应尚未提交客户端时透明切换。SSE 已经发出内容后不能再拼接另一个上游的输出。
- `408`、`429` 或 `5xx` 的**完整**错误响应可在客户端尚未收到字节时触发同请求切换；半截错误响应、流式上传及已提交的 SSE 不重试。这里仅保证客户端不拼接两个回答，不保证上游没有重复执行或重复计费；非幂等业务需另设幂等键或禁用透明重试。
- 安全模式已有静态用户认证、模型/上游权限、单机限流和 Token 额度；旧兼容模式必须显式开启，未选模式拒绝启动。公网部署仍须使用 TLS。额度不持久化，也不是精确账单。

## 4. 在 WSL 中构建与测试

以下命令在 WSL 的 Linux shell 执行，不是在 PowerShell 中执行。示例路径对应当前 Windows 工作区；如位置不同，请换成自己的 `/mnt/<盘符>/...` 路径。

```bash
cd /home/zzf/AI-llm-transplot
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release \
  -DLLMBRIDGE_WERROR=ON -DLLMBRIDGE_TLS=ON
cmake --build build-wsl -j"$(nproc)"
ctest --test-dir build-wsl -j4 --output-on-failure
```

需要 CMake 3.20+、C++20 编译器（建议 GCC 13+）、Python 3，以及启用 TLS 时的 OpenSSL 3 开发包。测试构建使用 CMake FetchContent 下载 GoogleTest，首次配置需要能访问 GitHub。缺少 OpenSSL 时可以暂时关闭 TLS 构建本地 Mock 场景：`-DLLMBRIDGE_TLS=OFF`；它不能连接公网 HTTPS Provider。

只运行本分支新增的真实进程故障切换测试：

```bash
ctest --test-dir build-wsl -R '^(AppMultiUpstreamFailover|AppStatusRetry)$' --output-on-failure
```

`app/tests/multi_upstream_e2e.py` 会自行启动本地备用 Mock、启动网关、构造一个不可达主上游，检查兼容模式的模型映射、密钥隔离和故障切换。`app/tests/status_retry_e2e.py` 覆盖 408/429/500 切换、503 尝试耗尽、限流等待和半截错误响应不重放。`app/tests/secure_users_e2e.py` 用虚拟 Key 和环境变量中的假 Provider Key 验证安全模式。均不需要真实 API Key。`app/ai-gateway.example.json` 使用 `127.0.0.1:9001/9002` 占位地址及环境变量名，直接启动它并不代表已有可用的 AI 后端。需要观察命令行可运行 `./build-wsl/bin/llmbridge --help`。

## 5. 修改功能时从哪里下手

| 需求 | 主要改动点 | 必须关注的验证 |
| --- | --- | --- |
| 新增配置项 | `app/options.hpp`、`app/options.cpp`、示例 JSON | `app/tests/config_test.cpp`；如新增 CLI 参数，再改 `cli_test.cpp` |
| 调整选择算法/冷却规则 | `app/routing_policy.*`，必要时 `gateway/include/gateway/policy.hpp` | `routing_policy_test.cpp`、`multi_upstream_e2e.py` |
| 新增协议转换 | `provider/src/` 与公开头文件 | `provider/tests/`，涵盖普通响应和支持的 SSE 情形 |
| 修改重试时机或 SSE 行为 | `gateway/src/core/`、`gateway/src/response.cpp` | 全量 CTest，新增连接中断和流式边界测试 |
| 加用户认证/额度 | 新的控制面组件与 `Policy` 集成 | 多用户隔离、错误凭证、并发和密钥泄露测试 |

改动前先找现有同类实现和测试，不要把阻塞磁盘 I/O、同步 DNS 或数据库操作直接放进事件循环。所有外部输入需要类型、长度、范围校验；日志不记录真实密钥、完整提示词或回复正文。项目自有新文档与注释使用中文，上游原有注释不做无关翻译。

## 6. 当前完成情况与下一步

已完成多上游路由、边界测试、管理端点、阶段三的静态安全模式，以及阶段四的完整错误响应切换、秒数/日期 `Retry-After` 限流、普通故障的有界退避、每模型独立上游池和 `SIGHUP` 原子摘除。`app/tests/model_pools_e2e.py` 验证跨模型隔离，`app/tests/hot_reload_e2e.py` 验证并发摘除与非法变更回退。修改上游地址、密钥或用户仍须重启；跨 Provider 幂等、持久化审计与额度、可靠账单尚未完成。TLS 已通过独立构建与测试，仓库也提供 Docker Compose 和 systemd 样例，但当前环境没有 Docker，容器启动与故障切换演示尚未实测；详情见[可靠性验证](reliability.md)和[部署说明](deployment.md)。Redis 共享状态尚未实现。
