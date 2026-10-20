#!/usr/bin/env python3
"""SIGHUP 原子摘除的进程级并发测试；不触碰已有请求和上游对象。"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import http.client
import http.server
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


class HotReload(unittest.TestCase):
    binary: Path

    def test_drain_rejects_other_changes_and_survives_concurrent_requests(self) -> None:
        servers = []
        for source in ("primary", "backup"):
            def handler_for(label: str) -> type[http.server.BaseHTTPRequestHandler]:
                class Handler(http.server.BaseHTTPRequestHandler):
                    def do_POST(self) -> None:  # noqa: N802
                        self.rfile.read(int(self.headers["Content-Length"]))
                        body = json.dumps({"source": label}).encode()
                        self.send_response(200)
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)

                    def log_message(self, _format: str, *_args: object) -> None:
                        pass

                return Handler

            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler_for(source))
            server.daemon_threads = True
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            servers.append((server, thread))

        def cleanup_servers() -> None:
            for server, thread in servers:
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)

        self.addCleanup(cleanup_servers)
        port = free_port()
        config = {
            "security": {"legacy_mode": True},
            "listen": {"port": port},
            "upstream": [{"url": f"127.0.0.1:{s.server_port}"} for s, _ in servers],
            "routing": {"mode": "first", "max_attempts": 2, "drained_upstreams": []},
            "runtime": {"io": "epoll", "workers": 2, "log_level": "error"},
        }
        directory = tempfile.TemporaryDirectory(prefix="llmbridge-reload-")
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "config.json"

        def write() -> None:
            temporary = path.with_suffix(".tmp")
            temporary.write_text(json.dumps(config), encoding="utf-8")
            os.replace(temporary, path)

        write()
        process = subprocess.Popen([str(self.binary), "--config", str(path)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        def cleanup_process() -> None:
            if process.poll() is None:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=4)
            if process.stderr:
                process.stderr.close()

        self.addCleanup(cleanup_process)

        def request() -> str:
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
            try:
                connection.request("POST", "/v1/chat/completions",
                                   body='{"model":"chat","messages":[]}',
                                   headers={"Content-Type": "application/json"})
                response = connection.getresponse()
                self.assertEqual(response.status, 200)
                return json.loads(response.read())["source"]
            finally:
                connection.close()

        def await_source(source: str) -> None:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise AssertionError(f"网关提前退出: {process.stderr.read()!r}")
                try:
                    if request() == source:
                        return
                except OSError:
                    pass
                time.sleep(0.03)
            raise AssertionError(f"等待 {source} 超时")

        await_source("primary")
        config["routing"]["drained_upstreams"] = ["0"]
        write()
        process.send_signal(signal.SIGHUP)
        await_source("backup")

        config["routing"]["mode"] = "round_robin"
        config["routing"]["drained_upstreams"] = []
        write()
        process.send_signal(signal.SIGHUP)
        time.sleep(0.3)
        self.assertEqual({request() for _ in range(12)}, {"backup"})

        config["routing"]["mode"] = "first"
        with ThreadPoolExecutor(max_workers=12) as executor:
            for i in range(12):
                config["routing"]["drained_upstreams"] = [] if i % 2 else ["0"]
                write()
                process.send_signal(signal.SIGHUP)
                results = list(executor.map(lambda _: request(), range(24)))
                self.assertTrue(set(results) <= {"primary", "backup"})
                self.assertIsNone(process.poll())

        config["routing"]["drained_upstreams"] = []
        write()
        process.send_signal(signal.SIGHUP)
        await_source("primary")


if __name__ == "__main__":
    HotReload.binary = Path(sys.argv.pop(1)).resolve()
    unittest.main()
