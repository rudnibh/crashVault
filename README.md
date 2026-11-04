# CrashVault — Crash-Consistent Versioning and Recovery Service

A lightweight Linux daemon that watches user-selected folders, stores deduplicated versions of every changed file, groups related changes into consistent project-level snapshots, and automatically detects corruption and restores the last valid state after a crash.

## Why This Exists

CrashVault demonstrates core Operating Systems concepts through a real, working implementation:

| OS Concept | Implementation | Where to See It |
|---|---|---|
| Filesystem monitoring | inotify + epoll | `src/watcher/inotify_watcher.cc` |
| Daemons and signals | double-fork, self-pipe trick | `src/daemon/daemonize.cc`, `signal_handler.cc` |
| Producer-consumer | Bounded queue, 2 condvars | `src/pipeline/bounded_queue.cc` |
| File locking | flock(LOCK_SH\|LOCK_NB) | `src/pipeline/worker_pool.cc` |
| Bounded memory | Buffer pool, LRU cache | `src/pipeline/buffer_pool.cc` |
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

# Run the demo (starts daemon, writes files, checks events, shuts down)
./scripts/demo.sh

# Or run unit tests
cd build && ctest -V
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
cvctl status               # Show daemon status
cvctl ping                 # Check daemon is alive
cvctl set-policy priority  # Switch scheduling policy at runtime
cvctl shutdown             # Gracefully stop daemon

# Phase 5+ commands:
cvctl list                 # List watched files
cvctl history <path>       # Show version history for a file
cvctl snapshots            # List project snapshots
cvctl snapshot-now         # Trigger immediate snapshot
cvctl restore <path>       # Restore file to last version
cvctl gc                   # Run garbage collection
cvctl simulate-crash       # Run crash simulator demo
```

## Architecture

```
crashvaultd (single process, multiple threads)
│
├── Watcher Thread (inotify/epoll)
│   └── → Debouncer (coalesces burst events, 300ms quiet window)
│         └── → Bounded Queue (mutex + 2 condvars, back-pressure)
│               └── → Worker Pool (N threads, nice +10)
│                     ├── Chunk file (FastCDC)
│                     ├── SHA-256 hash chunks
│                     ├── Write to object store (temp+fsync+rename)
│                     ├── Write manifest (WAL)
│                     └── Update snapshot (WAL commit)
│
└── IPC Thread (Unix socket, 0600)
    └── → cvctl / web_gateway → React dashboard
```

### On-Disk Layout

```
~/.crashvault/
  crashvaultd.pid           ← PID file (flock held for process lifetime)
  crashvaultd.sock          ← Unix domain socket (0600)
  config.ini                ← User config
  wal/
    wal.log                 ← Write-ahead log
    wal.checkpoint          ← Checkpoint LSN
  objects/
    ab/cdef...              ← Content-addressed chunks (SHA-256 keyed)
  manifests/
    <id>.json               ← Per-file version manifest
  snapshots/
    <id>.json               ← Project snapshot
  meta/
    file_history.json       ← path → [version_ids]
  logs/
    daemon.log              ← Daemon log
```

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

The daemon handles signals via the **self-pipe trick**: the signal handler writes one byte to a non-blocking pipe; the event loop reads it via epoll. This keeps all signal handling 100% async-signal-safe.

## Limitations

1. **Advisory locking only**: `flock(2)` is advisory. A process that doesn't call `flock` can still write concurrently. CrashVault cannot protect against non-cooperating writers.
2. **Linux only**: Uses inotify, epoll, eventfd, and signalfd — all Linux-specific. WSL2 `/mnt/c` paths use 9P filesystem which does not support inotify; run on a native Linux filesystem (ext4, btrfs, xfs).
3. **No encryption**: Stored chunks are unencrypted. For sensitive data, encrypt the store root (e.g., use an encrypted filesystem).
4. **No cross-machine sync**: CrashVault is single-machine only.
5. **No hard links or special files**: Only regular files are versioned.
6. **inotify watch limit**: The kernel defaults to 8192 or 65536 watches. For large trees, raise it: `echo 524288 | sudo tee /proc/sys/fs/inotify/max_user_watches`.


## CI

GitHub Actions runs:
1. Build (Release + RelWithDebInfo)
2. Unit tests (doctest)
3. ASan/UBSan build + tests
4. TSan build + tests
5. Integration tests (Python)
6. Fault-injection table (Phase 4+)
7. 100-run crash demo (Phase 6+)
