#!/usr/bin/env python3
"""
tests/integration/test_daemon_phase5.py
End-to-end test: versioning, history, snapshots, restore, verify, GC and the
simulate-crash path, all through the real daemon + cvctl IPC interface.

Usage: python3 test_daemon_phase5.py <crashvaultd> <cvctl>
"""

import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DEBOUNCE_MS = 100


def wait_for(predicate, timeout=10.0, interval=0.1, what="condition"):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(interval)
    raise AssertionError(f"timed out waiting for {what}")


class Daemon:
    def __init__(self, binary, watch_dir, store_dir, log_file,
                 snapshot_quiescence_s=0, env=None):
        self.binary = binary
        self.watch_dir = watch_dir
        self.store_dir = store_dir
        self.log_file = log_file
        self.socket = os.path.join(store_dir, "crashvaultd.sock")
        self.proc = None
        self.env = env

        self.config_path = os.path.join(store_dir, "config.ini")
        with open(self.config_path, "w") as f:
            f.write(f"""[daemon]
worker_count = 2
debounce_ms = {DEBOUNCE_MS}
scheduling_policy = fcfs
snapshot_quiescence_s = {snapshot_quiescence_s}
[watch]
root = {watch_dir}
""")

    def start(self):
        env = os.environ.copy()
        if self.env:
            env.update(self.env)
        self.proc = subprocess.Popen(
            [self.binary, "--foreground", "--debug",
             "--store", self.store_dir, "--log", self.log_file,
             "--config", self.config_path],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, preexec_fn=os.setsid, env=env,
        )
        # Wait for the socket to appear and the daemon to answer ping.
        cv = Cvctl(self.socket)
        wait_for(lambda: os.path.exists(self.socket), what="daemon socket")
        wait_for(lambda: cv.try_ping(), what="daemon ping")
        return self

    def stop(self):
        if self.proc and self.proc.poll() is None:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
                self.proc.wait()


class Cvctl:
    def __init__(self, socket, binary=None):
        self.socket = socket
        self.binary = binary or os.environ.get("CVCTL_BINARY", "cvctl")

    def raw(self, *args):
        p = subprocess.run(
            [self.binary, "--socket", self.socket, "--json", *args],
            capture_output=True, text=True, timeout=15,
        )
        if not p.stdout.strip():
            raise AssertionError(
                f"cvctl {' '.join(args)} produced no output (rc={p.returncode}): "
                f"{p.stderr.strip()}")
        return json.loads(p.stdout)

    def call(self, *args):
        resp = self.raw(*args)
        if not resp.get("ok"):
            raise AssertionError(f"cvctl {' '.join(args)} failed: {resp}")
        return resp.get("data", {})

    def try_ping(self):
        try:
            resp = self.raw("ping")
            return bool(resp.get("ok"))
        except Exception:
            return False


def read(path):
    with open(path) as f:
        return f.read()


def main():
    if len(sys.argv) != 3:
        print("Usage: test_daemon_phase5.py <crashvaultd> <cvctl>", file=sys.stderr)
        return 2

    daemon_bin, cvctl_bin = sys.argv[1], sys.argv[2]
    os.environ["CVCTL_BINARY"] = cvctl_bin

    with tempfile.TemporaryDirectory(prefix="cv_phase5_") as tmp:
        watch_dir = os.path.join(tmp, "watched")
        store_dir = os.path.join(tmp, "store")
        log_file = os.path.join(tmp, "daemon.log")
        restore_dir = os.path.join(tmp, "restored")
        os.makedirs(watch_dir)
        os.makedirs(store_dir)

        d = Daemon(daemon_bin, watch_dir, store_dir, log_file,
                   snapshot_quiescence_s=1)
        d.start()
        cv = Cvctl(d.socket, cvctl_bin)

        try:
            filepath = os.path.join(watch_dir, "demo.txt")

            # ── 1. versioning ────────────────────────────────────────────────
            with open(filepath, "w") as f:
                f.write("version one\n")
            wait_for(lambda: len(cv.call("history", filepath)["versions"]) == 1,
                     what="first version")
            with open(filepath, "w") as f:
                f.write("version two\n")
            wait_for(lambda: len(cv.call("history", filepath)["versions"]) == 2,
                     what="second version")
            print("PASS versioning + history")

            status = cv.call("status")
            assert status["tracked_files"] >= 1, status
            assert status["versions_stored"] >= 2, status
            print("PASS status counters")

            files = cv.call("list")["files"]
            assert any(f["path"] == filepath for f in files), files
            print("PASS list")

            # ── 2. automatic quiescence snapshot ─────────────────────────────
            wait_for(lambda: cv.call("snapshots")["count"] >= 1,
                     timeout=8, what="auto snapshot")
            print("PASS automatic snapshot")

            # ── 3. explicit snapshot ─────────────────────────────────────────
            snap = cv.call("snapshot-now")
            assert snap["files"] >= 1, snap
            snap_id = snap["id"]
            print(f"PASS snapshot-now ({snap_id})")

            # ── 4. restore a specific version ────────────────────────────────
            cv.call("restore", filepath, "1")
            wait_for(lambda: read(filepath) == "version one\n",
                     what="restore v1")
            print("PASS restore v1")

            # Restore v2 by id (the restore of v1 above may itself be
            # versioned by the watcher, which would make "latest" ambiguous).
            hist = cv.call("history", filepath)["versions"]
            v2_id = next(v["id"] for v in hist if v["version"] == 2)
            cv.call("restore", filepath, "--manifest", v2_id)
            wait_for(lambda: read(filepath) == "version two\n",
                     what="restore v2")
            print("PASS restore latest")

            # ── 5. restore a snapshot into an isolated directory ─────────────
            with open(filepath, "w") as f:
                f.write("version three\n")
            wait_for(lambda: len(cv.call("history", filepath)["versions"]) >= 3,
                     what="third version")

            cv.call("restore-snapshot", snap_id, restore_dir)
            restored = os.path.join(restore_dir, os.path.basename(filepath))
            assert read(restored) == "version two\n", read(restored)
            print("PASS restore-snapshot")

            # ── 6. verify + gc ───────────────────────────────────────────────
            assert cv.call("verify")["healthy"] is True
            print("PASS verify")
            cv.call("gc")
            print("PASS gc")

            # ── 7. simulate-crash ────────────────────────────────────────────
            cv.call("simulate-crash")
            with open(os.path.join(watch_dir, "crash_trigger.txt"), "w") as f:
                f.write("trigger crash\n")
            try:
                d.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                raise AssertionError("daemon did not crash after simulate-crash")
            assert d.proc.returncode == 90, d.proc.returncode
            print("PASS simulate-crash (exit 90)")

            # ── 8. restart: history and integrity survive ────────────────────
            d.start()
            cv = Cvctl(d.socket, cvctl_bin)
            wait_for(lambda: cv.try_ping(), what="restarted daemon ping")
            hist = cv.call("history", filepath)["versions"]
            assert len(hist) >= 3, hist
            assert cv.call("status")["pending_wal"] == 0
            assert cv.call("verify")["healthy"] is True
            print("PASS restart recovery + verify")
        finally:
            d.stop()
            if os.path.exists(log_file) and d.proc and d.proc.returncode not in (0, 90, None):
                with open(log_file) as f:
                    print("\n--- daemon log tail ---")
                    print("".join(f.readlines()[-30:]))

    print("\nALL PHASE 5 INTEGRATION CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
