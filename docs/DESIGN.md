# CrashVault Design Document

## 1. System Overview

CrashVault is a Linux daemon that continuously versions files in watched directories using content-defined chunking and crash-consistent metadata writes. It is designed as a teaching artefact for Operating Systems concepts: every OS mechanism used has a corresponding "how to see it working" command.

---

## 2. OS Concepts — Implementation Map

| Concept | File | Function/Class | How to See It |
|---|---|---|---|
| inotify (FS events) | `src/watcher/inotify_watcher.cc` | `InotifyWatcher::add_watch` | `strace -e inotify_add_watch ./crashvaultd --foreground` |
| epoll (I/O multiplexing) | `src/watcher/inotify_watcher.cc` | `InotifyWatcher::event_loop` | `strace -e epoll_wait,epoll_ctl ./crashvaultd --foreground` |
| Daemonization | `src/daemon/daemonize.cc` | `cv::daemonize` | `ps -o pid,ppid,sid,tty -p $(cat ~/.crashvault/crashvaultd.pid)` — no controlling tty |
| Self-pipe (signals) | `src/daemon/signal_handler.cc` | `signal_handler_init` | `kill -SIGHUP $(cat ~/.crashvault/crashvaultd.pid)` → config reloaded |
| PID file + flock | `src/daemon/daemonize.cc` | `open_and_lock_pidfile` | Try to start second instance — it will refuse with an error |
| Producer-consumer queue | `src/pipeline/bounded_queue.cc` | `BoundedQueue::push/pop` | `cvctl status` → see queue_depth; write many files rapidly |
| Mutex + 2 condvars | `src/pipeline/bounded_queue.cc` | `not_full_`, `not_empty_` | Use TSan build — zero races |
| nice/setpriority | `src/pipeline/worker_pool.cc` | `WorkerPool::worker_main` | `ps -o pid,ni -p $(pgrep crashvaultd)` |
| flock(LOCK_SH\|LOCK_NB) | `src/store/versioner.cc` | `Versioner::safe_read` | Write while holding exclusive lock; check "skipped-unstable" in log |
| mmap | `src/store/chunker.cc` | `IChunker::chunk_file` | Unit test chunks a >1 MiB file through the mmap path |
| WAL (write-ahead log) | `src/store/wal.cc`, `recovery.cc` | `Wal::prepare/commit/complete`, `recover_wal` | `CV_FAILPOINT=wal_after_commit`; restart; `cvctl verify` |
| fsync | `src/store/object_store.cc` | `ObjectStore::write_chunk` | `strace -e fsync,rename ./crashvaultd --foreground` |
| Atomic rename | `src/store/object_store.cc` | `ObjectStore::write_chunk` | `strace -e rename ./crashvaultd --foreground` |
| IPC (Unix socket) | `src/ipc/socket_server.cc` | `SocketServer::serve_loop` | `socat - UNIX-CONNECT:~/.crashvault/crashvaultd.sock` |
| Scheduling policy | `src/pipeline/bounded_queue.cc` | `BoundedQueue::set_policy` | `cvctl set-policy priority`; watch latency change |
| Snapshots | `src/store/manifest.cc` | `ManifestStore::create_snapshot` | `cvctl snapshot-now`; `cvctl snapshots` |
| GC / retention | `src/store/gc.cc` | `run_gc` | `cvctl gc`; storage usage drops |
| HTTP gateway | `src/web/gateway.cc` | `handle_client` | `./build/web_gateway`, open `http://127.0.0.1:8080` |

---

## 3. Thread Model

```
Thread 0: main (blocked in sleep loop, handles SIGHUP, shuts down)
Thread 1: InotifyWatcher::event_loop (blocked in epoll_wait)
Thread 2: Debouncer::scan_loop (blocked in condvar::wait_until)
Thread 3: SocketServer::serve_loop (blocked in accept)
Threads 4–N+3: WorkerPool workers (blocked in BoundedQueue::pop)
```

**Lock ordering** (to prove deadlock freedom):
```
Lock 1: BoundedQueue::mutex_   (guards queue_)
Lock 2: Debouncer::mu_         (guards pending_)
```

No code path holds Lock 2 while acquiring Lock 1, or vice versa. These are independent monitors with no nesting. **Deadlock is impossible** between these two locks.

The watcher thread calls `debouncer->push()`, which acquires Lock 2. It never acquires Lock 1.
The debouncer thread calls `queue.push()`, which acquires Lock 1. It never acquires Lock 2.
Worker threads call `queue.pop()`, which acquires Lock 1. They never acquire Lock 2.

---

## 4. WAL Record Format

The implementation uses **one JSON file per operation** rather than a single
append-only log. Each file is written with temp + fsync + atomic rename, which
makes every state transition atomic and removes the need for torn-record/CRC
handling.

