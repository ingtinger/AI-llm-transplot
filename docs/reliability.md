# 阶段五：可靠性验证

这份记录区分“代码能编译”“测试通过”和“可以生产部署”。所有构建都使用独立目录，避免 TLS、sanitizer 与普通 Release 版本互相覆盖。测试中的 Provider 是本地 Mock，不会调用公网 AI，也不会使用真实密钥。

## 环境准备

推荐 WSL2 Ubuntu 24.04、GCC 13+、CMake 3.20+。TLS 构建需要 **OpenSSL 开发包**，仅安装 `openssl` 命令或运行库不够：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libssl-dev python3
```

本轮验证机器没有免密 `sudo`，实际从 Ubuntu 仓库下载与系统 OpenSSL 3.0.13 匹配的 `libssl-dev`，仅解包到 Git 忽略的 `build-wsl-deps/`，并在 CMake 中指定头文件和运行库路径；**没有改动系统安装**。下面的命令是新机器有正常包管理权限时应使用的常规路径。

## TLS 与 HTTPS

```bash
cmake -S . -B build-wsl-tls -DCMAKE_BUILD_TYPE=Release \
  -DLLMBRIDGE_WERROR=ON -DLLMBRIDGE_TLS=ON -DLLMBRIDGE_BUILD_TESTS=ON
cmake --build build-wsl-tls -j4
ctest --test-dir build-wsl-tls -j4 -R 'Tls|TLS|SigV4' --output-on-failure
ctest --test-dir build-wsl-tls -j4 --output-on-failure
```

专项测试覆盖入口 TLS、HTTPS 上游、证书链及主机名拒绝、SSE、上游切换时重新握手和 `epoll`/`io_uring`。这是**本地生成证书的测试**，不代表已验证真实 Provider 的公网可用性、证书轮换或生产代理配置。

**2026-10-20 实测：**WSL2 Ubuntu 24.04、GCC 13.3、OpenSSL 3.0.13，`Release`、`LLMBRIDGE_WERROR=ON`；TLS 专项 **114/114 通过**，TLS 构建全量 CTest **2345/2345 通过**。本机以 Git 忽略的 `build-wsl-deps/` 保存开发包，并为 CMake 指定头文件与系统运行库；普通用户按上面的 `apt-get install libssl-dev` 即可，不必复制本机临时路径。

## ASan 与 UBSan

```bash
cmake -S . -B build-wsl-asan -DCMAKE_BUILD_TYPE=Debug \
  -DLLMBRIDGE_WERROR=ON -DLLMBRIDGE_TLS=OFF -DLLMBRIDGE_BUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-wsl-asan -j3 \
  --target llmbridge app_options_test net_http_test gateway_core_test
ctest --test-dir build-wsl-asan -j2 \
  -R '^App(StatusRetry|HotReload|ModelPools)$|^RoutingPolicy[.]|^Config[.]|^ProviderHeads[.]' \
  --output-on-failure
```

ASan 检查越界、释放后访问等内存错误，UBSan 检查未定义行为；它们不证明并发代码没有数据竞争。

**2026-10-20 实测：**上述四个目标编译通过，所选配置/HTTP 边界/路由/真实进程测试 **48/48 通过**，没有 ASan/UBSan 诊断。此结果不是完整测试集的 sanitizer 覆盖；与 TLS 的组合构建也未运行。

## TSan

```bash
cmake -S . -B build-wsl-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DLLMBRIDGE_WERROR=ON -DLLMBRIDGE_TLS=OFF -DLLMBRIDGE_BUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread' \
  -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST
cmake --build build-wsl-tsan -j3 --target app_options_test llmbridge
TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R \
  build-wsl-tsan/bin/app_options_test --gtest_filter='RoutingPolicy.*'
TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R \
  python3 app/tests/hot_reload_e2e.py build-wsl-tsan/bin/llmbridge
```

TSan 要单独构建，不能与 ASan 同时链接。**本机 WSL2** 下直接启动 GCC TSan 程序会在进入测试前报 `FATAL: ThreadSanitizer: unexpected memory mapping`；`setarch x86_64 -R` 才能启动，因此这里将 GoogleTest 发现改为 `PRE_TEST`，并直接在该包装下运行测试。在原生 Linux/CI 上应再用通常的 CTest 方式验证，不依赖 WSL 特例。

**2026-10-20 实测：**`RoutingPolicy.*` **21/21 通过**，真实进程 `hot_reload_e2e.py` **1/1 通过**，未报告数据竞争。这是选定的并发路径，不是全项目 TSan 验证。

## 验收边界

- `LLMBRIDGE_WERROR=ON` 构建通过只说明当前编译器没有警告，不说明逻辑正确。
- TLS 测试使用本地证书与 Mock，仍需在部署环境验证证书来源、有效期、权限和轮换。
- 单机内存限额不提供多实例精确计费；显式开启不安全重放可能重复执行上游请求。
- Docker/Compose、systemd、压力基准和生产网络限制属于阶段五后续任务。
