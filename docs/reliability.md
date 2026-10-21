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
cmake -S . -B build-tsan-current -DCMAKE_BUILD_TYPE=Debug \
  -DLLMBRIDGE_WERROR=ON -DLLMBRIDGE_TLS=OFF -DLLMBRIDGE_BUILD_TESTS=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=thread' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=thread' \
  -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST
cmake --build build-tsan-current -j3 --target app_options_test llmbridge
TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R \
  build-tsan-current/bin/app_options_test --gtest_filter='RoutingPolicy.*'
TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R \
  python3 app/tests/hot_reload_e2e.py build-tsan-current/bin/llmbridge
TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R \
  python3 app/tests/secure_users_e2e.py build-tsan-current/bin/llmbridge
```

TSan 要单独构建，不能与 ASan 同时链接。**本机 WSL2** 下直接启动 GCC TSan 程序会在进入测试前报 `FATAL: ThreadSanitizer: unexpected memory mapping`；`setarch x86_64 -R` 才能启动，因此这里将 GoogleTest 发现改为 `PRE_TEST`，并直接在该包装下运行测试。在原生 Linux/CI 上应再用通常的 CTest 方式验证，不依赖 WSL 特例。

**2026-10-21 实测：**新建且指向当前工作树的 `build-tsan-current` 后，`RoutingPolicy.*` **23/23 通过**，真实进程 `hot_reload_e2e.py` 与 `secure_users_e2e.py` 各 **1/1 通过**，未报告数据竞争。这覆盖热加载、共享路由状态、用户并发与额度路径，不是全项目 TSan 验证。

## Fuzz

CI 使用 Clang/libFuzzer 配合 ASan/UBSan，分别覆盖 JSON、HTTP、HTTP 差分、TLS 握手、用量扫描、配置、流式请求体和 SSE。上游 URL 解析器另有 `fuzz_upstream`：对任意字节验证拒绝必有原因，接受结果满足主机、端口和路径不变量，并能规范化后往返解析；种子包含路径与查询、旧式地址、空端口、userinfo 混淆和点段。

**2026-10-21 检查：**当前环境没有 Clang/libFuzzer，不能声称已在本机执行随机 Fuzz；新目标及解析实现通过 GCC 13 的 C++20/Werror 编译，五个提交种子的确定性冒烟通过。GitHub Actions 运行 `39561489092` 随后以 4096 字节输入上限执行该目标 60 秒，步骤 **1/1 通过**，没有 ASan/UBSan 诊断。

## 验收边界

- `LLMBRIDGE_WERROR=ON` 构建通过只说明当前编译器没有警告，不说明逻辑正确。
- TLS 测试使用本地证书与 Mock，仍需在部署环境验证证书来源、有效期、权限和轮换。
- 单机内存限额不提供多实例精确计费；显式开启不安全重放可能重复执行上游请求。
- Docker/Compose、systemd、压力基准和生产网络限制属于阶段五后续任务。

## 2026-10-20 续开发验证

修复管理端点的请求头长度判定和慢速请求头占用问题后，普通 Release / `LLMBRIDGE_WERROR=ON` 构建通过，完整 CTest **2216/2216 通过**。`AppManagement` 新增短请求头与大请求体同批到达、慢速请求头两种真实进程场景。独立的 `LLMBRIDGE_TLS=ON` / `LLMBRIDGE_BUILD_TESTS=OFF` / `LLMBRIDGE_WERROR=ON` 构建也通过，网关二进制可执行 `--help`。

新增的 Docker Compose、镜像与 systemd 样例已完成 JSON/YAML 语法及配置解析检查；本机无 Docker，未执行容器启动或故障切换演示。systemd 静态检查只提示示例路径 `/usr/local/bin/llmbridge` 尚未在本机安装，部署时须先安装二进制、配置与证书。

旧基准脚本在网关默认要求显式选择安全模式后无法启动进程；本轮为仅连接本地 Mock 的脚本加上 `--legacy-mode`，并让主延迟及饱和度脚本只停止自己启动的子进程。以下低负载命令用于冒烟验证，不作为性能结论：

兼容模式会监听所有网卡，运行主机基准时应使用隔离网络或防火墙。ENTERPILOT Compose 中的兼容模式端口已限制在宿主机 `127.0.0.1`。

```bash
bash bench/run_bench.sh 10 2 1 10
IO=epoll bash bench/saturate.sh 2 1 100
```

主延迟基准实际完成直连 **20/0**、代理 **20/0**（完成数/错误数）；饱和度基准在目标 100 RPS 时直连与代理均达到约 100 RPS。脚本现在将每轮完成数和错误数写入结果摘要。固定硬件下的长期吞吐、P50/P99、SSE 并发与 CPU/RSS 记录仍未验收。

管理请求解析改动还以独立 Debug ASan/UBSan 构建验证了 `AppManagement` 与演示配置解析测试，**2/2 通过**，未报告消毒器错误。此覆盖限于管理端点和配置样例，不代表全项目消毒器验收。

LiteLLM 对比脚本现在只清理由自身启动的进程，并在依赖缺失、服务未就绪或没有有效样本时停止测量；结果摘要会写出每条路径的完成数和错误数。本机未安装用于对比的 LiteLLM 虚拟环境，因此只验证了脚本语法、缺少依赖时的拒绝路径，以及另一个已运行 Mock 进程未被误杀；未生成新的 LiteLLM 性能数据。

基准脚本进一步拒绝缺失分位延迟或网关自身直方图的数据，避免将空值写成零延迟。以 `BIN=build/bin bash bench/run_bench.sh 0 1 0 20` 进行短时冒烟，直连与代理均完成 **20/0**（完成数/错误数）；这不是正式性能测量。LiteLLM 路径仍因本机缺少对应虚拟环境而未实测。

饱和度脚本也会拒绝缺失样本，并记录直连/代理完成数与错误数。`BIN=build/bin IO=epoll bash bench/saturate.sh 1 0 20` 的短时冒烟两条路径均为 **20/0**；零时长输入被正确拒绝。调试追踪脚本不再按命令行模糊杀进程，`--provider none` 映射到网关现用的 `openai` 方言，`none` 与 `anthropic` 两种本机往返冒烟均得到 HTTP 200；这些不是公网 Provider 或正式性能验收。

LiteLLM 重测入口现在为每次运行创建独立虚拟环境并记录路径，不会删除旧环境；任一对比轮次失败都会停止，不能输出完整重测结论。由于本机未安装 LiteLLM 且未执行网络安装，此入口仅完成脚本语法和控制流检查；运行后保留的环境可由操作者复核并自行清理。

SSE 对比脚本同样先检查 LiteLLM 环境和本机进程，再要求每条路径有测量后的分块、完成的流且零失败；直方图溢出或缺失统计会停止测量，正式 CSV 只在整轮成功后替换。以独立 `faststream`/`streamgen` 运行 1 秒验证输出字段解析（801 个测量分块、160 条完成流、零失败）；由于 LiteLLM 环境缺失，未运行完整 SSE 对比，也未产生新的可发布延迟结论。

`Retry-After` 增加年份 `9999` 边界回归测试：旧实现返回 0，新实现先把当前时间降到秒精度再求差，按契约截断到 65535 秒，避免纳秒范围溢出。此前还增加了 IMF-fixdate 的星期一致性测试；`Release`/`LLMBRIDGE_WERROR=ON` 构建与全量 CTest 通过，定向 ASan/UBSan 测试 **1/1 通过**，没有消毒器诊断。

配置解析与文件加载现在统一限制为 4 MiB，文件以有界分块读取；新增测试用 `/dev/zero` 验证持续可读输入会及时被拒绝，而演示配置仍能解析。`Release`/`LLMBRIDGE_WERROR=ON` 构建与全量 CTest 通过，配置上限定向 ASan/UBSan 测试 **1/1 通过**，没有消毒器诊断。

当前工作树的独立 TLS/Werror 测试构建还对日期边界与配置上限运行了定向 CTest，**2/2 通过**。该构建复用本机保存的 OpenSSL 开发头文件和已下载的 GoogleTest；没有执行新的 TLS 全量测试或容器实测。

被拒绝的上游 URL 与非法 CLI 值不再回显原文；错误仍指出字段、上游索引和拒绝原因。新增单元测试与 `AppSecureUsers` 真实进程测试用假凭据标记检查启动输出，定向 **3/3 通过**。`Release`/Werror 构建与全量 CTest、独立 TLS/Werror 二进制构建均通过。这验证的是启动拒绝路径，不替代对所有运行时日志的保密审查。

同一保密约束扩展到未知 JSON 字段名：仍然拒绝启动，并报告配置组或 `security.users`，但不回显该字段的原文。配置单元测试和 `AppSecureUsers` 真实进程测试使用假凭据字段名检查这一点，定向 **2/2 通过**；相关的旧配置拒绝断言调整后 **28/28 通过**，`Release`/Werror 全量 CTest 及独立 TLS/Werror 定向测试 **2/2 通过**。

配置文件路径在“不可读、为空、超限、解析失败”错误中也不再回显；操作者仍会看到具体拒绝原因。`Config.UnreadablePathIsNotEchoed` 与真实进程 `AppSecureUsers` 用假凭据文件名验证定向 **2/2 通过**；`Release`/Werror 全量 CTest 和独立 TLS/Werror 定向测试 **2/2 通过**。

上游主机名校验补齐逐标签边界：标签不能以连字符开头或结尾，最长 63 字节；合法的 63 字节标签仍可解析。`UpstreamParse.RejectsMalformedHosts` 在 Release/Werror、ASan/UBSan 和独立 TLS/Werror 构建下各 **1/1 通过**，Release 全量 CTest 通过。该检查在启动解析路径执行，不触及请求热路径。

单个上游 URL 现在限制为 2048 字节，避免超长配置路径进入每次请求的目标拼接。边界单元测试在 Release/Werror、ASan/UBSan 和独立 TLS/Werror 构建下各 **1/1 通过**；Release/Werror 全量构建、全量 CTest、约定及文档数据检查通过。

显式空端口（如 `https://host:`）不再误用默认端口。新增两个回归输入先证明旧解析器会接受它们，再在三种构建下通过 `UpstreamParse.RejectsBadPorts`；Release/Werror 全量 CTest 通过。

