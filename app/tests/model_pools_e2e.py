#!/usr/bin/env python3
"""AI-llm-transplot 对 llmbridge 的扩展（Apache-2.0）：模型池真实进程测试。"""

from __future__ import annotations

import http.client
import http.server
import json
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


def mock(label: str, status: int) -> tuple[http.server.ThreadingHTTPServer,
                                          threading.Thread, list[dict[str, object]]]:
    received: list[dict[str, object]] = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self) -> None:  # noqa: N802
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            received.append({"model": body["model"],
                             "authorization": self.headers.get("Authorization", "")})
            payload = json.dumps({"source": label}).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def log_message(self, _format: str, *_args: object) -> None:
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread, received


def request(port: int, model: str) -> tuple[int, dict[str, object]]:
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    try:
        connection.request("POST", "/v1/chat/completions",
                           body=json.dumps({"model": model, "messages": [
                               {"role": "user", "content": "ping"}]}),
                           headers={"Content-Type": "application/json",
                                    "X-Kottos-Key-One": "secret-one",
                                    "X-Kottos-Key-Two": "secret-two",
                                    "X-Kottos-Key-Three": "secret-three"})
        response = connection.getresponse()
        return response.status, json.loads(response.read())
    finally:
        connection.close()


def main(binary: Path) -> None:
    backends = [mock("chat-primary", 503), mock("code", 200), mock("chat-backup", 200)]
    process: subprocess.Popen[bytes] | None = None
    try:
        port = free_port()
        config = {
            "security": {"legacy_mode": True},
            "listen": {"port": port, "tls": False},
            "upstream": [
                {"url": f"127.0.0.1:{backends[i][0].server_port}", "dialect": "openai",
                 "credential_header": f"x-kottos-key-{name}",
                 "models": [model], "model": f"provider-{name}"}
                for i, (name, model) in enumerate((("one", "chat"), ("two", "code"),
                                                    ("three", "chat")))
            ],
            "routing": {"mode": "first", "max_attempts": 3, "allow_unsafe_replay": True,
                        "failure_threshold": 1, "cooldown_s": 30},
            "runtime": {"io": "epoll", "workers": 1, "log_level": "error"},
        }
        with tempfile.TemporaryDirectory(prefix="llmbridge-pools-") as directory:
            path = Path(directory) / "config.json"
            path.write_text(json.dumps(config), encoding="utf-8")
            process = subprocess.Popen([str(binary), "--config", str(path)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"网关启动失败: {process.stderr.read()!r}")
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                        break
                except OSError:
                    time.sleep(0.02)
            else:
                raise TimeoutError("网关启动超时")

            assert request(port, "chat") == (200, {"source": "chat-backup"})
            assert request(port, "code") == (200, {"source": "code"})
            assert request(port, "unknown")[0] == 503
            first, second, third = (entry[2] for entry in backends)
            assert len(first) == len(second) == len(third) == 1
            assert first[0] == {"model": "provider-one", "authorization": "Bearer secret-one"}
            assert second[0] == {"model": "provider-two", "authorization": "Bearer secret-two"}
            assert third[0] == {"model": "provider-three", "authorization": "Bearer secret-three"}

            process.send_signal(signal.SIGTERM)
            process.wait(timeout=3)
            if process.stderr:
                process.stderr.close()
            single_port = free_port()
            config["listen"]["port"] = single_port
            config["upstream"] = [config["upstream"][1]]
            config["upstream"][0].pop("credential_header")
            config["upstream"][0].pop("model")
            config["routing"]["max_attempts"] = 1
            single_path = Path(directory) / "single.json"
            single_path.write_text(json.dumps(config), encoding="utf-8")
            process = subprocess.Popen([str(binary), "--config", str(single_path)],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f"单上游网关启动失败: {process.stderr.read()!r}")
                try:
                    with socket.create_connection(("127.0.0.1", single_port), timeout=0.1):
                        break
                except OSError:
                    time.sleep(0.02)
            else:
                raise TimeoutError("单上游网关启动超时")
            assert request(single_port, "unknown")[0] == 503
            assert len(second) == 1
    finally:
        if process is not None:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            if process.stderr:
                process.stderr.close()
        for server, thread, _received in backends:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve())
