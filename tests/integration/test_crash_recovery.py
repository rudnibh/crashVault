#!/usr/bin/env python3
"""
tests/integration/test_crash_recovery.py
Deterministic crash test using a compiled-in failpoint.

The daemon is started with CV_FAILPOINT=versioner_after_manifest, so it dies
abruptly (exit 90, no destructors, no shutdown path) after the manifest is
durable but before the WAL COMMIT / history update. On restart the WAL replay
must complete the operation: the version must appear in history and verify
must report a healthy store.

Usage: python3 test_crash_recovery.py <crashvaultd> <cvctl>
"""

import json
import os
import signal
import subprocess
import sys
import tempfile
import time


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


class Cvctl:
    def __init__(self, socket, binary):
        self.socket = socket
        self.binary = binary

    def raw(self, *args):
        p = subprocess.run(
            [self.binary, "--socket", self.socket, "--json", *args],
            capture_output=True, text=True, timeout=10,
        )
        if not p.stdout.strip():
            raise RuntimeError(p.stderr.strip())
        return json.loads(p.stdout)

    def call(self, *args):
        resp = self.raw(*args)
        if not resp.get("ok"):
            raise AssertionError(f"{args}: {resp}")
        return resp.get("data", {})

    def ping(self):
        try:
            return self.raw("ping").get("ok") is True
        except Exception:
            return False


def start(binary, watch_dir, store_dir, log_file, env_extra=None):
    config = os.path.join(store_dir, "config.ini")
    with open(config, "w") as f:
        f.write(f"""[daemon]
worker_count = 1
debounce_ms = 100
scheduling_policy = fcfs
snapshot_quiescence_s = 0
[watch]
root = {watch_dir}
""")
    env = os.environ.copy()
    if env_extra:
        env.update(env_extra)
    proc = subprocess.Popen(
        [binary, "--foreground", "--debug", "--store", store_dir,
         "--log", log_file, "--config", config],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, preexec_fn=os.setsid, env=env,
    )
    return proc


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
        print("Usage: test_crash_recovery.py <crashvaultd> <cvctl>", file=sys.stderr)
        return 2
    daemon_bin, cvctl_bin = sys.argv[1], sys.argv[2]

    with tempfile.TemporaryDirectory(prefix="cv_crash_") as tmp:
        watch_dir = os.path.join(tmp, "watched")
        store_dir = os.path.join(tmp, "store")
        log_file = os.path.join(tmp, "daemon.log")
        os.makedirs(watch_dir)
        os.makedirs(store_dir)
        socket = os.path.join(store_dir, "crashvaultd.sock")
        cv = Cvctl(socket, cvctl_bin)

        filepath = os.path.join(watch_dir, "recover.txt")

        # ── Crash run ────────────────────────────────────────────────────────
        proc = start(daemon_bin, watch_dir, store_dir, log_file,
                     {"CV_FAILPOINT": "versioner_after_manifest"})
        try:
            wait_for(lambda: cv.ping(), what="crashing daemon ping")
            # Write only after the inotify watch is active, otherwise the
            # daemon never sees the event and the failpoint cannot fire.
            with open(filepath, "w") as f:
                f.write("this version must survive the crash\n")
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                raise AssertionError("failpoint did not fire")
            assert proc.returncode == 90, f"unexpected exit {proc.returncode}"
            print("PASS daemon crashed at versioner_after_manifest (exit 90)")
        finally:
            stop(proc)

        # ── Recovery run ─────────────────────────────────────────────────────
        proc = start(daemon_bin, watch_dir, store_dir, log_file)
        try:
            wait_for(lambda: cv.ping(), what="recovered daemon ping")
            hist = wait_for(
                lambda: (cv.call("history", filepath).get("versions") or [None]),
                what="recovered history")
            assert len(hist) == 1, hist
            assert hist[0]["file_size"] == len(
                "this version must survive the crash\n")
            assert cv.call("status")["pending_wal"] == 0
            assert cv.call("verify")["healthy"] is True
            print("PASS WAL replay completed the interrupted version")
            print("PASS verify reports a healthy store")
        finally:
            stop(proc)

        if os.path.exists(log_file):
            if "WAL replay" not in open(log_file).read():
                print("WARN: expected WAL replay messages in daemon log")

    print("\nALL CRASH RECOVERY CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
