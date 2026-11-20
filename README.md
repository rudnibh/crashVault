# CrashVault — Crash-Consistent Versioning and Recovery Service

A lightweight Linux daemon that watches user-selected folders, stores deduplicated versions of every changed file, groups related changes into consistent project-level snapshots, recovers the last committed state after a crash, and verifies stored data for corruption.

## Why This Exists

CrashVault demonstrates core Operating Systems concepts through a real, working implementation:

| OS Concept | Implementation | Where to See It |
|---|---|---|
| Filesystem monitoring | inotify + epoll | `src/watcher/inotify_watcher.cc` |
| Daemons and signals | double-fork, self-pipe trick | `src/daemon/daemonize.cc`, `signal_handler.cc` |
| Producer-consumer | Bounded queue, 2 condvars | `src/pipeline/bounded_queue.cc` |
| File locking | flock(LOCK_SH\|LOCK_NB) | `src/store/versioner.cc` (`safe_read`) |
| Bounded memory | Byte-budget read leases | `src/pipeline/buffer_pool.cc` |
| IPC | Unix domain socket, JSON | `src/ipc/socket_server.cc` |
| Scheduling | nice/setpriority, FCFS/Priority | `src/pipeline/worker_pool.cc` |
| WAL + crash consistency | WAL + fsync + atomic rename | `src/store/wal.cc`, `object_store.cc` |

## 60-Second Quickstart

```bash
# Prerequisites: g++ >= 10, cmake >= 3.18, python3
git clone https://github.com/rudnibh/crashvault
cd crashvault

# Build (fetches doctest and nlohmann/json automatically)
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j$(nproc)

# Run the full demo (versioning, snapshots, restore, verify, GC, crash recovery)
./scripts/demo.sh

# Repeated crash consistency check (default 10 runs, use 100 for stress)
./scripts/crash_demo.sh 100

# Run the complete test suite (unit + integration + demos)
cd build && ctest --output-on-failure
```

### Web dashboard

```bash
./build/crashvaultd --foreground &
./build/web_gateway --socket ~/.crashvault/crashvaultd.sock --port 8080
# Open http://127.0.0.1:8080
```

### Benchmark

```bash
./build/bench_store    # FastCDC vs fixed-size + end-to-end dedup ratio
```

## Running the Daemon

```bash
# Foreground mode (logs to stderr)
./build/crashvaultd --foreground --config config.ini

# Daemon mode (double-fork, logs to ~/.crashvault/logs/daemon.log)
./build/crashvaultd --config config.ini
```

## Configuration

Create `~/.crashvault/config.ini`:

```ini
[daemon]
worker_count = 4
debounce_ms = 300
scheduling_policy = fcfs     # or: priority
memory_cap_mb = 256

[watch]
root = /home/user/projects
root = /home/user/documents
ignore = *.swp
ignore = *.tmp

[retention]
versions = 10
days = 30
```

## Command Reference (cvctl)

```bash
cvctl status                       # Show daemon status and counters
cvctl ping                         # Check daemon is alive
cvctl set-policy priority          # Switch scheduling policy at runtime
cvctl reload                       # Reload config (same as SIGHUP)
cvctl shutdown                     # Gracefully stop daemon

cvctl list [root]                   # List tracked files
cvctl history <path>                # Show versions (newest first)
cvctl snapshots                     # List project snapshots
cvctl snapshot-now [root]           # Trigger an immediate snapshot
cvctl restore <path> [version]      # Restore newest or a 1-based version
cvctl restore <path> --dest <file>  # Restore somewhere else
cvctl restore <path> --manifest ID  # Restore an exact manifest
cvctl restore-snapshot <id> [dir]   # Restore a whole snapshot ('latest' works)
cvctl verify                        # Re-hash every chunk and check history
cvctl gc                            # Apply retention and reclaim disk space
cvctl simulate-crash                # Crash after the next version (demo/test)
```

`cvctl` talks to the daemon over `~/.crashvault/crashvaultd.sock`; pass
`--socket PATH` when the daemon uses a different store root. All commands
accept `--json` for scripting.

## Architecture

```
crashvaultd (single process, multiple threads)
│
├── Watcher Thread (inotify/epoll)
│   └── → Debouncer (coalesces burst events, quiet window)
│         └── → Bounded Queue (mutex + 2 condvars, back-pressure, aging)
│               └── → Worker Pool (N threads, nice +10, small files first)
│                     ├── flock + stat-before/after safe read
│                     ├── Chunk file (FastCDC)
│                     ├── SHA-256 hash chunks
│                     ├── Write chunks (temp+fsync+rename+fsync dir)
│                     ├── WAL PREPARE → write manifest → COMMIT
│                     ├── Append per-path history → COMPLETE
│                     └── Mark snapshot dirty
│
├── Main Thread
│   ├── WAL replay on startup (crash recovery)
│   ├── Quiescence auto-snapshots
│   └── SIGHUP config reload
│
└── IPC Thread (Unix socket, 0600)
    ├── → cvctl
    └── → web_gateway → browser dashboard
```

