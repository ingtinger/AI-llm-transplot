# AI-llm-transplot 分阶段开发计划

本文是下一步的**实施顺序和验收清单**，不是已实现功能说明。`[x]` 表示已有相应实现和基础测试；`[ ]` 表示尚未完成。每阶段先写测试，再实现，再更新配置示例和中文文档，不要仅凭编译通过就勾选任务。

## 当前起点

工程已基于 `llmbridge` 实现多上游路由、故障切换、阶段二管理端点，以及阶段三的静态安全模式。**无认证兼容模式必须显式开启；额度仅在单进程内存中，不适合直接作为公网多租户计费服务。**

依赖顺序：**阶段一收尾 → 阶段二可观测 → 阶段三多用户最小闭环 → 阶段四可靠性 → 阶段五交付**。优先做出可验证的单机服务，再考虑 Redis、多实例或复杂调度。

## 阶段一：封住当前数据面的边界

**目标：**让现有路由和流式故障行为可预测，为后续功能提供稳定基线。

- [x] 拉取并验证 llmbridge 上游源码。
- [x] 支持上游数组配置、`first`、`round_robin` 和 `sticky` 路由。
- [x] 实现可重试传输故障后的备用上游选择、失败计数和冷却。
- [x] 实现每上游模型映射与专用密钥请求头。
- [x] 完成双 Mock 上游的故障切换端到端测试。
- [x] 新增 SSE 测试：首字节前连接失败可切换；已有数据发给客户端后不得切换或拼接两个回答。
- [x] 覆盖上游均不可用、达到 `max_attempts`、冷却到期探测、缺少/重复密钥头等边界。
- [x] 固定一次全量 CTest 和 `LLMBRIDGE_WERROR=ON` 构建结果，记录环境与命令。

**验收：**本地 Mock 不需要真实 API Key；上述场景均有自动化断言；SSE 失败时客户端只能看到一个上游的输出或明确错误。

**建议改动点：**`app/tests/routing_policy_test.cpp`、`app/tests/multi_upstream_e2e.py`、`gateway/tests/`；仅在测试暴露缺陷时改 `app/routing_policy.cpp` 或 `gateway/src/core/`。

**本轮验收记录（2026-10-18）：**WSL2 Ubuntu 24.04、GCC 13.3.0、CMake 3.28，`Release`、`LLMBRIDGE_WERROR=ON`、`LLMBRIDGE_TLS=OFF`。新增 `app/tests/multi_upstream_boundaries.py`，实际启动网关和 Mock；策略单测补充尝试上限、不可重试失败、全部冷却探测、冷却到期及 `429` 对后续请求的影响。全量 CTest 为 **2189/2189 通过**；新增进程级边界测试连续重复 5 次通过。

```bash
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release \
  -DLLMBRIDGE_WERROR=ON -DLLMBRIDGE_TLS=OFF
cmake --build build-wsl -j4
ctest --test-dir build-wsl -j4 --output-on-failure
ctest --test-dir build-wsl -R '^AppMultiUpstreamBoundaries$' \
  --repeat until-fail:5 --output-on-failure
```

尝试 `LLMBRIDGE_TLS=ON` 时，CMake 因 WSL 缺少 OpenSSL 开发包而未完成配置；本轮只验证了本地 HTTP Mock 路径，**未验证 HTTPS/TLS 构建与连接**。安装 OpenSSL 3 开发包后应重新以独立构建目录验证 TLS。

## 阶段二：让服务器可观察、可管理

**目标：**运维人员能区分“进程活着”“可以接流量”“哪个上游异常”。先明确管理端点的访问边界，避免把指标或内部状态意外暴露公网。

1. [x] 管理端点默认关闭；启用时绑定独立的 `127.0.0.1` 端口，并写明反向代理部署要求。
2. [x] 实现 `GET /health`：报告管理线程可响应，不依赖上游状态。
3. [x] 实现 `GET /ready`：至少一个上游未冷却时返回 200，全部冷却时返回 503；不主动探测外部网络。
4. [x] 实现 `GET /metrics`：Prometheus 文本格式，输出固定上游索引的路由选择次数、传输失败数、响应状态、冷却状态与首字节延迟。
5. [x] 增加进程内请求序号及有界异步结构化请求日志；记录初始/最终上游、重试和最终状态，不记录敏感内容。

**验收：**用 Mock 制造健康、全部失败和恢复三种状态，HTTP 状态码与指标变化正确；并发请求下统计不丢失；管理端点不能借普通公网监听意外开放。