```
wal/00000000000000000042.wal       ← PREPARE or COMMIT (incomplete)
wal/00000000000000000042.wal.done  ← COMPLETE (authoritative, GC-able)
```

```json
{
  "seq": 42,
  "state": "PREPARE | COMMIT | COMPLETE",
  "manifest_id": "uuid",
  "file_path": "/abs/path",
  "version": 3,
  "chunk_shas": ["...", "..."],
  "timestamp": "2026-01-01T00:00:00Z"
}
```

**Why per-operation files:** a single append log needs truncation, rotation and
torn-tail detection. With one file per operation, advancing state is just an
atomic rename, and completed entries can be garbage-collected independently
(`Wal::gc` keeps the last 200).

**Recovery algorithm (`recover_wal`):**

1. Read every `.wal` file (`.wal.done` files are complete).
2. PREPARE and no manifest on disk → discard; any chunks are unreferenced and GC reclaims them.
3. PREPARE and manifest exists → crash happened after the manifest rename but before COMMIT; append history and complete.
4. COMMIT → re-append history (idempotent) and complete.
5. Leave entries pending if the history write fails, so the next restart retries.

**Crash-consistency argument:**

| Step | Ordering guarantee | Effect of crash |
|---|---|---|
| 1. Write chunk to temp file | No guarantee | Temp file ignored/GC'd |
| 2. fsync(temp_fd) | Data in stable storage | Chunk survives |
| 3. rename(temp, objects/ab/cdef) | Atomic on same filesystem | Chunk either fully present or absent |
| 4. fsync(dir_fd) | Directory entry durable | Chunk visible after reboot |
| 5. WAL PREPARE | Intent durable | Recovery knows an operation was in flight |
| 6. write manifest (temp+fsync+rename) | Manifest atomic | Either old or new manifest |
| 7. WAL COMMIT | Commit durable | Operation must survive |
| 8. append history (mutex + atomic write) | History durable | Replay can redo it idempotently |
| 9. WAL COMPLETE | No further recovery needed | Entry may be GC'd |

A crash before step 6 leaves an uncommitted operation with only harmless
orphaned chunks. A crash after step 6 is repaired on the next startup by
replaying the WAL. **No committed version is ever lost.**

---

## 5. On-Disk Layout

```
~/.crashvault/
│
├── crashvaultd.pid       # 10-byte PID + newline, locked with flock(LOCK_EX)
├── crashvaultd.sock      # AF_UNIX SOCK_STREAM, permissions 0600
├── config.ini
│
├── wal/
│   ├── <seq>.wal         # Incomplete operation (PREPARE or COMMIT)
│   └── <seq>.wal.done    # Completed operation (GC keeps last 200)
│
├── objects/
│   └── ab/               # First 2 hex chars of SHA-256
│       └── cdef...       # Remaining 62 hex chars = chunk data (mode 0444)
│
├── manifests/
│   └── <uuid>.json       # { "path": ..., "chunks": [...], "sha256": ..., "version": N }
│
├── snapshots/
│   └── <uuid>.json       # { "id": ..., "root": ..., "files": { path: manifest_id } }
│
├── meta/
│   └── file_history.json # { "/abs/path": ["manifest_id_1", "manifest_id_2", ...] }
│
└── logs/
    └── daemon.log        # ISO-8601 timestamped log lines
```

---

## 6. Debounce Algorithm

**Problem:** A single vim save generates 5+ inotify events in <50ms.
**Goal:** Exactly one "file settled" event per logical save.

**Algorithm:**
1. For each raw event, record `deadline = now() + debounce_ms` in a map keyed by path.
2. If the path is already in the map, **reset** the deadline (extends the quiet window).
3. A background thread sleeps until the nearest deadline, then fires all expired entries.
4. Callers receive exactly one settled event per path per quiet window.

**Why this correctly handles all write patterns:**
- **vim (atomic rename):** `IN_CREATE(tmp)`, `IN_CLOSE_WRITE(tmp)`, `IN_MOVED_TO(target)` — all within 10ms. Deadline reset 3×. One settled event for target.
- **Direct write:** `IN_CLOSE_WRITE` — single event. One settled event.
- **Append writes:** Multiple `IN_MODIFY`/`IN_CLOSE_WRITE` within debounce window — one settled event.

---

## 7. Content-Defined Chunking (FastCDC)

**Why CDC over fixed-size:** Small edits in the middle of a file shift all subsequent fixed-size chunk boundaries, forcing re-storage of the entire tail. CDC uses a gear hash to find natural boundaries in the content; a small edit typically changes only 1–2 chunks.

**Implementation:** Gear hash table (256 random 64-bit values). Sliding window over content. Boundary condition: `hash & mask == MAGIC`. Mask is chosen to target average chunk size.

