#!/usr/bin/env python3
"""真实进程测试：SSE 故障边界、密钥头校验和全部上游失败。"""

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
import unittest
from pathlib import Path


def free_port() -> int:
    """获取测试用回环端口。"""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def event_chunk(label: str) -> bytes:
    """生成一个完整的 OpenAI SSE 数据块。"""
    event = json.dumps(
        {
            "id": label,
            "object": "chat.completion.chunk",
            "choices": [
                {"index": 0, "delta": {"content": label}, "finish_reason": None}
            ],
        }
    ).encode()
    data = b"data: " + event + b"\n\n"
    return f"{len(data):x}\r\n".encode() + data + b"\r\n"


class GatewayBoundaries(unittest.TestCase):
    """每个场景独立启动网关和 Mock，避免健康状态互相影响。"""

    gateway_binary: Path

    def start_mock(
        self, label: str, *, abort_after_first_event: threading.Event | None = None
    ) -> tuple[int, list[dict[str, object]]]:
        requests: list[dict[str, object]] = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self) -> None:  # noqa: N802
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length)
                requests.append(
                    {
                        "authorization": self.headers.get("Authorization", ""),
                        "body": json.loads(body),
                    }
                )
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                self.wfile.write(event_chunk(label))
                self.wfile.flush()
                if abort_after_first_event is not None:
                    # 等客户端确实收到第一个事件，再中断上游连接。
                    abort_after_first_event.wait(timeout=3)
                    self.close_connection = True
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return
                done = b"data: [DONE]\n\n"
                self.wfile.write(f"{len(done):x}\r\n".encode() + done + b"\r\n0\r\n\r\n")
                self.wfile.flush()

            def log_message(self, _format: str, *_args: object) -> None:
                """测试不输出 Mock HTTP 访问日志。"""

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def stop_mock() -> None:
            if abort_after_first_event is not None:
                abort_after_first_event.set()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

        self.addCleanup(stop_mock)
        return int(server.server_port), requests

    def start_gateway(self, primary_port: int, backup_port: int) -> int:
        gateway_port = free_port()
        config = {
            "security": {"legacy_mode": True},
            "listen": {"port": gateway_port, "tls": False},
            "upstream": [
                {
                    "url": f"127.0.0.1:{primary_port}",
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
            },
            "timeouts": {"connect_s": 1, "upstream_s": 3},
            "runtime": {"io": "epoll", "workers": 1, "log_level": "error"},
        }
        temp = tempfile.TemporaryDirectory(prefix="llmbridge-boundaries-")
        self.addCleanup(temp.cleanup)
        config_path = Path(temp.name) / "config.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        process = subprocess.Popen(
            [str(self.gateway_binary), "--config", str(config_path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            env={**os.environ, "LC_ALL": "C"},
        )

        def stop_gateway() -> None:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            if process.stderr is not None:
                process.stderr.close()

        self.addCleanup(stop_gateway)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if process.poll() is not None:
                stderr = process.stderr.read().decode(errors="replace") if process.stderr else ""
                self.fail(f"网关启动失败: {stderr}")
            try:
                with socket.create_connection(("127.0.0.1", gateway_port), timeout=0.1):
                    return gateway_port
            except OSError:
                time.sleep(0.02)
        self.fail("等待网关监听超时")

    def request(
        self, port: int, *, stream: bool = False, duplicate_primary_key: bool = False
    ) -> tuple[http.client.HTTPConnection, http.client.HTTPResponse]:
        body = json.dumps(
            {
                "model": "chat-default",
                "stream": stream,
                "messages": [{"role": "user", "content": "ping"}],
            }
        ).encode()
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        connection.putrequest("POST", "/v1/chat/completions")
        connection.putheader("Content-Type", "application/json")
        connection.putheader("Content-Length", str(len(body)))
        if duplicate_primary_key:
            connection.putheader("X-Kottos-Key-Primary", "first-secret")
            connection.putheader("X-Kottos-Key-Primary", "second-secret")
        else:
            connection.putheader("X-Kottos-Key-Primary", "primary-secret")
        connection.putheader("X-Kottos-Key-Backup", "backup-secret")
        connection.endheaders(body)
        self.addCleanup(connection.close)
        return connection, connection.getresponse()

    def test_sse_retries_before_any_upstream_response(self) -> None:
        backup_port, backup_requests = self.start_mock("backup")
        port = self.start_gateway(free_port(), backup_port)
        _, response = self.request(port, stream=True)
        payload = response.read()
        self.assertEqual(response.status, 200, payload)
        self.assertIn(b"backup", payload)
        self.assertIn(b"data: [DONE]", payload)
        self.assertEqual(len(backup_requests), 1)
        self.assertEqual(backup_requests[0]["authorization"], "Bearer backup-secret")
        self.assertEqual(backup_requests[0]["body"]["model"], "backup-model")

    def test_sse_does_not_retry_after_client_receives_event(self) -> None:
        release_primary = threading.Event()
        primary_port, primary_requests = self.start_mock(
            "primary", abort_after_first_event=release_primary
        )
        backup_port, backup_requests = self.start_mock("backup")
        port = self.start_gateway(primary_port, backup_port)
        _, response = self.request(port, stream=True)
        try:
            first_line = response.readline()
            self.assertEqual(response.status, 200)
            self.assertIn(b"primary", first_line)
        finally:
            release_primary.set()
        try:
            remainder = response.read()
        except http.client.IncompleteRead as exc:
            remainder = exc.partial
        self.assertNotIn(b"backup", first_line + remainder)
        self.assertNotIn(b"data: [DONE]", first_line + remainder)
        self.assertEqual(len(primary_requests), 1)
        self.assertEqual(backup_requests, [])

    def test_missing_or_duplicate_selected_key_is_rejected(self) -> None:
        backup_port, backup_requests = self.start_mock("backup")
        port = self.start_gateway(free_port(), backup_port)
        for duplicate in (False, True):
            with self.subTest(duplicate=duplicate):
                if duplicate:
                    _, response = self.request(port, duplicate_primary_key=True)
                else:
                    body = b'{"model":"chat-default","messages":[]}'
                    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                    self.addCleanup(connection.close)
                    connection.request(
                        "POST", "/v1/chat/completions", body=body,
                        headers={"Content-Type": "application/json", "X-Kottos-Key-Backup": "backup-secret"},
                    )
                    response = connection.getresponse()
                payload = response.read()
                self.assertEqual(response.status, 400, payload)
        self.assertEqual(backup_requests, [])

    def test_all_upstreams_unavailable_returns_failure(self) -> None:
        port = self.start_gateway(free_port(), free_port())
        _, response = self.request(port)
        payload = response.read()
        self.assertGreaterEqual(response.status, 500, payload)
        self.assertLess(response.status, 600, payload)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("用法: multi_upstream_boundaries.py <llmbridge-binary>")
    GatewayBoundaries.gateway_binary = Path(sys.argv.pop()).resolve()
    unittest.main()