**建议改动点：**`gateway/include/gateway/metrics.hpp`、`gateway/src/metrics.cpp`、`gateway/src/core/`、`app/main.cpp`，并新增管理端点的进程级测试。

**实现位置：**`app/management_server.*`、`app/observability.*`、`app/routing_policy.*` 和 `app/tests/management_e2e.py`。管理端点不复用数据面事件循环；指标中 `route_selections_total` 包含选路后被本地校验拒绝的请求，延迟只采集最终尝试中有有效上游时间戳的请求。请求日志默认关闭，满队列计入丢弃指标。

**本轮验收记录（2026-10-18）：**WSL2 Ubuntu 24.04、GCC 13.3.0、CMake 3.28，`Release`、`LLMBRIDGE_WERROR=ON`、`LLMBRIDGE_TLS=OFF`。`AppManagement` 真实启动两个 Mock 和双工作线程网关，验证仅回环监听、非 GET/过长请求头拒绝、健康/就绪状态转换、冷却到期恢复、并发计数以及日志不泄露测试密钥与提示词。管理测试重复 5 次通过；全量 CTest **2192/2192 通过**。阶段一所述 TLS 依赖缺失仍存在，HTTPS/TLS 路径本轮未验证。

```bash
cmake --build build-wsl -j4
ctest --test-dir build-wsl -R '^AppManagement$' --repeat until-fail:5 --output-on-failure
ctest --test-dir build-wsl -j4 --output-on-failure
```

## 阶段三：完成多用户最小闭环

**目标：**普通用户只持有网关签发的虚拟 API Key，不能看到或选择其他用户的 Provider Key；先做单机、静态配置可验证版本。

1. [x] 设计用户/租户/密钥/模型权限数据结构和配置格式；定义静态创建、撤销、轮换流程，不在仓库提交真实密钥。
2. [x] 在转发前认证网关虚拟 Key，拒绝缺失、无效、过期或已撤销凭证；明确 401/403 等错误语义。
3. [x] 安全模式从环境变量读取 Provider Key 并按选中上游注入，客户端无需 `X-Kottos-Key-*`；旧模式必须显式开启，默认拒绝无认证启动。
4. [x] 限定用户可用模型和上游池；相同 `X-Conversation-Id` 不影响不同用户的授权与选路起点。
5. [x] 实现每用户固定窗口速率限制、并发上限和立即拒绝；429 返回可理解的错误。
6. [ ] 已记录最终请求的 Token/上游费用字段，按 UTC 日/月做单机 Token 配额；**尚需持久化审计与额度、重试全尝试用量核算和可验证费用规则**。

第 6 项的现状、缺失用量处理与可靠账单验收条件见 [用量与审计边界](accounting.md)。

**阶段三当前验收：**`AppSecureUsers` 真实进程双用户测试覆盖无效凭证、模型越权、用户级速率、日/月 Token 用量、授权上游故障切换及服务端密钥不泄露。单元测试覆盖配置严格解析、重复凭证头、过期/禁用、缺失用量、并发名额释放及用户上游白名单。未选安全/兼容模式或两者同时开启会拒绝启动；安全模式默认要求入口 TLS，非回环明文上游会拒绝。认证在完整请求体到达后执行，未认证上传仍需前置连接/上传限额。额度在请求完成后结算，单个请求可能越额；上游缺失用量时配置配额的用户本周期后续请求拒绝；重启清零，多实例不共享。生产上线前仍须完成第 6 项，并验证 TLS 构建。

**续开发验收记录（2026-10-19）：**WSL2 Ubuntu 24.04，`Release`、`LLMBRIDGE_WERROR=ON`、`LLMBRIDGE_TLS=OFF`，全量 CTest **2201/2201 通过**；新增未选择/冲突模式、明文入口和非回环明文上游的启动拒绝测试。两份配置样例和测试配置均通过 JSON 解析。尝试单独配置 `LLMBRIDGE_TLS=ON` 时，CMake 因 WSL 缺少 OpenSSL 3 的开发头文件/库而失败，故本轮仍未验证 TLS 构建与 HTTPS 连接。

```bash
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Release \
  -DLLMBRIDGE_WERROR=ON -DLLMBRIDGE_TLS=OFF
cmake --build build-wsl -j4
ctest --test-dir build-wsl -j4 --output-on-failure
```

**验收：**至少两个测试用户同时请求；无凭证与越权请求被拒绝；一个用户超限不影响另一个用户；日志和响应不泄露服务端 Provider Key；历史消息仍由客户端每轮完整携带，不用会话 ID 代替上下文。

