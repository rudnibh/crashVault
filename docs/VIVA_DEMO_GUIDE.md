# CrashVault — Viva & Demo Guide

This guide is written for a project evaluation/viva. It gives a repeatable
demo, a short version for time-constrained evaluations, and answers to the
questions examiners usually ask about the design.

---

## 1. Clean Build Check

```bash
cd /path/to/crashvault

cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"

# Everything: 2 unit suites + 4 integration suites + 2 demos
ctest --test-dir build --output-on-failure
```

Expected result:

```
100% tests passed, 0 tests failed out of 8
```

The build uses `-Wall -Wextra -Werror`, so a successful build is also evidence
that the project compiles warning-free.

For a clean-store manual demo, create a temporary store and watched folder:

```bash
rm -rf /tmp/cv-viva
mkdir -p /tmp/cv-viva/store /tmp/cv-viva/watched

cat > /tmp/cv-viva/store/config.ini <<'EOF'
[daemon]
worker_count = 2
debounce_ms = 150
scheduling_policy = priority
snapshot_quiescence_s = 1
memory_cap_mb = 128

[watch]
root = /tmp/cv-viva/watched

[retention]
versions = 10
days = 30
EOF

./build/crashvaultd --foreground --debug \
  --store /tmp/cv-viva/store \
  --config /tmp/cv-viva/store/config.ini \
  --log /tmp/cv-viva/daemon.log
```

Leave that running in **Terminal 1**. In **Terminal 2**:

```bash
CV="./build/cvctl --socket /tmp/cv-viva/store/crashvaultd.sock"

$CV status
$CV ping
```

The most important status fields are `tracked_files`, `versions_stored`,
`queue_depth`, `pending_wal`, and `read_budget`.

---

## 2. Ten-Minute Demo (recommended)

### 2.1 Start and show the watcher (1 minute)

With the daemon from Section 1 running:

```bash
printf 'version one\n' > /tmp/cv-viva/watched/notes.txt
sleep 1
./build/cvctl --socket /tmp/cv-viva/store/crashvaultd.sock list
```

**Say:** “The daemon uses inotify through epoll, debounces burst writes, and
pushes one settled event per file into a bounded producer-consumer queue.”

### 2.2 Show versioning and history (1 minute)

```bash
printf 'version two\n' > /tmp/cv-viva/watched/notes.txt
sleep 1

./build/cvctl --socket /tmp/cv-viva/store/crashvaultd.sock \
  history /tmp/cv-viva/watched/notes.txt
```

**Say:** “Each change becomes a manifest. Files are split with FastCDC,
chunks are stored by SHA-256, and identical chunks are deduplicated
automatically.”

### 2.3 Show restore and verify (2 minutes)

```bash
./build/cvctl --socket /tmp/cv-viva/store/crashvaultd.sock \
  restore /tmp/cv-viva/watched/notes.txt 1 --dest /tmp/cv-viva/restored.txt

cat /tmp/cv-viva/restored.txt

./build/cvctl --socket /tmp/cv-viva/store/crashvaultd.sock verify
```

**Say:** “Restore reassembles the file from chunks, re-hashes every chunk,
fsyncs a temp file, restores its mode/mtime, and atomically renames it into
place. `verify` re-hashes the whole store and checks history/snapshot
references.”

### 2.4 Show snapshots and snapshot restore (1 minute)

```bash
./build/cvctl --socket /tmp/cv-viva/store/crashvaultd.sock \
  snapshot-now /tmp/cv-viva/watched

./build/cvctl --socket /tmp/cv-viva/store/crashvaultd.sock snapshots

# Use the id printed above:
./build/cvctl --socket /tmp/cv-viva/store/crashvaultd.sock \
  restore-snapshot <SNAPSHOT_ID> /tmp/cv-viva/snapshot-restore

find /tmp/cv-viva/snapshot-restore -type f -print
```

**Say:** “A snapshot maps every tracked path to its newest valid manifest.
Restoring a snapshot is therefore a point-in-time project restore, not a
per-file operation.”

### 2.5 Show GC and retention (1 minute)

```bash
./build/cvctl --socket /tmp/cv-viva/store/crashvaultd.sock gc
```

**Say:** “GC trims history to the retention limit, deletes unreferenced
manifests, sweeps unreferenced chunks, and removes stale temp files from
crashed writers. A manifest referenced by any snapshot is never deleted.”

