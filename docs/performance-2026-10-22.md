# 2026-10-22 阶段五性能与故障验收

本文只记录 AI-llm-transplot 当前分支在本机的可复现实测，不引用上游历史数字作为本分支结论。测试均使用本地确定性 Mock，不调用公网 Provider。

## 固定环境

- WSL2 Linux 6.18.33.2，x86_64，8 个可见 CPU。
- Intel Core Ultra 5 225H；本轮 WSL 可见频率约 3686 MHz，无法读取温度和 C-state。
- GCC 13.3.0、CMake 3.28.3，`Release`、`LLMBRIDGE_WERROR=ON`、TLS 关闭。
- 数据面 1 个 worker；非流式与饱和测试使用 `epoll`，SSE 同时测 `epoll` 与 `io_uring`。
- CPU 是网关进程在测量窗口的 user+system 时间；RSS 是 Linux `VmHWM`。所有结果只有单次轮次，不给出跨机器泛化结论。

## 非流式延迟

命令：

```bash
BIN=build/bin bash bench/run_bench.sh 10 8 2 100 1000 5000
```

Python Mock 固定延迟 10 ms。完成数与错误数格式为 `completed/errors`；added 是代理路径与直连路径的客户端分位数之差。

| 目标 RPS | added p50 | added p99 | 网关自测 p99 | 代理完成/错误 | 网关 CPU | 峰值 RSS |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | 0.360 ms | 0.070 ms | 0.124 ms | 800/0 | 0.13 s | 20,784 KiB |
| 1,000 | 0.100 ms | 0.140 ms | 0.084 ms | 7,999/0 | 0.53 s | 32,184 KiB |
| 5,000 | 0.110 ms | 0.250 ms | 0.107 ms | 40,001/0 | 1.77 s | 47,872 KiB |

直连和代理的开放环请求数可有一到数个边界差异，取决于持续时间截止点；错误计数均为零。低负载下两个独立样本的分位数相减会受调度噪声影响，因此 100 RPS 的 p50 与 p99 差值不能解释为单调关系。

## 单线程吞吐

命令：

```bash
BIN=build/bin IO=epoll BACKENDS=4 bash bench/saturate.sh 5 2 \
  10000 30000 50000 70000
```

四个 `fastbackend` 共享端口，响应无人工延迟，请求体为 64 B。

| 目标 RPS | 实现 RPS | p50 | p99 | 完成/错误 | 网关 CPU | 峰值 RSS |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 10,000 | 10,000 | 130 µs | 620 µs | 50,001/0 | 1.89 s | 32,840 KiB |
| 30,000 | 30,000 | 110 µs | 1,110 µs | 149,998/0 | 3.53 s | 36,700 KiB |
| 50,000 | 50,002 | 150 µs | 4,060 µs | 250,011/0 | 4.97 s | 36,912 KiB |
| 70,000 | 61,021 | 608,140 µs | 878,001 µs | 305,106/0 | 6.95 s | 40,324 KiB |

直连后端在 70,000 RPS 仍达到 70,004 RPS，而代理只达到 61,021 RPS 且排队尾延迟突增；因此本环境、该短测参数下的单 worker 拐点位于 50k–70k RPS 之间，不能把 70k 目标值写成吞吐能力。

## SSE 并发、失败率与资源

命令：

```bash
BIN=build/bin bash bench/run_stream_cpu.sh 3 1
BIN=build/bin RUN_PHASE1=0 PHASE2_LEVELS='1024 4096 8192' \
  bash bench/run_stream_cpu.sh 8 2
```

第一轮为每流 50 token/s，第二轮为每流 10 token/s。下表选取可说明拐点的网关数据；所有列出的客户端失败数均为零。

| 后端 | 并发流 | 每流速率 | 交付比例 | chunk p50/p99 | 完整流/失败 | 网关 CPU | 峰值 RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| epoll | 1,024 | 50 tok/s | 98.9% | 228/953 µs | 2,048/0 | 1.26 s | 27,756 KiB |
| io_uring | 1,024 | 50 tok/s | 98.9% | 236/982 µs | 2,048/0 | 1.05 s | 44,772 KiB |
| epoll | 4,096 | 50 tok/s | 76.2% | 15,554/37,995 µs | 7,098/0 | 3.03 s | 90,548 KiB |
| io_uring | 4,096 | 50 tok/s | 84.5% | 16,084/48,443 µs | 7,489/0 | 3.02 s | 103,320 KiB |
| epoll | 4,096 | 10 tok/s | 98.8% | 237/9,502 µs | 4,096/0 | 3.44 s | 80,636 KiB |
| io_uring | 4,096 | 10 tok/s | 98.9% | 257/6,481 µs | 4,096/0 | 2.73 s | 94,852 KiB |
| epoll | 8,192 | 10 tok/s | 91.5% | 518/800,106 µs | 8,192/0 | 6.21 s | 153,584 KiB |
| io_uring | 8,192 | 10 tok/s | 94.0% | 411/793,114 µs | 8,192/0 | 5.03 s | 163,368 KiB |

8,192 流时直连 Mock 本身也只有 90.9% 交付率且 p99 超过 800 ms，因此该档不能单独归因于网关。结果显示 4,096 流、10 tok/s 是本轮更可信的容量点；高并发需要独立后端主机和更长轮次复核。

## 故障注入

以下真实进程用例各连续运行 5 次，总计 20 次均通过：不可达主上游切换、`408`/`429`/`5xx` 与半截响应、并发 `SIGHUP` 摘除、正常重启账本重放及 SIGKILL 后保守关闭。

```bash
ctest --test-dir build \
  -R '^App(MultiUpstreamFailover|StatusRetry|HotReload|SecureUsers)$' \
  --repeat until-fail:5 --output-on-failure
```

总墙钟时间 109.36 秒，20 次调用无失败。这是确定性故障正确性门禁，不是随机故障率估计；Docker Compose 的 TLS/停主 Mock 演示由 `deploy/smoke_test.sh` 在 CI 另行验证。
