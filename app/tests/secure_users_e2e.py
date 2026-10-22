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
                              "model": model,
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


def rejected_upstream_errors_hide_input(binary: Path) -> None:
    """错误原因可见，但误填在上游地址中的凭据不能进入进程输出。"""
    marker = "sk-secret-marker"
    primary = subprocess.run(
        [str(binary), "--legacy-mode", "--listen", "0", "--upstream",
         f"https://host/{marker}#fragment"],
        capture_output=True, timeout=5, check=False)
    assert primary.returncode != 0
    assert b"bad --upstream" in primary.stderr
    assert marker.encode() not in primary.stdout + primary.stderr

    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "config.json"
        path.write_text(json.dumps({"security": {"legacy_mode": True},
                                    "upstream": [{"url": "127.0.0.1:9"},
                                                 {"url": f"https://host/{marker}#fragment"}]}),
                        encoding="utf-8")
        secondary = subprocess.run([str(binary), "--config", str(path)],
                                   capture_output=True, timeout=5, check=False)
    assert secondary.returncode != 0
    assert b"bad upstream 1" in secondary.stderr
    assert marker.encode() not in secondary.stdout + secondary.stderr

    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "config.json"
        path.write_text(json.dumps({marker: True}), encoding="utf-8")
        unknown = subprocess.run([str(binary), "--config", str(path)],
                                 capture_output=True, timeout=5, check=False)
    assert unknown.returncode != 0
    assert b"unknown key" in unknown.stderr
    assert marker.encode() not in unknown.stdout + unknown.stderr

    unreadable = subprocess.run([str(binary), "--config", f"/nonexistent/{marker}.json"],
                                capture_output=True, timeout=5, check=False)
    assert unreadable.returncode != 0
    assert b"cannot read" in unreadable.stderr
    assert marker.encode() not in unreadable.stdout + unreadable.stderr

    for args, reason in [([f"--upstream={marker}"], b"flags take a separate value"),
                         (["--listen", marker], b"must be an integer"),
                         (["--upstream-dialect", marker], b"must be one of")]:
        result = subprocess.run([str(binary), *args], capture_output=True,
                                timeout=5, check=False)
        assert result.returncode != 0
        assert reason in result.stderr
        assert marker.encode() not in result.stdout + result.stderr


def duplicate_user_permissions_are_rejected(binary: Path) -> None:
    """重复权限在读取配置时拒绝，且错误输出不回显配置值。"""
    marker = "secret-duplicate-marker"
    base = {"id": "alice", "tenant": "team", "key_env": "ALICE_KEY",
            "models": ["chat"], "upstreams": [0]}
    cases = [
        [{**base, "models": [marker, marker]}],
        [{**base, "upstreams": [0, 0]}],
        [{**base, "id": marker},
         {**base, "id": marker, "key_env": "BOB_KEY"}],
        [{**base, "id": "alice", "key_env": marker},
         {**base, "id": "bob", "key_env": marker}],
    ]
    with tempfile.TemporaryDirectory() as temp:
        path = Path(temp) / "config.json"
        for users in cases:
            path.write_text(json.dumps({"security": {"users": users}}), encoding="utf-8")
            result = subprocess.run([str(binary), "--config", str(path)], capture_output=True,
                                    timeout=5, check=False)
            assert result.returncode != 0
            assert b"duplicate" in result.stderr
            assert marker.encode() not in result.stdout + result.stderr


def main(binary: Path) -> None:
    rejected_upstream_errors_hide_input(binary)
    duplicate_user_permissions_are_rejected(binary)
    port, dead, live = free_port(), free_port(), free_port()
    mock = http.server.ThreadingHTTPServer(("127.0.0.1", live), Mock)
    thread = threading.Thread(target=mock.serve_forever, daemon=True)
    thread.start()
    alice, bob = "gateway-alice-key-123456", "gateway-bob-key-12345678"
    provider_a, provider_b = "provider-a-secret", "provider-b-secret"
    config = {
        "listen": {"port": port},
        "upstream": [
            {"id": "provider-a", "url": f"127.0.0.1:{dead}", "dialect": "openai",
             "credential_env": "PROVIDER_A", "model": "model-a"},
            {"id": "provider-b", "url": f"127.0.0.1:{live}", "dialect": "openai",
             "credential_env": "PROVIDER_B", "model": "model-b"},
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
    stderr_parts: list[bytes] = []
    try:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "config.json"
            ledger_path = Path(temp) / "audit.jsonl"
            config["accounting"] = {
                "ledger_path": str(ledger_path),
                "prices": [
                    {"provider": provider, "model": model, "version": "test-v1",
                     "source": "mock price fixture", "valid_from_unix_s": 0,
                     "valid_until_unix_s": 0, "rates": {"input": 2, "output": 5}}
                    for provider, model in (("provider-a", "model-a"),
                                            ("provider-b", "model-b"))
                ],
            }
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
            ledger_bytes = b""
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if ledger_path.exists():
                    ledger_bytes = ledger_path.read_bytes()
                    if len(ledger_bytes.splitlines()) == 3:
                        break
                time.sleep(0.01)
            else:
                raise AssertionError(f"audit records were not persisted: {ledger_bytes!r}")
            process.terminate()
            _, first_stderr = process.communicate(timeout=5)
            stderr_parts.append(first_stderr)
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
                raise AssertionError("restarted gateway did not listen")
            # Bob 已在前一个进程恰好用完 5 Token；重启后必须从账本恢复，而不是清零。
            assert request(port, bob, "chat-b")[0] == 429
            assert Mock.received == [(f"Bearer {provider_b}", "", "model-b"),
                                     (f"Bearer {provider_b}", "", "model-b")], Mock.received
    finally:
        if process is not None:
            process.terminate()
            _, final_stderr = process.communicate(timeout=5)
            stderr_parts.append(final_stderr)
        if stderr_parts:
            stderr = b"".join(stderr_parts)
            for secret in (alice, bob, provider_a, provider_b, "private-prompt"):
                assert secret.encode() not in stderr
            assert b'"tokens_in":3' in stderr and b'"tokens_out":2' in stderr, stderr
            records = [json.loads(line) for line in ledger_bytes.decode().splitlines()]
            assert len(records) == 3, records
            retried = {}
            for record in records:
                retried.setdefault(record["request_seq"], []).append(record)
                assert record["tenant_id"] in ("team-a", "team-b")
                assert record["user_id"] in ("alice", "bob")
                assert len(record["instance_id"]) == 32
                assert len(record["crc32"]) == 8
            pair = next(group for group in retried.values() if len(group) == 2)
            pair.sort(key=lambda item: item["attempt"])
            assert pair[0]["provider_id"] == "provider-a"
            assert pair[0]["outcome"] == "transport_error" and pair[0]["retrying"]
            assert pair[0]["cost_status"] == "usage_unknown", pair
            assert pair[1]["provider_id"] == "provider-b" and pair[1]["status"] == 200
            assert pair[1]["calculated_cost_pdollars"] == 16
            for secret in (alice, bob, provider_a, provider_b, "private-prompt"):
                assert secret.encode() not in ledger_bytes
        mock.shutdown()
        thread.join(timeout=3)
        mock.server_close()


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve())
