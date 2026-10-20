#!/usr/bin/env python3
"""真实进程验证虚拟 Key、服务端凭证、权限和用户级限额。"""

from __future__ import annotations

import http.client
import http.server
import json
import os
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


class Mock(http.server.BaseHTTPRequestHandler):
    received: list[tuple[str, str, str]] = []

    def do_POST(self) -> None:  # noqa: N802
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        model = json.loads(body)["model"]
        self.received.append((self.headers.get("Authorization", ""),
                              self.headers.get("X-Kottos-Key-Forged", ""), model))
        payload = json.dumps({"id": "secure-mock", "object": "chat.completion",
                              "choices": [{"index": 0, "message": {"role": "assistant", "content": "ok"},
                                           "finish_reason": "stop"}],
                              "usage": {"prompt_tokens": 3, "completion_tokens": 2, "total_tokens": 5}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, _format: str, *_args: object) -> None:
        pass


def request(port: int, token: str | None, model: str) -> tuple[int, bytes]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    headers = {"Content-Type": "application/json", "X-Conversation-Id": "shared-chat",
               "X-Kottos-Key-Forged": "forged-provider-key"}
    if token is not None:
        headers["Authorization"] = f"Bearer {token}"
    conn.request("POST", "/v1/chat/completions",
                 body=json.dumps({"model": model, "messages": [{"role": "user", "content": "private-prompt"}]}),
                 headers=headers)
    response = conn.getresponse()
    status, body = response.status, response.read()
    conn.close()
    return status, body


def main(binary: Path) -> None:
    port, dead, live = free_port(), free_port(), free_port()
    mock = http.server.ThreadingHTTPServer(("127.0.0.1", live), Mock)
    thread = threading.Thread(target=mock.serve_forever, daemon=True)
    thread.start()
    alice, bob = "gateway-alice-key-123456", "gateway-bob-key-12345678"
    provider_a, provider_b = "provider-a-secret", "provider-b-secret"
    config = {
        "listen": {"port": port},
        "upstream": [
            {"url": f"127.0.0.1:{dead}", "dialect": "openai", "credential_env": "PROVIDER_A", "model": "model-a"},
            {"url": f"127.0.0.1:{live}", "dialect": "openai", "credential_env": "PROVIDER_B", "model": "model-b"},
        ],
        "routing": {"mode": "first", "max_attempts": 2, "failure_threshold": 1, "cooldown_s": 30},
        "security": {"enabled": True, "allow_insecure_http": True, "users": [
            {"id": "alice", "tenant": "team-a", "key_env": "ALICE_KEY", "models": ["chat-a"],
             "upstreams": [0, 1], "requests_per_minute": 1, "max_concurrent": 1},
            {"id": "bob", "tenant": "team-b", "key_env": "BOB_KEY", "models": ["chat-b"],
             "upstreams": [1], "requests_per_minute": 2, "max_concurrent": 1,
             "daily_tokens": 5, "monthly_tokens": 5},
        ]},
        "runtime": {"io": "epoll", "workers": 2, "request_log": True, "log_level": "error"},
    }
    process: subprocess.Popen[bytes] | None = None
    try:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "config.json"
            path.write_text(json.dumps(config), encoding="utf-8")
            env = {**os.environ, "PROVIDER_A": provider_a, "PROVIDER_B": provider_b,
                   "ALICE_KEY": alice, "BOB_KEY": bob}
            process = subprocess.Popen([str(binary), "--config", str(path)],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise AssertionError(process.stderr.read().decode(errors="replace"))
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                        break
                except OSError:
                    time.sleep(0.02)
            else:
                raise AssertionError("gateway did not listen")
            assert request(port, None, "chat-a")[0] == 401
            assert request(port, "wrong-key", "chat-a")[0] == 401
            assert request(port, alice, "chat-b")[0] == 403
            assert request(port, alice, "chat-a")[0] == 200
            assert request(port, alice, "chat-a")[0] == 429
            assert request(port, bob, "chat-b")[0] == 200
            assert request(port, bob, "chat-b")[0] == 429
            assert Mock.received == [(f"Bearer {provider_b}", "", "model-b"),
                                     (f"Bearer {provider_b}", "", "model-b")], Mock.received
    finally:
        if process is not None:
            process.terminate()
            _, stderr = process.communicate(timeout=5)
            for secret in (alice, bob, provider_a, provider_b, "private-prompt"):
                assert secret.encode() not in stderr
            assert b'"tokens_in":3' in stderr and b'"tokens_out":2' in stderr, stderr
        mock.shutdown()
        thread.join(timeout=3)
        mock.server_close()


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve())