**建议改动点：**以 `gateway/include/gateway/policy.hpp` 为接入契约，控制面实现与数据面分离；新增认证/限流模块及多用户端到端测试。不要在事件循环中同步读配置文件或数据库。

## 阶段四：提高故障处理和调度能力

**目标：**区分“响应尚未发送，可以重试”和“响应已提交，只能终止”；在保证正确性后再优化负载均衡。

1. [x] 在提交客户端响应前处理上游 `408`、`429`、`5xx`，经显式风险开关允许按策略切换并限制总尝试次数；已经发送响应头或 SSE 字节时禁止透明重试。
2. [x] 支持秒数和 IMF-fixdate `Retry-After`（最多采信 300 秒）及普通故障最多 16 倍的有界冷却；限流期禁止探测。内置策略默认不重放上游可能已执行的请求，流式上传、半截响应、已提交 SSE 均拒绝状态码重试。只有显式启用 `routing.allow_unsafe_replay` 才接受跨 Provider 非幂等 POST 可能重复执行/计费的风险；这不是幂等保证。
3. [x] 实现每模型独立上游池，再考虑加权轮询或最低在途请求策略；用基准测试证明新算法的收益与开销。已实现池内独立轮询、用户授权交集和池内故障切换；当前没有性能证据支持改用加权或最低在途策略。
4. [x] 支持配置热加载与上游优雅摘除，并确保已有连接与共享策略状态的生命周期安全。当前明确限定为 `SIGHUP` 热更新 `routing.drained_upstreams`：原子位图发布，旧请求继续完成；其他字段变更拒绝并保留旧配置，新增/删除上游需重启。
5. [x] Redis 准入决策：本阶段仍以单机为目标，尚无真实多实例需求，因此**不接入 Redis**。未来若启用共享额度，认证请求在 Redis 故障时默认拒绝，避免无限额计费；仅存限流计数和必要路由元数据，不存聊天正文。多实例精确限额当前不受支持。

**验收：**覆盖连接拒绝、超时、`429`、`5xx`、半连接、SSE 中断和重试耗尽；任何情况下不产生重复或拼接的客户端回答。新增并发测试验证热加载期间不会访问已销毁配置。

**阶段四当前边界：**只有完整收到上游错误响应、请求原文可重建且客户端尚未收到字节，并且管理员显式接受可能重复执行的风险时，才会调用策略选择备用上游；流式上传、半截错误响应和已提交 SSE 不作状态码重试。启用风险开关后不能把“客户端只收到一个回答”理解为“上游只计费一次”。仅上游摘除列表可不中断更新；Redis 尚未实现，不能用于多实例精确限额。

**阶段四完成口径：**单机故障切换、幂等风险默认拒绝及显式开关、HTTP 日期退避、每模型路由和固定上游表的热摘除均已实现；第 5 项按准入条件决定暂不引入 Redis。完整配置热替换、跨 Provider 恰好一次计费和多实例共享额度不属于本阶段可宣称的能力，部署前仍需独立验证 TLS 与压力/消毒器测试。

**阶段四部分验收记录（2026-10-19）：**WSL2 Ubuntu 24.04、`Release`、`LLMBRIDGE_WERROR=ON`、`LLMBRIDGE_TLS=OFF`。`AppStatusRetry` 真实进程覆盖 408/429/500 切换、503 耗尽、`Retry-After` 与半截错误响应不重放；原有 SSE/传输故障测试继续通过。全量 CTest **2203/2203 通过**，`AppStatusRetry` 连续重复 5 次通过。阶段四尚未整体验收：第 2-5 项未完成，TLS/HTTPS、sanitizer 与热加载并发测试未验证。

**本轮继续开发：**增加 `upstream.models` 客户端模型池，首次选路、池内重试、用户授权交集和每模型轮询游标使用同一配置；旧配置未设置模型池时保持原行为。连续失败冷却以基础值的 1、2、4、8、16 倍递增，且不在热路径等待。`AppModelPools` 使用三个真实 Mock 验证模型隔离、Provider 模型映射与故障切换。`routing_policy_bench` 在 WSL2 Ubuntu 24.04 / Intel Core Ultra 5 225H / Release 单线程运行三轮、每轮 100 万次 `decide()`：未启用模型池为 37.56–41.62 ns/次，两个模型池为 40.19–40.80 ns/次，差值落在测量波动内；这是选路微基准，不代表端到端吞吐提升。模型池的收益首先是正确性隔离。尚未验证加权轮询或最低在途策略相对现有轮询的收益，因此暂不引入。

