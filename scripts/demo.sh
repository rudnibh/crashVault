#!/usr/bin/env bash
# scripts/demo.sh — CrashVault end-to-end demo
#
# Exercises the real daemon through its public IPC interface:
#   versioning → history → snapshots → restore → verify → GC → crash recovery
#
# The crash is deterministic: `cvctl simulate-crash` arms a failpoint that
# makes the daemon exit abruptly after the next version, then the demo restarts
# it and proves WAL replay retained the committed state.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"

GREEN='\033[0;32m'
RED='\033[0;31m'
CYAN='\033[0;36m'
NC='\033[0m'

pass() { echo -e "${GREEN}PASS${NC} $*"; }
fail() { echo -e "${RED}FAIL${NC} $*"; exit 1; }
info() { echo -e "${CYAN}INFO${NC} $*"; }

# ── 0. Find binaries ─────────────────────────────────────────────────────────
DAEMON="${BUILD_DIR}/crashvaultd"
CVCTL="${BUILD_DIR}/cvctl"
[[ -x "$DAEMON" ]] || fail "crashvaultd not found at $DAEMON — run: cmake -B build && cmake --build build"
[[ -x "$CVCTL" ]]  || fail "cvctl not found at $CVCTL"

info "crashvaultd: $DAEMON"
info "cvctl:       $CVCTL"

# ── 1. Temporary environment ────────────────────────────────────────────────
TMPDIR=$(mktemp -d /tmp/cv_demo_XXXXXX)
WATCHDIR="${TMPDIR}/watched"
STOREDIR="${TMPDIR}/store"
LOGFILE="${TMPDIR}/daemon.log"
SOCK="${STOREDIR}/crashvaultd.sock"
mkdir -p "$WATCHDIR" "$STOREDIR"

cleanup() {
  if [[ -n "${DAEMON_PID:-}" ]]; then
    kill "$DAEMON_PID" 2>/dev/null || true
    wait "$DAEMON_PID" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR"
}
trap cleanup EXIT

cat > "${STOREDIR}/config.ini" <<EOF
[daemon]
worker_count = 2
debounce_ms = 200
scheduling_policy = priority
snapshot_quiescence_s = 1
[watch]
root = ${WATCHDIR}
EOF

ctl() { "$CVCTL" --socket "$SOCK" "$@"; }

wait_for_socket() {
  for _ in $(seq 1 50); do
    if ctl ping >/dev/null 2>&1; then return 0; fi
    sleep 0.1
  done
  return 1
}

start_daemon() {
  "$DAEMON" --foreground --debug --store "$STOREDIR" \
    --config "${STOREDIR}/config.ini" --log "$LOGFILE" \
    >"${TMPDIR}/daemon.stdout" 2>&1 &
  DAEMON_PID=$!
  if ! wait_for_socket; then
    cat "$LOGFILE" 2>/dev/null || true
    fail "daemon failed to start"
  fi
  pass "Daemon running (pid=$DAEMON_PID)"
}

# ── 2. Start daemon ─────────────────────────────────────────────────────────
info "Starting crashvaultd..."
start_daemon

info "Pinging daemon via cvctl..."
ctl ping | grep -q pong && pass "cvctl ping → pong"

# ── 3. Write files using different patterns ─────────────────────────────────
info "Writing files using vim-style atomic rename..."
TMP_FILE="${WATCHDIR}/.vim_tmp"
echo "Hello from vim" > "$TMP_FILE"
mv "$TMP_FILE" "${WATCHDIR}/vim_file.txt"

info "Writing files using direct write..."
echo "Direct write v1" > "${WATCHDIR}/direct_file.txt"

info "Appending to a log file..."
for i in $(seq 1 5); do
  echo "Log line $i" >> "${WATCHDIR}/app.log"
  sleep 0.05
done
sleep 1.2  # debounce + worker drain

ctl list | grep -q "vim_file.txt"    && pass "vim_file.txt tracked"
ctl list | grep -q "direct_file.txt" && pass "direct_file.txt tracked"
ctl list | grep -q "app.log"         && pass "app.log tracked"

# ── 4. Real versions + history ──────────────────────────────────────────────
info "Creating a second version of direct_file.txt..."
echo "Direct write v2" > "${WATCHDIR}/direct_file.txt"
sleep 1.0
HIST=$(ctl history "${WATCHDIR}/direct_file.txt")
echo "$HIST" | grep -q "v1" && pass "history contains v1"
echo "$HIST" | grep -q "v2" && pass "history contains v2"

ctl status | grep -q "Versions stored:" && pass "status reports versions"

# ── 5. Snapshots ────────────────────────────────────────────────────────────
info "Creating an explicit snapshot..."
SNAP_OUT=$(ctl snapshot-now "${WATCHDIR}")
SNAP_ID=$(echo "$SNAP_OUT" | awk -F'"' '/Snapshot:/ {print $2}')
[[ -n "$SNAP_ID" ]] || fail "snapshot-now returned no id"
pass "snapshot created: $SNAP_ID"
ctl snapshots | grep -q "$SNAP_ID" && pass "snapshot listed"

# ── 6. Restore ──────────────────────────────────────────────────────────────
info "Restoring version 1 of direct_file.txt to a separate path..."
ctl restore "${WATCHDIR}/direct_file.txt" 1 --dest "${TMPDIR}/restored.txt" >/dev/null
if grep -q "Direct write v1" "${TMPDIR}/restored.txt"; then
  pass "restore v1 produced the original content"
else
  fail "restore v1 content mismatch"
fi

# ── 7. Verify + GC ──────────────────────────────────────────────────────────
info "Verifying manifests and chunk hashes..."
ctl verify | grep -q "OK: all manifests" && pass "verify: store healthy"

info "Running garbage collection..."
ctl gc >/dev/null && pass "gc completed"

# ── 8. Deterministic crash + recovery ───────────────────────────────────────
sleep 0.5  # let the queue drain so the next event is definitely the trigger
info "Arming simulate-crash and writing a trigger file..."
ctl simulate-crash >/dev/null
echo "survive the crash" > "${WATCHDIR}/crash_survivor.txt"

info "Waiting for daemon to die abruptly..."
CRASH_RC=0
wait "$DAEMON_PID" || CRASH_RC=$?
DAEMON_PID=""
[[ "$CRASH_RC" -eq 90 ]] && pass "daemon crashed abruptly (exit 90)" || \
  fail "expected exit 90, got $CRASH_RC"

info "Restarting daemon (WAL replay)..."
start_daemon
sleep 0.5
ctl verify | grep -q "OK: all manifests" && pass "store healthy after crash"
if ctl list | grep -q "crash_survivor.txt"; then
  pass "version written before crash survived WAL replay"
else
  # The trigger file was versioned before the crash, so it must be present.
  fail "crash_survivor.txt missing after recovery"
fi
ctl status | grep -q "Pending WAL:     0" && pass "WAL fully replayed"

# ── 9. Graceful shutdown ────────────────────────────────────────────────────
info "Sending shutdown via cvctl..."
ctl shutdown >/dev/null
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
grep -q "stopped cleanly" "$LOGFILE" && pass "daemon shut down cleanly"

echo
echo "════════════════════════════════════════"
echo " CrashVault demo: ALL CHECKS PASSED ✓"
echo "════════════════════════════════════════"
