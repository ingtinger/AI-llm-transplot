#!/usr/bin/env python3
"""真实进程验证回环管理端口、就绪状态、指标和安全请求日志。"""

from __future__ import annotations

import concurrent.futures
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
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def start_mock() -> tuple[http.server.ThreadingHTTPServer, threading.Thread, dict[str, int]]:
    state = {"status": 500}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self) -> None:  # noqa: N802
            self.rfile.read(int(self.headers.get("Content-Length", "0")))
            status = state["status"]
            payload = json.dumps(
                {"id": "mock-response", "object": "chat.completion", "choices": []}
                if status == 200 else {"error": {"message": "mock failure"}}
            ).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def log_message(self, _format: str, *_args: object) -> None:
            """不输出 Mock 服务日志。"""

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread, state


def get(port: int, path: str) -> tuple[int, str]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        conn.request("GET", path)
        response = conn.getresponse()
        return response.status, response.read().decode()
    finally:
        conn.close()


def post(port: int) -> tuple[int, str]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        body = json.dumps(
            {"model": "chat-default", "messages": [{"role": "user", "content": "private-prompt"}]}
        )
        conn.request(
            "POST", "/v1/chat/completions", body=body,
            headers={
                "Content-Type": "application/json",
                "X-Kottos-Key-Primary": "primary-secret",
                "X-Kottos-Key-Backup": "backup-secret",
            },
        )
        response = conn.getresponse()
        return response.status, response.read().decode()
    finally:
        conn.close()


def metric(body: str, name: str, labels: str = "") -> float:
    prefix = f"{name}{labels} "
    for line in body.splitlines():
        if line.startswith(prefix):
            return float(line[len(prefix):])
    raise AssertionError(f"未找到指标 {prefix!r}")


def main(binary: Path) -> None:
    primary, primary_thread, primary_state = start_mock()
    backup, backup_thread, backup_state = start_mock()
    data_port, admin_port = free_port(), free_port()
    process: subprocess.Popen[bytes] | None = None
    completed = False
    try:
        config = {
            "security": {"legacy_mode": True},
            "listen": {"port": data_port},
            "management": {"port": admin_port},
            "upstream": [
                {"url": f"127.0.0.1:{primary.server_port}", "dialect": "openai",
                 "credential_header": "x-kottos-key-primary"},
                {"url": f"127.0.0.1:{backup.server_port}", "dialect": "openai",
                 "credential_header": "x-kottos-key-backup"},
            ],
            "routing": {"mode": "first", "max_attempts": 1,
                        "failure_threshold": 1, "cooldown_s": 2},
            "runtime": {"io": "epoll", "workers": 2,
                        "log_level": "error", "request_log": True},
        }
        with tempfile.TemporaryDirectory(prefix="llmbridge-management-") as tmp:
            path = Path(tmp) / "config.json"
            path.write_text(json.dumps(config), encoding="utf-8")
            process = subprocess.Popen(
                [str(binary), "--config", str(path)],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                env={**os.environ, "LC_ALL": "C"},
            )
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise AssertionError(f"网关启动失败: {process.stderr.read()!r}")
                try:
                    status, body = get(admin_port, "/health")
                    break
                except OSError:
                    time.sleep(0.02)
            else:
                raise AssertionError("管理端口未启动")
            assert (status, body) == (200, "ok\n")
            assert get(admin_port, "/ready") == (200, "ready\n")
            assert get(admin_port, "/unknown")[0] == 404
            method = http.client.HTTPConnection("127.0.0.1", admin_port, timeout=5)
            try:
                method.request("POST", "/health", body=b"")
                assert method.getresponse().status == 405
            finally:
                method.close()
            with socket.create_connection(("127.0.0.1", admin_port), timeout=5) as oversized:
                oversized.sendall(b"GET /health HTTP/1.1\r\nX-Large: " + b"a" * 2100 + b"\r\n\r\n")
                assert b"431 Request Header Fields Too Large" in oversized.recv(256)
            with socket.socket() as sock:
                sock.settimeout(0.2)
                assert sock.connect_ex(("127.0.0.2", admin_port)) != 0
            assert post(data_port)[0] == 500
            assert get(admin_port, "/ready")[0] == 200
            assert post(data_port)[0] == 500
            assert get(admin_port, "/ready") == (503, "not ready\n")
            status, metrics = get(admin_port, "/metrics")
            assert status == 200
            for index in range(2):
                label = f'{{upstream="{index}"}}'
                assert metric(metrics, "ai_gateway_upstream_route_selections_total", label) == 1
                assert metric(metrics, "ai_gateway_upstream_available", label) == 0
                assert metric(metrics, "ai_gateway_upstream_responses_total",
                              f'{{upstream="{index}",class="5xx"}}') == 1
                assert metric(metrics, "ai_gateway_upstream_response_latency_seconds_count", label) == 1
            assert "secret" not in metrics and "private-prompt" not in metrics

            time.sleep(2.1)
            assert get(admin_port, "/ready") == (200, "ready\n")
            primary_state["status"] = 200
            backup_state["status"] = 200
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                results = list(pool.map(lambda _: post(data_port)[0], range(8)))
            assert results == [200] * 8, results
            _, metrics = get(admin_port, "/metrics")
            assert metric(metrics, "ai_gateway_upstream_responses_total",
                          '{upstream="0",class="2xx"}') >= 8
            assert metric(metrics, "ai_gateway_upstream_response_latency_seconds_sum",
                          '{upstream="0"}') > 0
            assert metric(metrics, "ai_gateway_request_log_dropped_total") == 0
            # 数据端口不应直接处理管理路径。
            assert get(data_port, "/health") != (200, "ok\n")
            completed = True
    finally:
        if process is not None:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
            try:
                _, stderr = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                _, stderr = process.communicate(timeout=5)
            logs = stderr.decode(errors="replace")
            assert "primary-secret" not in logs and "backup-secret" not in logs
            assert "private-prompt" not in logs
            entries = [json.loads(line) for line in logs.splitlines() if line.startswith('{"event":"request"')]
            if completed:
                assert len(entries) >= 10, logs
                assert all("request_id" in item and "initial_upstream" in item and
                           "final_upstream" in item and "attempts" in item for item in entries)
        for server, thread in ((primary, primary_thread), (backup, backup_thread)):
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("用法: management_e2e.py <llmbridge-binary>")
    main(Path(sys.argv[1]).resolve())
