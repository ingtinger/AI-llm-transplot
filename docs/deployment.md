# 单机部署与双 Mock 演示

本文是 AI-llm-transplot 分支新增的部署说明，遵循 Apache-2.0 许可证。

本文的 Docker Compose 只把 TLS 数据端口映射到宿主机 `127.0.0.1:8088`；两个 Mock 上游与网关共享容器网络命名空间，分别监听其中的回环端口 9001 和 9002。管理端口 9091 也只在该命名空间的回环地址可访问。演示密钥不是生产凭据。

## Docker Compose 演示

需要 Docker Engine 和 Compose 插件。在 `deploy/` 目录下执行：

```bash
cp .env.example .env
id -u
id -g
```

把输出的 UID/GID 填入 `.env` 的 `GATEWAY_UID` / `GATEWAY_GID`；可以保留其余三个演示值。随后生成仅用于本机的短期证书：

```bash
mkdir -p certs
chmod 600 .env
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj '/CN=localhost' \
  -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' \
  -keyout certs/key.pem -out certs/cert.pem
chmod 600 certs/key.pem
docker compose --env-file .env up --build -d
```

首次启动时两个 Mock 可能在网关之后数秒内才就绪。用网关虚拟 Key 发请求，再停止主 Mock 验证备用上游：

```bash
demo_key=demo-gateway-key-change-me
curl --cacert certs/cert.pem https://127.0.0.1:8088/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $demo_key" \
  -d '{"model":"chat-default","messages":[{"role":"user","content":"ping"}]}'
docker compose stop mock-primary
curl --cacert certs/cert.pem https://127.0.0.1:8088/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $demo_key" \
  -d '{"model":"chat-default","messages":[{"role":"user","content":"ping"}]}'
docker compose start mock-primary
```

正常响应包含 Mock 的 `pong`。停止主 Mock 后，如果下一次请求恰好复用了已关闭的连接，网关可能先返回错误，因为默认不重放已发送的请求；再发送一个新请求会在主上游冷却时选用备用上游。查看管理状态可在容器内请求回环端口；不要额外映射 9091：

```bash
docker compose exec gateway bash -c 'exec 3<>/dev/tcp/127.0.0.1/9091; printf "GET /ready HTTP/1.0\r\n\r\n" >&3; IFS= read -r line <&3; printf "%s\n" "$line"'
docker compose logs --tail=30 gateway
docker compose down
```

镜像使用 OpenSSL 3 构建、非 root 用户运行，携带 `LICENSE` 和本分支的 `NOTICE`，并以只读方式挂载配置和证书。`.env`、`certs/` 和本地 `private/` 目录被 Docker 构建上下文排除；前两者也不会进入 Git。Compose 的 JSON 日志限制为每文件 10 MB、最多 3 个文件。停止时先向进程发送 `SIGTERM`，30 秒后容器运行时可能强制结束；网关当前没有跨重启保留在途请求的机制。此 Compose 文件只用于本机演示，不应直接映射公网端口或把演示密钥用于真实 Provider。

## systemd 单机示例

使用 `LLMBRIDGE_TLS=ON` 构建后，安装网关二进制，创建只用于运行服务的账号，并将 [服务单元](../deploy/ai-llm-transplot.service) 安装为 `/etc/systemd/system/ai-llm-transplot.service`。将 [安全配置样例](../app/ai-gateway.example.json) 复制到 `/etc/ai-llm-transplot/config.json`，修改 Provider 地址、模型、证书路径及端口。配置文件只写环境变量名；真实凭据放在 `/etc/ai-llm-transplot/credentials.env`，格式为 `PROVIDER_PRIMARY_KEY=...` 等，权限设为 root 可读的 `0600`。TLS 私钥应仅由 `ai-gateway` 用户读取。

```bash
sudo useradd --system --user-group --no-create-home --shell /usr/sbin/nologin ai-gateway
sudo install -d -m 0750 -o root -g ai-gateway /etc/ai-llm-transplot
sudo install -m 0644 deploy/ai-llm-transplot.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ai-llm-transplot
sudo systemctl status ai-llm-transplot
sudo systemctl reload ai-llm-transplot
```

执行 `enable --now` 前必须先准备配置、凭据和证书。服务单元以 `SIGTERM` 停止、以 `SIGHUP` 仅热更新 `routing.drained_upstreams`；修改其他配置或轮换环境变量中的密钥须重启。标准输出和错误进入 journald；在宿主机的 journald 配置中设定容量上限与保留策略，避免日志无限增长。数据端口需由 TLS 保护，并配合防火墙或反向代理限制入口；反向代理不应转发仅监听 `127.0.0.1` 的管理端口。

这些示例不提供持久化额度、完整审计或跨实例共享状态，相关限制见 [用量与审计边界](accounting.md)。
