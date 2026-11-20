#!/usr/bin/env bash
# scripts/crash_demo.sh [RUNS] — repeated crash/recovery consistency check
#
# Each run crashes the daemon at a different point in the write protocol,
# restarts it, and checks that:
#   1. startup WAL replay terminates with zero pending entries
#   2. every manifest/chunk still verifies
#   3. a newly written file is versioned after recovery (daemon functional)
#   4. operations that reached the manifest/commit point are never lost
#
# Usage: ./scripts/crash_demo.sh [runs]      (default: 10; CI may use 10)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
RUNS="${1:-10}"

GREEN='\033[0;32m'; RED='\033[0;31m'; CYAN='\033[0;36m'; NC='\033[0m'
pass() { echo -e "${GREEN}PASS${NC} $*"; }
fail() { echo -e "${RED}FAIL${NC} $*"; exit 1; }
info() { echo -e "${CYAN}INFO${NC} $*"; }

DAEMON="${BUILD_DIR}/crashvaultd"
CVCTL="${BUILD_DIR}/cvctl"
[[ -x "$DAEMON" && -x "$CVCTL" ]] || fail "build first: cmake -B build && cmake --build build"

# Failpoints in protocol order. Stages before the manifest is durable may
# legitimately lose the in-flight operation; later stages must be recovered.
STAGES=(
  object_after_write
  object_after_fsync
  object_after_rename
  wal_after_prepare
  versioner_after_manifest
  wal_after_commit
  versioner_after_history
  wal_after_complete
  versioner_after_complete
)
# Stages from this index onward must have a durable manifest/history.
COMMITTED_FROM=4

wait_for_socket() {
  local sock="$1" pid="$2"
  for _ in $(seq 1 50); do
    if "$CVCTL" --socket "$sock" ping >/dev/null 2>&1; then return 0; fi
    kill -0 "$pid" 2>/dev/null || return 1
    sleep 0.1
  done
  return 1
}

WAIT_RC=0
wait_for_exit() {
  local pid="$1"
  WAIT_RC=0
  for _ in $(seq 1 100); do
    if [[ -d "/proc/$pid" ]] && ! grep -q '^State:.*Z' "/proc/$pid/status" 2>/dev/null; then
      sleep 0.1
    else
      break
    fi
  done
  wait "$pid" || WAIT_RC=$?
}

for run in $(seq 1 "$RUNS"); do
  stage="${STAGES[$(( (run - 1) % ${#STAGES[@]} ))]}"
  tmp=$(mktemp -d /tmp/cv_crash_demo_XXXXXX)
  watch="$tmp/watched"; store="$tmp/store"; log="$tmp/daemon.log"
  sock="$store/crashvaultd.sock"; mkdir -p "$watch" "$store"

  cat > "$store/config.ini" <<EOF
[daemon]
worker_count = 1
debounce_ms = 100
snapshot_quiescence_s = 0
[watch]
root = $watch
EOF

  start_daemon() { # $1 = optional failpoint
    local fp="${1:-}"
    if [[ -n "$fp" ]]; then
      CV_FAILPOINT="$fp" "$DAEMON" --foreground --store "$store" \
        --config "$store/config.ini" --log "$log" >/dev/null 2>&1 &
    else
      "$DAEMON" --foreground --store "$store" \
        --config "$store/config.ini" --log "$log" >/dev/null 2>&1 &
    fi
    DAEMON_PID=$!
    wait_for_socket "$sock" "$DAEMON_PID" || {
      cat "$log" 2>/dev/null || true
      fail "daemon failed to start (run $run, stage $stage)"
    }
  }

  cleanup_run() {
    if [[ -n "${DAEMON_PID:-}" ]]; then
      kill "$DAEMON_PID" 2>/dev/null || true
      wait "$DAEMON_PID" 2>/dev/null || true
      DAEMON_PID=""
    fi
    rm -rf "$tmp"
  }

  DAEMON_PID=""
  start_daemon "$stage"
  printf "crash payload %s\n" "$run" > "$watch/victim.txt"

  wait_for_exit "$DAEMON_PID"
  local_rc=$WAIT_RC
  DAEMON_PID=""
  [[ "$local_rc" -eq 90 ]] || { cleanup_run; fail "run $run stage $stage: exit $local_rc (expected 90)"; }

  # Restart normally and validate recovery.
  start_daemon
  "$CVCTL" --socket "$sock" verify | grep -q "OK: all manifests" || {
    cleanup_run; fail "run $run stage $stage: verify failed after recovery";
  }
  "$CVCTL" --socket "$sock" status | grep -q "Pending WAL:     0" || {
    cleanup_run; fail "run $run stage $stage: pending WAL entries after replay";
  }

  count=$("$CVCTL" --socket "$sock" --json history "$watch/victim.txt" \
          | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["data"]["versions"]))')
  idx=$(( (run - 1) % ${#STAGES[@]} ))
  if (( idx >= COMMITTED_FROM )); then
    [[ "$count" -eq 1 ]] || { cleanup_run; fail "run $run stage $stage: committed version lost (count=$count)"; }
  else
    [[ "$count" -le 1 ]] || { cleanup_run; fail "run $run stage $stage: duplicate versions (count=$count)"; }
  fi

  # The daemon must still accept and version new writes.
  printf "after restart %s\n" "$run" > "$watch/after.txt"
  ok=0
  for _ in $(seq 1 30); do
    n=$("$CVCTL" --socket "$sock" --json history "$watch/after.txt" \
        | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["data"]["versions"]))' 2>/dev/null || echo 0)
    if [[ "$n" -ge 1 ]]; then ok=1; break; fi
    sleep 0.1
  done
  [[ "$ok" -eq 1 ]] || { cleanup_run; fail "run $run stage $stage: daemon not functional after recovery"; }

  "$CVCTL" --socket "$sock" shutdown >/dev/null || true
  wait "$DAEMON_PID" 2>/dev/null || true
  DAEMON_PID=""
  cleanup_run
  pass "run $run/$RUNS stage=$stage"
done

echo
echo "════════════════════════════════════════"
echo " Crash consistency: $RUNS/$RUNS runs passed ✓"
echo "════════════════════════════════════════"
