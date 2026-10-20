# AI-llm-transplot 开发规范

## 项目定位

本仓库基于 `kottos-ai/llmbridge`，实现面向多用户、多 AI Provider 的高性能网关服务器。数据面保持轻量、无阻塞和低延迟，用户、额度等控制面能力通过清晰接口逐步加入。

## 技术栈

- 语言：C++20。
- 平台：Linux，推荐 WSL2 Ubuntu 24.04。
- 构建：CMake 3.20 及以上。
- 编译器：GCC 13 及以上或 Clang 16 及以上。
- 网络：原生 `epoll` 和 `io_uring`。
- 测试：GoogleTest、CTest、Python 标准库端到端测试。
- TLS：OpenSSL 3，可通过 `LLMBRIDGE_TLS=ON` 启用。

## 目录说明

```text
app/        可执行程序、配置、内置路由策略
gateway/    HTTP/SSE 代理数据面与 Policy 扩展接口
net/        Socket、HTTP 边界、TLS、io_uring
provider/   OpenAI 与各 Provider 的协议转换
bench/      Mock 后端、压测和性能测量工具
docs/       AI-llm-transplot 中文设计与任务文档
```

## 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLLMBRIDGE_WERROR=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build -j4 --output-on-failure
```

连接公网 HTTPS Provider 时必须增加：

```bash
-DLLMBRIDGE_TLS=ON
```

## 开发要求

- 新增项目文档、README 和项目自有注释使用中文。
- 上游原有英文文档和注释不做机械翻译，避免阻碍同步和审查。
- 修改热路径前先阅读 `DESIGN.md` 和 `GATEWAY-INTERNALS.md`。
- 公共 API 必须说明所有权、生命周期和线程安全性。
- 路由策略会被多个工作线程共享，状态必须使用原子操作或明确同步。
- 不在热路径执行阻塞 I/O、DNS 查询、文件访问或无界内存分配。
- 不在已经向客户端发送 SSE 数据后切换上游。
- 外部输入必须有长度、字符集、类型和范围校验。
- 日志不得输出完整 API Key、Authorization、提示词或回复正文。

## 配置要求

- 未知配置项必须导致启动失败，不能静默忽略。
- 配置文件只保存 Provider 密钥的请求头名称，不保存真实密钥。
- 新增配置项时必须同时更新：
  - `app/options.hpp`
  - `app/options.cpp`
  - `app/llmbridge.example.json`
  - `app/tests/config_test.cpp`
  - `app/tests/cli_test.cpp`，仅命令行选项需要

## 测试要求

- 新功能必须包含单元测试。
- 路由、重试、密钥和模型映射必须包含真实进程端到端测试。
- 提交前使用 `LLMBRIDGE_WERROR=ON` 构建。
- 修改网络状态机后运行全部 CTest，并根据风险补充 ASan、TSan 或 Fuzz 验证。
- Windows 检出必须保持 `.sh` 和 `.py` 为 LF，规则记录在 `.gitattributes`。

## 许可证

保留上游 Apache-2.0 的 `LICENSE`、`NOTICE` 和源文件版权信息。AI-llm-transplot 新增文件需要注明属于本分支的扩展；发布修改版本时同时携带修改后的 `NOTICE`。