### 2.6 Show crash consistency (2 minutes)

The automated demo is the clearest way to show this:

```bash
./scripts/crash_demo.sh 9
```

Expected: nine runs, each crashing at a different point in the write protocol,
then restarting and verifying that committed versions survived and the daemon
still works.

For the full stress result:

```bash
./scripts/crash_demo.sh 100
# Expected: Crash consistency: 100/100 runs passed
```

**Say:** “The crash demo injects deterministic failpoints at chunk-write,
WAL-prepare, manifest, commit, history, and complete stages. After each crash
the WAL is replayed and the store is verified.”

### 2.7 Show the dashboard (1 minute)

In Terminal 2:

```bash
./build/web_gateway \
  --socket /tmp/cv-viva/store/crashvaultd.sock \
  --port 8080
```

Open `http://127.0.0.1:8080`.

**Say:** “The gateway is loopback-only. It forwards `/api/*` calls to the
daemon over the Unix socket, so the HTTP layer cannot bypass daemon state.”

---

## 3. Two-Minute Elevator Demo

If the evaluator has very little time, run only:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./scripts/demo.sh
```

The script demonstrates versioning, history, snapshots, restore, verify, GC,
deterministic crash, WAL replay, and graceful shutdown in one run. Finish with:

```bash
./scripts/crash_demo.sh 9
```

That gives a complete functional and crash-consistency story in under two
minutes on a normal Linux machine.

---

## 4. Design Walkthrough Map

Use this when the examiner asks “show me where that happens.”

| Topic | File | Function |
|---|---|---|
| Daemonization, pid file | `src/daemon/daemonize.cc` | `cv::daemonize` |
| Self-pipe signals | `src/daemon/signal_handler.cc` | `signal_handler_init`, `signal_handler_drain` |
| Config parsing/reload | `src/daemon/config.cc` | `Config::load`, `Config::reload` |
| inotify + epoll | `src/watcher/inotify_watcher.cc` | `InotifyWatcher::event_loop` |
| Debouncing | `src/watcher/debouncer.cc` | `Debouncer::scan_loop` |
| Bounded queue | `src/pipeline/bounded_queue.cc` | `BoundedQueue::push/pop` |
| Worker scheduling | `src/pipeline/worker_pool.cc` | `WorkerPool::worker_main` |
| Memory budget | `src/pipeline/buffer_pool.cc` | `BufferPool::acquire/release` |
| FastCDC chunking | `src/store/chunker.cc` | `FastCDCChunker::chunk` |
| Chunk storage/dedup | `src/store/object_store.cc` | `ObjectStore::put_chunk` |
| Manifests/history/snapshots | `src/store/manifest.cc` | `ManifestStore` |
| Versioning protocol | `src/store/versioner.cc` | `Versioner::process` |
| WAL | `src/store/wal.cc` | `Wal::prepare/commit/complete/gc` |
| Startup recovery | `src/store/recovery.cc` | `recover_wal` |
| Retention/GC | `src/store/gc.cc` | `run_gc` |
| IPC | `src/ipc/socket_server.cc` | `SocketServer` |
| CLI | `src/cli/main.cc` | `cvctl` |
| Dashboard/HTTP | `src/web/gateway.cc` | `handle_client` |

---

## 5. Likely Viva Questions and Answers

### 5.1 Why is this an Operating Systems project?

It demonstrates file-system monitoring (inotify), I/O multiplexing (epoll),
daemonization, POSIX signals (self-pipe), producer-consumer synchronisation
(mutex + condition variables), file locking (`flock`), bounded-memory
scheduling, Unix-domain-socket IPC, durable writes (`fsync`, `rename`),
write-ahead logging, crash recovery, and garbage collection.

### 5.2 What exactly happens when a file changes?

1. inotify reports an event.
2. epoll wakes the watcher thread.
3. The debouncer waits for a quiet window so burst writes become one event.
4. The event enters the bounded queue.
5. A worker takes it, advisory-locks and reads the file while checking
   `stat` before/after (mtime nanoseconds + size).
6. FastCDC splits the file into chunks.
7. Each chunk is written to a temp file, `fsync`ed, renamed into place, and
   the shard directory is `fsync`ed.
8. The WAL records PREPARE.
9. The manifest is written atomically.
10. The WAL is advanced to COMMIT.
11. Per-path history is appended atomically.
12. The WAL is marked COMPLETE and the old `.wal` is removed.

### 5.3 Why are chunks written before the WAL PREPARE?

Because a manifest must never reference a chunk that is not durable. If we
logged PREPARE first and crashed after writing only some chunks, recovery would
have to decide whether a manifest is valid when its data is not. Writing chunks
first means the worst case is an unreferenced, content-addressed chunk, which
is harmless and reclaimable by GC. The WAL then records the intent to publish
a manifest for that already-durable chunk set.

### 5.4 What happens at each crash point?

| Crash point | Recovery behaviour |
|---|---|
| During chunk write / fsync / rename | Temp file is stale; GC removes it after one hour. No manifest exists. |
| After all chunks, before WAL PREPARE | Orphan chunks; GC reclaims them. No user-visible version. |
| After WAL PREPARE, before manifest | Orphan chunks; replay sees PREPARE with no manifest and discards the entry. |
| After manifest, before WAL COMMIT | Replay sees PREPARE plus an existing manifest, finishes the history append, and completes the entry. |
| After WAL COMMIT, before/after history | Replay sees COMMIT and re-appends history idempotently. |
| After history, before COMPLETE | Same as above; duplicate history entries are rejected by `append_version`. |
| After COMPLETE | Nothing to repair. |

This is what the nine-stage `scripts/crash_demo.sh` checks.

### 5.5 Why one WAL file per operation instead of one append-only log?

An append-only log needs truncation, rotation, and torn-tail/CRC handling.
With one file per operation, a state transition is just an atomic rename. A
completed entry is renamed to `.wal.done`, and completed files can be deleted
independently by GC. The design is easier to audit and test.

### 5.6 How does deduplication work?

The chunk hash is its address: `objects/<first-2-hex>/<remaining-62-hex>`.
Before writing, `has_chunk()` checks whether the object already exists. If it
does, the manifest only records the hash and no data is rewritten. FastCDC’s
content-defined boundaries mean an insertion near the start of a file only
changes local chunks instead of shifting every fixed-size block.

### 5.7 How is memory bounded?

`BufferPool` hands out byte leases. A worker must acquire a lease equal to the
file size before reading it. If the file is larger than the whole budget, the
pool reserves the entire budget and lets that one file run alone. This prevents
four workers from each reading a huge file at once.

### 5.8 How do you prevent reading a file while it is being written?

`flock(LOCK_SH | LOCK_NB)` is attempted. If a cooperating writer holds an
exclusive lock, the read backs off and retries. Since `flock` is only advisory,
the read also compares `fstat` before and after, including nanoseconds and file
size; if it changed, the version is abandoned as unstable and retried.

### 5.9 Why are chunks read-only (`0444`)?

Chunks are immutable by construction: their filename is their SHA-256. Making
them read-only protects against accidental modification. Tests explicitly
`chmod` before corrupting a chunk to exercise verification.

### 5.10 How does restore avoid writing a half-file?

Restore writes to a unique temp file next to the destination, re-hashes every
chunk as it reads it, `fchmod`s the recorded mode, `fsync`s, restores mtime,
then atomically renames over the destination and `fsync`s the directory. A
crash leaves either the old file or the complete new file, never a partial one.

### 5.11 What does GC protect?

GC first expires old snapshots, then trims per-file history to the retention
limit, then marks every manifest referenced by surviving history **or** by any
snapshot. Only unmarked manifests are deleted. It then marks every chunk
referenced by any surviving manifest and deletes unmarked chunks plus stale
temp files. This prevents deleting data that a snapshot still needs.

### 5.12 How are snapshots consistent?

A snapshot records the newest manifest that still exists for each tracked path
under the snapshot root. It is a project-level point-in-time mapping. GC treats
snapshot references as roots, so an old snapshot survives even if the file
history has moved on.

### 5.13 Why the self-pipe trick?

POSIX signal handlers may only call async-signal-safe functions. The handler
sets an atomic flag and writes one byte to a non-blocking pipe. The main loop
waits on that pipe with `poll(2)`, drains it in normal context, and then does
logging, config reload, or shutdown. This avoids calling `malloc`, `printf`, or
mutex code inside a signal handler.

### 5.14 Why is there a config mutex now?

The IPC server has multiple handler threads reading policy, retention, and
watch roots, while SIGHUP reloads configuration on the main thread. A
`std::mutex` protects the shared `Config`. Workers keep their own immutable
copies of startup settings, so versioning does not contend on it. TSan runs
cleanly over unit and integration tests.

### 5.15 What is intentionally *not* done?

- `flock` is advisory; non-cooperating writers are only detected by stat checks.
- Linux-only: inotify, epoll, eventfd.
- No encryption or remote replication.
- No hard links/special files.
- Dashboard has no authentication and is loopback-only.
- `SIGHUP` hot-reloads policy, retention, and quiescence; changing watched roots, ignore patterns, worker count, memory cap, debounce, store root, or gateway port requires a restart (the daemon logs this and keeps the running values).
- There is no automatic restore of a corrupt file; `verify` reports corruption
  and `restore` refuses to emit corrupt chunks. A user chooses an older valid
  version or restores a snapshot.

These are documented limitations, not silent gaps.

### 5.16 How do you know the code is correct?

- `-Wall -Wextra -Werror` build.
- `unit_phase1`: 15 test cases / 37 assertions.
- `unit_phase2`: 47 test cases / 603 assertions.
- Integration suites for watcher, daemon IPC/versioning/snapshots/GC, crash
  recovery, and web gateway.
- End-to-end demo and nine-stage crash demo as CTest targets.
- ASan+UBSan build: all 8 CTest suites pass.
- TSan build: all 8 CTest suites pass.
- 100/100 repeated crash-consistency runs passed.
- Release build passes with failpoints compiled out.

---

## 6. Useful Commands for the Evaluation

```bash
# Full test suite
ctest --test-dir build --output-on-failure