`upstream.strip_headers` 现在最多接受 64 个合法 HTTP 字段名，每个最多 64 字节；包含冒号、空格、斜杠、超限或大小写不敏感重复的名称在配置解析时被拒绝，不会复制到各工作线程并在每次请求中重复比较。回归测试先证明旧解析器误接受这些输入，修复后在 Release/Werror、ASan/UBSan 和独立 TLS/Werror 构建下各 **1/1 通过**；Release/Werror 全量 CTest、约定及文档数据检查通过。

`routing.drained_upstreams` 在复制配置前限制为最多 64 项，单个索引文本最多 20 字节，与 64 位摘除掩码一致。回归测试先证明旧解析器接受超长列表和索引文本，修复后与 `strip_headers` 边界测试一起在 ASan/UBSan 和独立 TLS/Werror 构建下各 **2/2 通过**；Release/Werror 全量构建及 CTest、约定和文档数据检查通过。

`routing.sticky_header` 现在在配置解析和策略构造两层统一限制为 1..64 字节，并只接受字母、数字、连字符和下划线，防止接近配置文件上限的名称进入每次请求的头匹配。两个回归测试先证明旧实现会接受非法字符和 65 字节名称，修复后在 Release/Werror、ASan/UBSan 和独立 TLS/Werror 构建下各 **2/2 通过**；Release/Werror 全量 CTest、约定及文档数据检查通过。

安全用户配置现在拒绝重复的用户 `id`、`key_env`、用户模型和许可上游索引；配置解析阶段给出具体错误，策略构造层继续防止程序化调用绕过校验。两个回归测试先证明旧实现会接受四类重复项，修复后在 Release/Werror、ASan/UBSan 和独立 TLS/Werror 构建下各 **2/2 通过**；`AppSecureUsers` 又用四份临时配置验证真实进程会拒绝启动且不回显配置值，定向 **1/1 通过**。Release/Werror 全量 CTest、约定及文档数据检查通过。

HTTP 请求与响应共用的头解析器现在逐字验证字段名，只接受 RFC 9110 `token` 字符；空格、分隔符、DEL、高位字节和内嵌 NUL 不再被转发给语义可能不同的下游解析器。回归测试先证明旧实现接受六类非法名称，同时保留全部合法 `tchar` 的正例；修复后在 Release/Werror、ASan/UBSan 和独立 TLS/Werror 构建下各 **1/1 通过**，Release/Werror 全量 CTest **2229/2229 通过**。约 1 KiB 响应头的本机 Release 性能冒烟为 p50 **925 ns**、p95 **1384 ns**；单次短测仅用于发现数量级回退，不作为稳定性能基线。
