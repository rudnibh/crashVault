#!/usr/bin/env python3
"""
tests/integration/test_web_gateway.py
Smoke test for the HTTP dashboard gateway.

Starts the daemon and the gateway, then checks:
  - GET / returns the dashboard HTML
  - GET /api/status and /api/list return daemon data
  - POST /api/snapshot-now mutates state through the daemon
  - GET /health is alive

Usage: python3 test_web_gateway.py <crashvaultd> <web_gateway>
"""

import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error


def wait_for(predicate, timeout=10.0, what="condition"):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except Exception:
            pass
        time.sleep(0.1)
    raise AssertionError(f"timed out waiting for {what}")


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def http(method, url, timeout=5):
    req = urllib.request.Request(url, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.headers.get("Content-Type", ""), r.read().decode()


def http_json(method, url):
    status, _, body = http(method, url)
    return status, json.loads(body)


def stop(proc):
    if proc and proc.poll() is None:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            proc.wait()


def main():
    if len(sys.argv) != 3:
        print("Usage: test_web_gateway.py <crashvaultd> <web_gateway>", file=sys.stderr)
        return 2
    daemon_bin, gateway_bin = sys.argv[1], sys.argv[2]

    with tempfile.TemporaryDirectory(prefix="cv_web_") as tmp:
        watch_dir = os.path.join(tmp, "watched")
        store_dir = os.path.join(tmp, "store")
        log_file = os.path.join(tmp, "daemon.log")
        os.makedirs(watch_dir)
        os.makedirs(store_dir)
        sock = os.path.join(store_dir, "crashvaultd.sock")
        config = os.path.join(store_dir, "config.ini")
        with open(config, "w") as f:
            f.write(f"""[daemon]
worker_count = 1
debounce_ms = 100
snapshot_quiescence_s = 0
[watch]
root = {watch_dir}
""")

        daemon = subprocess.Popen(
            [daemon_bin, "--foreground", "--debug", "--store", store_dir,
             "--log", log_file, "--config", config],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            preexec_fn=os.setsid,
        )
        port = free_port()
        gateway = None
        try:
            wait_for(lambda: os.path.exists(sock), what="daemon socket")

            with open(os.path.join(watch_dir, "web.txt"), "w") as f:
                f.write("hello from the web test\n")
            time.sleep(1.0)

            gateway = subprocess.Popen(
                [gateway_bin, "--socket", sock, "--port", str(port),
                 "--bind", "127.0.0.1"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                preexec_fn=os.setsid,
            )
            base = f"http://127.0.0.1:{port}"

            def get(url):
                return http_json("GET", base + url)

            def ping_gateway():
                try:
                    status, ctype, body = http("GET", base + "/health")
                    return status == 200 and "ok" in body
                except Exception:
                    return False

            wait_for(ping_gateway, what="gateway health")

            status, ctype, body = http("GET", base + "/")
            assert status == 200 and "CrashVault" in body
            assert "text/html" in ctype
            print("PASS dashboard HTML")

            status, data = get("/api/status")
            assert status == 200 and data["ok"] is True, data
            assert data["data"]["daemon_pid"] > 0
            print("PASS /api/status")

            status, data = get("/api/list")
            assert status == 200 and data["ok"] is True, data
            paths = [f["path"] for f in data["data"]["files"]]
            assert any(p.endswith("web.txt") for p in paths), paths
            print("PASS /api/list")

            status, data = get("/api/snapshots")
            assert status == 200 and data["ok"] is True, data
            print("PASS /api/snapshots")

            status, data = http_json("POST", base + "/api/snapshot-now")
            assert status == 200 and data["ok"] is True, data
            assert data["data"]["files"] >= 1
            print("PASS POST /api/snapshot-now")
        finally:
            stop(gateway)
            stop(daemon)

    print("\nALL WEB GATEWAY CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