### On-Disk Layout

```
~/.crashvault/
  crashvaultd.pid           ← PID file (flock held for process lifetime)
  crashvaultd.sock          ← Unix domain socket (0600)
  config.ini                ← User config
  wal/
    <seq>.wal               ← Incomplete operation (PREPARE or COMMIT)
    <seq>.wal.done          ← Completed operation (GC'd after 200 entries)
  objects/
    ab/cdef...              ← Content-addressed chunks (SHA-256 keyed, 0444)
  manifests/
    <id>.json               ← Per-file version manifest
  snapshots/
    <id>.json               ← Project snapshot (path → manifest id)
  meta/
    file_history.json       ← path → [version_ids]
  logs/
    daemon.log              ← Daemon log
```

### Crash Recovery

Every version write follows this protocol:

1. write chunks (temp → fsync → rename → fsync dir)
2. WAL PREPARE (intent durable)
3. write manifest (atomic)
4. WAL COMMIT
5. append history
6. WAL COMPLETE

On startup the daemon replays every `.wal` entry:

| Entry state | Recovery action |
|---|---|
| PREPARE, no manifest | discard (chunks are harmless and GC'd later) |
| PREPARE, manifest exists | finish history append (crash between 3 and 4) |
| COMMIT | re-append history idempotently, then COMPLETE |
| COMPLETE | not replayed |

`cvctl verify` re-hashes every chunk, checks history references, and validates
snapshot references. `cvctl restore` also re-hashes each chunk while rebuilding
a file, so corrupt data is never silently restored. `cvctl gc` enforces
retention and removes unreferenced manifests/chunks plus stale temp files.

## Build Variants

```bash
# ASan + UBSan (address and undefined behaviour sanitizers)
cmake -B build-asan -DSANITIZE=asan -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j$(nproc)
cd build-asan && ctest -V

# TSan (thread sanitizer — checks for data races)
cmake -B build-tsan -DSANITIZE=tsan -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan -j$(nproc)

# Release (failpoints disabled)
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j$(nproc)
```

## Signals

| Signal | Effect |
|---|---|
| `SIGTERM` | Graceful drain and shutdown |
| `SIGINT` | Same as SIGTERM (foreground only) |
| `SIGHUP` | Reload config file |

The daemon handles signals via the **self-pipe trick**: the signal handler writes one byte to a non-blocking pipe; the main loop waits on that pipe with `poll(2)` and drains it in normal thread context. This keeps all signal handling async-signal-safe and makes shutdown immediate instead of waiting for a sleep tick.

## Limitations

1. **Advisory locking only**: `flock(2)` is advisory. A process that doesn't call `flock` can still write concurrently. CrashVault cannot protect against non-cooperating writers.
2. **Linux only**: Uses inotify, epoll, eventfd, and POSIX signals — all Linux-specific. WSL2 `/mnt/c` paths use a 9P filesystem which does not support inotify; run on a native Linux filesystem (ext4, btrfs, xfs).
3. **No encryption**: Stored chunks are unencrypted. For sensitive data, encrypt the store root (e.g., use an encrypted filesystem).
4. **No cross-machine sync**: CrashVault is single-machine only.
5. **No hard links or special files**: Only regular files are versioned.
6. **inotify watch limit**: The kernel defaults to 8192 or 65536 watches. For large trees, raise it: `echo 524288 | sudo tee /proc/sys/fs/inotify/max_user_watches`.
7. **Dashboard has no authentication**: `web_gateway` binds to `127.0.0.1` by default. Do not expose it on a public interface.
8. **Failpoints are debug-only**: `CV_FAILPOINT` crash injection is compiled out in `Release` builds.
9. **Some settings need a restart**: `SIGHUP` hot-reloads the scheduling policy, retention limits, and snapshot quiescence window. Changing watched roots, ignore patterns, worker count, memory cap, debounce window, store root, or gateway port requires restarting the daemon because those values are baked into threads, watchers, and buffer pools at startup.

## Evaluation / Viva

See [`docs/VIVA_DEMO_GUIDE.md`](docs/VIVA_DEMO_GUIDE.md) for a repeatable
10-minute demo, a 2-minute short demo, code walkthrough map, and likely viva
questions with answers.

## CI

GitHub Actions runs:
1. Build (Release + RelWithDebInfo)
2. Unit tests (doctest)
3. Integration tests (watcher, daemon IPC, crash recovery, web gateway)
4. End-to-end demo + repeated crash-consistency demo
5. ASan/UBSan build + unit tests
6. TSan build + unit tests
7. Benchmark smoke build (`bench_store`)
