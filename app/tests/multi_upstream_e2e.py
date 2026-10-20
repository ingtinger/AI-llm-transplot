#!/usr/bin/env python3
"""启动真实网关进程，验证多上游故障切换、模型映射和密钥隔离。"""

from __future__ import annotations

import http.client
import http.server
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


def free_port() -> int:
    """申请一个当前空闲的本地端口。"""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class BackupHandler(http.server.BaseHTTPRequestHandler):
    """记录网关发来的请求，并返回最小 OpenAI 兼容响应。"""

    received: dict[str, object] = {}

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        type(self).received = {
            "path": self.path,
            "authorization": self.headers.get("Authorization", ""),
            "primary_header": self.headers.get("X-Kottos-Key-Primary", ""),
            "backup_header": self.headers.get("X-Kottos-Key-Backup", ""),
            "body": json.loads(body),
        }
        payload = json.dumps(
            {
                "id": "backup-response",
                "object": "chat.completion",
                "choices": [
                    {
                        "index": 0,
                        "message": {"role": "assistant", "content": "backup-ok"},
                        "finish_reason": "stop",
                    }
                ],
            }
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, _format: str, *_args: object) -> None:
        """测试不输出 Python HTTP 服务器的访问日志。"""


def wait_until_listening(port: int, process: subprocess.Popen[bytes]) -> None:
    """等待网关监听完成，进程提前退出时立即报告。"""
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            stderr = process.stderr.read().decode(errors="replace") if process.stderr else ""
            raise RuntimeError(f"网关启动失败: {stderr}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.02)
    raise TimeoutError("等待网关监听超时")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("用法: multi_upstream_e2e.py <llmbridge-binary>")

    gateway_binary = Path(sys.argv[1]).resolve()
    gateway_port = free_port()
    dead_port = free_port()
    backup_port = free_port()
    backup = http.server.ThreadingHTTPServer(("127.0.0.1", backup_port), BackupHandler)
    backup_thread = threading.Thread(target=backup.serve_forever, daemon=True)
    backup_thread.start()

    config = {
        "security": {"legacy_mode": True},
        "listen": {"port": gateway_port, "tls": False},
        "upstream": [
            {
                "url": f"127.0.0.1:{dead_port}",
                "dialect": "openai",
                "credential_header": "x-kottos-key-primary",
                "model": "primary-model",
            },
            {
                "url": f"127.0.0.1:{backup_port}",
                "dialect": "openai",
                "credential_header": "x-kottos-key-backup",
                "model": "backup-model",
            },
        ],
        "routing": {
            "mode": "first",
            "max_attempts": 2,
            "failure_threshold": 1,
            "cooldown_s": 30,
            "sticky_header": "x-conversation-id",
        },
        "timeouts": {"connect_s": 1, "upstream_s": 5},
        "runtime": {"io": "epoll", "workers": 1, "log_level": "error"},
    }

    process: subprocess.Popen[bytes] | None = None
    try:
        with tempfile.TemporaryDirectory(prefix="llmbridge-e2e-") as temp_dir:
            config_path = Path(temp_dir) / "config.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")
            process = subprocess.Popen(
                [str(gateway_binary), "--config", str(config_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env={**os.environ, "LC_ALL": "C"},
            )
            wait_until_listening(gateway_port, process)

            body = json.dumps(
                {
                    "model": "chat-default",
                    "messages": [{"role": "user", "content": "ping"}],
                }
            )
            connection = http.client.HTTPConnection("127.0.0.1", gateway_port, timeout=5)
            connection.request(
                "POST",
                "/v1/chat/completions",
                body=body,
                headers={
                    "Content-Type": "application/json",
                    "X-Conversation-Id": "conversation-e2e",
                    "X-Kottos-Key-Primary": "primary-secret",
                    "X-Kottos-Key-Backup": "backup-secret",
                },
            )
            response = connection.getresponse()
            response_body = response.read()
            connection.close()

            assert response.status == 200, (response.status, response_body)
            assert json.loads(response_body)["id"] == "backup-response"
            received = BackupHandler.received
            assert received["authorization"] == "Bearer backup-secret", received
            assert received["primary_header"] == "", received
            assert received["backup_header"] == "", received
            assert received["body"]["model"] == "backup-model", received
    finally:
        backup.shutdown()
        backup.server_close()
        backup_thread.join(timeout=2)
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
