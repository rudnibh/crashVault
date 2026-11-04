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
| flock(LOCK_SH\|LOCK_NB) | `src/pipeline/worker.cc` | `safe_read_file` | Write while holding exclusive lock; check "skipped-unstable" in log |
| mmap | `src/store/chunker.cc` | `MmapChunker::open` | Chunk a file >1 MiB; check `/proc/<pid>/maps` for the mapping |
| WAL (write-ahead log) | `src/store/wal.cc` | `WAL::append` | Kill -9 mid-write; restart; run `cvctl verify` |
| fsync | `src/store/object_store.cc` | `write_object` | Power-off test; chunk survives reboot |
| Atomic rename | `src/store/object_store.cc` | `write_object` | `strace -e rename ./crashvaultd --foreground` |
| IPC (Unix socket) | `src/ipc/socket_server.cc` | `SocketServer::serve_loop` | `socat - UNIX-CONNECT:~/.crashvault/crashvaultd.sock` |
| Scheduling policy | `src/pipeline/bounded_queue.cc` | `BoundedQueue::set_policy` | `cvctl set-policy priority`; watch latency change |
| GC | `src/store/gc.cc` | `GarbageCollector::run` | `cvctl gc`; storage usage drops |

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

```
┌──────────┬────────┬────────┬──────────────┬──────────┐
│ length   │  type  │  txid  │   payload    │  CRC32   │
│ (4 bytes)│(1 byte)│(8 bytes│ (length-13 B)│ (4 bytes)│
└──────────┴────────┴────────┴──────────────┴──────────┘
```

**Types:**
- `0x01` BEGIN
- `0x02` OP_ADD_MANIFEST
- `0x03` OP_ADD_SNAPSHOT
- `0x04` OP_DELETE_VERSION
- `0x05` OP_GC_SWEEP
- `0x06` COMMIT
- `0x07` CHECKPOINT

**Recovery algorithm:**
1. Scan records from last checkpoint position.
2. For each record: verify CRC32. On mismatch, truncate from that position (torn write).
3. Collect complete transactions (BEGIN...COMMIT pairs).
4. Redo committed transactions idempotently (re-applying a completed write is a no-op because we check for existing objects before writing).
5. Roll back incomplete transactions (remove any objects written without a COMMIT).

**Crash-consistency argument:**

| Step | Ordering guarantee | Effect of crash |
|---|---|---|
| 1. Write chunk to temp file | No guarantee | Temp file garbage-collected on restart |
| 2. fsync(temp_fd) | Data in stable storage | Chunk survives |
| 3. rename(temp, objects/ab/cdef) | Atomic on same filesystem | Chunk either fully present or absent |
| 4. fsync(dir_fd) | Directory entry durable | Chunk visible after reboot |
| 5. WAL: BEGIN + OP | Log durable | Can redo or ignore |
| 6. fsync(wal_fd) | WAL record durable | Recovery knows to redo |
| 7. WAL: COMMIT | Commit durable | Transaction committed |
| 8. Update in-memory index | No I/O | Re-built from WAL on restart |

A crash between steps 3 and 7 leaves a dangling chunk. This is safe: the chunk is unreferenced and will be collected by GC. No committed version is ever lost.

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
│   ├── wal.log           # Append-only WAL
│   └── wal.checkpoint    # 8-byte LSN of last checkpoint
│
├── objects/
│   └── ab/               # First 2 hex chars of SHA-256
│       └── cdef...       # Remaining 62 hex chars = chunk data
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

**Global atomics:** `g_shutdown_requested`, `g_reload_requested` are `std::atomic<bool>`. All accesses use correct memory order. **No race.**

### Starvation prevention (Priority policy)

Large files get low priority (high priority number). Without aging, small files would perpetually starve large ones. Aging: items waiting longer than `AGING_THRESHOLD_MS` (5s) are promoted to priority 0. This bounds wait time.

---

## 9. Memory Management

**Buffer pool:** Fixed-size blocks (default 1 MiB) pre-allocated up to `memory_cap_bytes`. Workers request a block, process a chunk, return the block. If no block is available, the worker sleeps until one is returned. This bounds total memory regardless of file sizes.

**mmap:** Files larger than `mmap_threshold_bytes` are mapped with `mmap(PROT_READ, MAP_PRIVATE)`. The kernel pages in data on demand; we never hold the entire file in memory at once.

**LRU chunk hash cache:** Recently seen chunk SHA-256s are cached in an LRU to avoid re-hashing chunks we've already stored. Cache size is bounded at construction.

---

## 10. Comparison to Related Systems

| Feature | CrashVault | Git | ext4 journaling | ZFS snapshots |
|---|---|---|---|---|
| Automatic (no user action) | ✓ | ✗ (need commit) | ✓ (FS-level) | ✓ (zfs snapshot) |
| Sub-file dedup | ✓ (CDC) | Partial (pack files) | ✗ | ✗ |
| Crash recovery | WAL + fsync | Rerere / reflog | Journal replay | CoW + txg |
| Cross-file consistency | Snapshots | ✓ (commits) | ✗ | ✓ |
| Linux-only | ✓ | ✗ | ✓ | ✓ |
| Advisory lock limitation | ✓ | ✗ | N/A | N/A |
