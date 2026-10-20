#!/usr/bin/env python3
"""AI-llm-transplot 对 llmbridge 的扩展（Apache-2.0）：完整错误响应的故障注入测试。"""

from __future__ import annotations

import http.client
import http.server
import email.utils
import json
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
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class StatusRetry(unittest.TestCase):
    binary: Path

    def mock(self, status: int, retry_after: int | str = 0,
             truncate: bool = False, no_response: bool = False
             ) -> tuple[int, list[dict[str, object]]]:
        received: list[dict[str, object]] = []

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self) -> None:  # noqa: N802
                body = self.rfile.read(int(self.headers["Content-Length"]))
                received.append(json.loads(body))
                if no_response:
                    self.close_connection = True
                    return
                payload = json.dumps({"source": "backup" if status == 200 else "primary"}).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload) + (10 if truncate else 0)))
                if retry_after:
                    self.send_header("Retry-After", str(retry_after))
                self.end_headers()
                self.wfile.write(payload)
                if truncate:
                    self.wfile.flush()
                    self.close_connection = True

            def log_message(self, _format: str, *_args: object) -> None:
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def cleanup() -> None:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

        self.addCleanup(cleanup)
        return int(server.server_port), received

    def gateway(self, primary: int, backup: int, *, allow_unsafe: bool = True) -> int:
        port = free_port()
        config = {
            "security": {"legacy_mode": True},
            "listen": {"port": port, "tls": False},
            "upstream": [
                {"url": f"127.0.0.1:{p}", "dialect": "openai", "credential_header": key}
                for p, key in ((primary, "x-kottos-key-primary"),
                               (backup, "x-kottos-key-backup"))
            ],
            "routing": {"mode": "first", "max_attempts": 2,
                        "allow_unsafe_replay": allow_unsafe,
                        "failure_threshold": 1, "cooldown_s": 0},
            "timeouts": {"connect_s": 1, "upstream_s": 3},
            "runtime": {"io": "epoll", "workers": 1, "log_level": "error"},
        }
        directory = tempfile.TemporaryDirectory(prefix="llmbridge-status-")
        self.addCleanup(directory.cleanup)
        config_file = Path(directory.name) / "config.json"
        config_file.write_text(json.dumps(config), encoding="utf-8")
        process = subprocess.Popen([str(self.binary), "--config", str(config_file)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        def cleanup() -> None:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            if process.stderr:
                process.stderr.close()

        self.addCleanup(cleanup)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"网关启动失败: {process.stderr.read()!r}")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                    return port
            except OSError:
                time.sleep(0.02)
        raise TimeoutError("网关启动超时")

    def request(self, port: int) -> tuple[int, dict[str, object]]:
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        try:
            connection.request("POST", "/v1/chat/completions",
                               body=json.dumps({"model": "chat-default", "messages": [
                                   {"role": "user", "content": "ping"}]}),
                               headers={"Content-Type": "application/json",
                                        "X-Kottos-Key-Primary": "first-secret",
                                        "X-Kottos-Key-Backup": "second-secret"})
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def test_429_switches_and_retry_after_blocks_new_requests(self) -> None:
        primary, first = self.mock(429, retry_after=30)
        backup, second = self.mock(200)
        port = self.gateway(primary, backup)
        self.assertEqual(self.request(port), (200, {"source": "backup"}))
        self.assertEqual(self.request(port), (200, {"source": "backup"}))
        self.assertEqual(len(first), 1)
        self.assertEqual(len(second), 2)

    def test_http_date_retry_after_blocks_new_requests(self) -> None:
        future = email.utils.formatdate(time.time() + 60, usegmt=True)
        primary, first = self.mock(429, retry_after=future)
        backup, second = self.mock(200)
        port = self.gateway(primary, backup)
        self.assertEqual(self.request(port), (200, {"source": "backup"}))
        self.assertEqual(self.request(port), (200, {"source": "backup"}))
        self.assertEqual((len(first), len(second)), (1, 2))

    def test_503_switches_once_and_exhaustion_relays_error(self) -> None:
        primary, first = self.mock(503)
        backup, second = self.mock(503)
        port = self.gateway(primary, backup)
        self.assertEqual(self.request(port), (503, {"source": "primary"}))
        self.assertEqual(len(first), 1)
        self.assertEqual(len(second), 1)

    def test_default_safe_mode_relays_error_without_replaying_post(self) -> None:
        primary, first = self.mock(503)
        backup, second = self.mock(200)
        port = self.gateway(primary, backup, allow_unsafe=False)
        self.assertEqual(self.request(port), (503, {"source": "primary"}))
        self.assertEqual((len(first), len(second)), (1, 0))

    def test_default_safe_mode_does_not_replay_post_after_silent_disconnect(self) -> None:
        primary, first = self.mock(200, no_response=True)
        backup, second = self.mock(200)
        port = self.gateway(primary, backup, allow_unsafe=False)
        self.assertEqual(self.request(port)[0], 502)
        self.assertEqual((len(first), len(second)), (1, 0))

    def test_408_and_500_switch_to_backup(self) -> None:
        for status in (408, 500):
            with self.subTest(status=status):
                primary, first = self.mock(status)
                backup, second = self.mock(200)
                port = self.gateway(primary, backup)
                self.assertEqual(self.request(port), (200, {"source": "backup"}))
                self.assertEqual((len(first), len(second)), (1, 1))

    def test_truncated_error_is_not_replayed(self) -> None:
        primary, first = self.mock(503, truncate=True)
        backup, second = self.mock(200)
        port = self.gateway(primary, backup)
        self.assertEqual(self.request(port)[0], 502)
        self.assertEqual(len(first), 1)
        self.assertEqual(len(second), 0)


if __name__ == "__main__":
    StatusRetry.binary = Path(sys.argv.pop(1)).resolve()
    unittest.main()