# One unit suite with details
./build/test_phase2

# ASan/UBSan
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=asan
cmake --build build-asan -j"$(nproc)"
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:abort_on_error=1 \
ctest --test-dir build-asan --output-on-failure

# TSan
cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=tsan
cmake --build build-tsan -j"$(nproc)"
TSAN_OPTIONS=second_deadlock_stack=1:abort_on_error=1 \
ctest --test-dir build-tsan --output-on-failure

# Benchmark
./build/bench_store

# End-to-end
./scripts/demo.sh
./scripts/crash_demo.sh 100
```

### Manual failpoint example

Start a daemon with a failpoint (RelWithDebInfo or Debug build):

```bash
CV_FAILPOINT=versioner_after_manifest ./build/crashvaultd \
  --foreground --store /tmp/cv-viva/store \
  --config /tmp/cv-viva/store/config.ini \
  --log /tmp/cv-viva/crash.log
```

Write a file. The daemon exits with code 90 after the manifest is durable but
before WAL COMMIT. Restart without the failpoint, then:

```bash
./build/cvctl --socket /tmp/cv-viva/store/crashvaultd.sock \
  history /tmp/cv-viva/watched/crash-test.txt
./build/cvctl --socket /tmp/cv-viva/store/crashvaultd.sock verify
```

The committed version is present and the store is healthy.

---

## 7. Troubleshooting During the Demo

| Symptom | Fix |
|---|---|
| `Cannot connect to daemon` | Use the same `--store` path and pass `--socket <store>/crashvaultd.sock` to `cvctl`. |
| No files appear | Wait for the debounce window (150–300 ms plus worker drain), or check that the path is under a configured root. |
| No inotify events | Do not use WSL `/mnt/c`; use a native Linux filesystem or `/tmp`. |
| Dashboard cannot reach daemon | Start `web_gateway` with the daemon’s actual `--socket` path. |
| Port 8080 busy | Use `--port 8081`. |
| Crash demo says build first | Run the build command from Section 1. |
| Stale socket after a manual kill | Remove `<store>/crashvaultd.sock` and restart. |
| Permission denied on demo script | `chmod +x scripts/*.sh` (the repository normally stores them executable). |

---

## 8. One-Sentence Closing Statement

“CrashVault is a Linux daemon that watches folders, stores deduplicated
content-addressed versions, groups them into snapshots, and uses a WAL with
fsync plus atomic rename to guarantee that a crash never loses a committed
version — all verified by unit, integration, sanitizer, and repeated
crash-injection tests.”
