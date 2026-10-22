# AI-llm-transplot 架构与请求路径

本文描述本分支的单机部署边界。数据面保持事件驱动；认证、路由、额度和审计通过 `gateway::Policy`、`RequestSink` 等接口接入，不在网络热路径读取文件或访问外部数据库。

```mermaid
flowchart LR
    C[OpenAI 兼容客户端] -->|TLS / HTTP / SSE| W[数据面工作线程<br/>epoll 或 io_uring]
    W --> A[认证、模型权限<br/>速率与并发准入]
    A --> R[共享路由策略<br/>模型池、冷却、摘除]
    R --> P1[Provider 0]
    R --> P2[Provider 1..N]
    P1 --> W
    P2 --> W
    W -->|逐尝试事件，无阻塞入队| Q[有界审计队列]
    Q --> L[专用账本线程<br/>CRC32 JSONL + fdatasync]
    M[回环管理端口<br/>health / ready / metrics] --> R
    M --> Q
    H[SIGHUP 控制路径] -->|仅发布摘除位图| R
```

## 生命周期与所有权

- `main` 启动时完整解析配置、解析 DNS、装载价目表并重放账本；任一严格校验失败都不启动工作线程。
- 每个数据面工作线程拥有自己的连接和事件循环。不可变上游配置在工作线程存活期间保持有效，不跨线程转移 Socket 所有权。
- `RoutingPolicy`、指标和用户额度由工作线程共享；可变字段使用原子操作。`SIGHUP` 只原子发布固定上游表的摘除位图，不替换正在使用的配置对象。
- 请求日志与审计账本各自使用有界队列和专用线程。队列满时不阻塞事件循环；普通请求日志可丢弃并计数，账本失败则让带额度用户 fail-closed。
- 正常停止先停止接纳新连接，再结束工作线程、排空审计队列并持久化 `session_end`。强制退出留下未闭合 session，下次启动会把相关 UTC 周期的额度恢复为 unknown。

## 一次请求的关键边界

```mermaid
sequenceDiagram
    participant Client as 客户端
    participant Worker as 数据面线程
    participant Policy as 认证与路由策略
    participant Provider as Provider
    participant Ledger as 审计队列

    Client->>Worker: 完整 HTTP 请求
    Worker->>Policy: 认证、授权、额度与并发准入
    Policy-->>Worker: 上游索引、模型映射、Provider Key
    Worker->>Provider: 转换并发送请求
    alt 客户端尚未收到字节且故障可重试
        Provider--xWorker: 连接故障或完整错误响应
        Worker->>Ledger: 记录失败尝试
        Worker->>Policy: 选择未尝试的备用上游
        Worker->>Provider: 有界重试
    else 响应已提交或 SSE 已发送
        Provider--xWorker: 中断
        Worker->>Ledger: 记录截断或未知用量
        Worker--xClient: 终止，不拼接另一上游响应
    end
    Provider-->>Worker: 完整响应或 SSE
    Worker->>Ledger: 记录该尝试原始用量与计价版本
    Worker-->>Client: OpenAI 兼容响应
    Worker->>Policy: 释放并发名额并结算额度
```

默认不会重放可能已被 Provider 执行的非幂等请求。只有显式启用 `routing.allow_unsafe_replay`，并且客户端尚未收到响应字节时，完整的 `408`、`429`、`5xx` 才可切换；这不提供跨 Provider 恰好一次语义。

## 部署边界

当前完成口径是单进程、单机保守额度。账本具有单写者锁，不能让多个实例共享一个文件；多个实例也不共享速率、额度、粘性或健康状态。管理端口固定绑定回环地址，生产入口必须启用 TLS，Provider Key 仅由服务端环境变量注入。容器与 systemd 操作见[部署说明](deployment.md)，账务限制见[用量与审计边界](accounting.md)。