**本轮验收记录（2026-10-19）：**WSL2 Ubuntu 24.04、`Release`、`LLMBRIDGE_WERROR=ON`、`LLMBRIDGE_TLS=OFF` 构建通过；全量 CTest **2210/2210 通过**，`AppModelPools` 连续重复 5 次通过。单上游只配置模型池时也创建策略，未知模型返回 503；同一请求第三次尝试不会回访起始上游。TLS/HTTPS 与 sanitizer 本轮未验证。阶段四第 2、4、5 项仍未完成，尤其热加载和上游优雅摘除尚无实现。

**阶段四最终验收记录（2026-10-19）：**WSL2 Ubuntu 24.04、`Release`、`LLMBRIDGE_WERROR=ON`、`LLMBRIDGE_TLS=OFF` 构建通过；全量 CTest **2215/2215 通过**。新增 `AppStatusRetry` 验证默认不重放已被上游读取的 POST、显式风险开关下的状态码切换、日期 `Retry-After` 和半截响应拒绝重试；`AppHotReload` 在两个工作线程、并发请求与多次 `SIGHUP` 下验证摘除/恢复、拒绝其他配置变更和进程存活，连续重复 **3 次通过**。TLS/HTTPS、ASan/TSan 及多实例精确额度仍未验证，不应视作生产部署验收。

```bash
./build-wsl/bin/routing_policy_bench
ctest --test-dir build-wsl -R 'RoutingPolicy|AppModelPools' --output-on-failure
ctest --test-dir build-wsl -j4 --output-on-failure
ctest --test-dir build-wsl -R '^AppModelPools$' --repeat until-fail:5 --output-on-failure
```

```bash
cmake --build build-wsl -j4
ctest --test-dir build-wsl -j4 --output-on-failure
ctest --test-dir build-wsl -R '^AppStatusRetry$' \
  --repeat until-fail:5 --output-on-failure
```

## 阶段五：部署、性能与求职展示

**目标：**从“能在开发机运行”走到“别人能复现、能评估”。

**当前进度（2026-10-20）：**已在 WSL2 Ubuntu 24.04 / GCC 13.3 / OpenSSL 3.0.13 的独立 TLS 构建中通过 `LLMBRIDGE_WERROR=ON` 编译、TLS 专项 **114/114** 和全量 CTest **2345/2345**。构建方法与边界记录在 `docs/reliability.md`；这只完成阶段五的 TLS 验证，不等于容器部署、消毒器或性能验收完成。

**可靠性补充验收（2026-10-20）：**独立 Debug ASan/UBSan 构建及选定测试 **48/48 通过**；WSL2 下以 `setarch x86_64 -R` 运行 TSan，路由策略 **21/21** 和真实进程热加载 **1/1** 通过，未报告数据竞争。普通启动 TSan 会因 WSL 内存映射布局在测试前失败，原生 Linux/CI 仍需复测。sanitizer 结果仅覆盖选定路径，不是全量消毒器验收；Fuzz、部署与性能任务保持未完成。

- [ ] 提供可复现的 Dockerfile、Compose 和 systemd 示例；明确 TLS 终止、管理端点绑定、日志轮转、密钥注入和优雅停止方式。
- [ ] 增加 AddressSanitizer、ThreadSanitizer 验证；针对网络解析和配置输入考虑 Fuzz 测试。
- [ ] 建立固定硬件/编译参数/Mock 行为的吞吐、P50/P99、SSE 并发与故障注入基准；同时报告失败率和资源占用，不只报告最高 QPS。
- [ ] 整理架构图、配置示例、演示脚本及“一个上游失效后请求转移”的可复现录像/操作步骤。
- [ ] 在 README 中写清楚上游项目贡献与本分支新增内容，保留 Apache-2.0 的 `LICENSE`、`NOTICE` 和归属信息。

**验收：**新机器按中文 README 能构建、运行测试、启动两个 Mock 上游并重现故障切换；性能数字有测试命令和环境，不能引用上游基准冒充本分支实测。

## 每阶段统一完成标准

- 代码、配置样例、中文文档和相应测试同步更新。
- `LLMBRIDGE_WERROR=ON` 构建通过；受影响测试与全量 CTest 通过。修改网络状态机时补充 sanitizer 或故障注入验证。
- 明确写出尚未覆盖的边界和部署限制；未实现的目标不标记为完成。
- 独立保留每阶段的测试命令、结果和关键设计决策，方便复盘与面试展示。