**Parameters:**
- Min chunk: 2 KiB
- Average chunk: 8 KiB  
- Max chunk: 64 KiB

**Dedup:** Chunks stored in `objects/<2hex>/<62hex>`. Writing an existing chunk is a no-op (content-addressable storage means identical content always hashes to the same key).

---

## 8. Concurrency Safety Analysis

### Race conditions

**BoundedQueue:** Protected by a single `std::mutex`. All accesses to `queue_`, `path_index_` are under `mutex_`. The two `std::condition_variable` objects (`not_full_`, `not_empty_`) are associated with the same mutex. **No data race possible.**

**Debouncer:** Protected by `mu_`. All accesses to `pending_` are under `mu_`. **No data race possible.**

**InotifyWatcher:** Runs in a single thread (`event_loop`). The `wd_to_path_` and `path_to_wd_` maps are accessed only from that thread. The `callback_` is called from that thread. **No data race possible.**

**Logging (logger.cc):** Uses `write(2)` with `O_APPEND`. Linux guarantees atomicity of writes ≤ `PIPE_BUF` (4096 bytes) with `O_APPEND`. All log lines are ≤ 512 bytes. **No race possible for short messages.**

**BufferPool:** `used_` is protected by `mu_`; waiters block on `cv_` with predicates, and every lease releases exactly once via RAII. `in_use()`/`available()` take the same lock. **No data race.**

**Global atomics:** `g_shutdown_requested`, `g_reload_requested` are `std::atomic<bool>`. All accesses use correct memory order. **No race.**

### Starvation prevention (Priority policy)

Large files get low priority (high priority number). Without aging, small files would perpetually starve large ones. Aging: items waiting longer than `AGING_THRESHOLD_MS` (5s) are promoted to priority 0. This bounds wait time.

---

## 9. Memory Management

**Read budget (`BufferPool`):** a byte-counting semaphore, not a fixed-block
pool. `Versioner::safe_read` acquires a lease for the file size before
allocating its buffer and holds it through hash + chunk + manifest. Workers
block when the budget (`memory_cap_mb`, default 256 MiB) is exhausted, so N
concurrent workers cannot multiply peak memory. A file larger than the whole
budget is allowed to run alone (its lease reserves the full budget), which
avoids deadlock while still serialising oversized readers.

**mmap:** `IChunker::chunk_file` maps files at or above
`mmap_threshold_bytes` with `mmap(PROT_READ, MAP_PRIVATE)` and chunks directly
from the mapping. The production `Versioner` path intentionally uses
`flock` + `read` + before/after `stat` instead: a non-cooperating writer that
truncates a mapped file can deliver `SIGBUS`, whereas the read path can detect
the size/mtime change and retry. The mmap path is exercised by the chunker unit
tests and is available to zero-copy callers.

---

## 10. Snapshots, Retention and GC

**Snapshots** record the newest valid manifest for every tracked file under a
root (`ManifestStore::create_snapshot`). They are created:

- automatically after `snapshot_quiescence_s` seconds without events, or
- on demand with `cvctl snapshot-now [root]`.

Restoring a snapshot (`cvctl restore-snapshot <id> [dir]`) either overwrites the
original paths or writes an isolated copy under `dir`, preserving the path
relative to the snapshot root.

**Retention / GC** (`run_gc`) applies two rules:

1. Per-file history is trimmed to `retention_versions` (newest kept).
2. Snapshots older than `retention_days` are removed.

Then it computes which manifests are still referenced by surviving history or
snapshots, deletes unreferenced manifests, and finally sweeps object-store
chunks not referenced by any surviving manifest. Content-addressed orphans from
interrupted writes are reclaimed here, which is safe because no manifest points
at them.

## 11. Web Gateway

`web_gateway` is a separate loopback-only HTTP process. It serves a
self-contained dashboard and forwards `/api/*` requests to the daemon over the
same Unix-socket JSON protocol used by `cvctl`. It holds no state of its own,
so the daemon remains the single source of truth.

## 12. Comparison to Related Systems

| Feature | CrashVault | Git | ext4 journaling | ZFS snapshots |
|---|---|---|---|---|
| Automatic (no user action) | ✓ | ✗ (need commit) | ✓ (FS-level) | ✓ (zfs snapshot) |
| Sub-file dedup | ✓ (CDC) | Partial (pack files) | ✗ | ✗ |
| Crash recovery | WAL + fsync | Rerere / reflog | Journal replay | CoW + txg |
| Cross-file consistency | Snapshots | ✓ (commits) | ✗ | ✓ |
| Linux-only | ✓ | ✗ | ✓ | ✓ |
| Advisory lock limitation | ✓ | ✗ | N/A | N/A |
