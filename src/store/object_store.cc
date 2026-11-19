// src/store/object_store.cc — Content-addressed chunk store implementation
#include "object_store.h"
#include "../../vendor/sha256.h"
#include "../daemon/logger.h"
#include "../daemon/failpoint.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <functional>

namespace cv {

ObjectStore::ObjectStore(const std::string &store_root)
    : store_root_(store_root)
    , objects_root_(store_root + "/objects")
{
    // Ensure objects/ root exists
    mkdir(objects_root_.c_str(), 0755);
}

std::string ObjectStore::chunk_path(const std::string &sha256) const {
    // Shard layout: objects/<2hex>/<62hex>
    // WHY: Having all chunks in one directory would create a directory with
    // potentially millions of entries, slowing readdir/stat significantly.
    // Sharding by first 2 hex chars gives 256 directories, capping entries
    // per directory at ~N/256 (e.g. 100K chunks → ~390 per shard).
    return objects_root_ + "/" + sha256.substr(0, 2) + "/" + sha256.substr(2);
}

bool ObjectStore::ensure_shard_dir(const std::string &sha256) const {
    std::string dir = objects_root_ + "/" + sha256.substr(0, 2);
    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("mkdir shard " + dir + ": " + strerror(errno));
        return false;
    }
    return true;
}

bool ObjectStore::has_chunk(const std::string &sha256) const {
    return access(chunk_path(sha256).c_str(), F_OK) == 0;
}

bool ObjectStore::write_chunk(const std::string &sha256,
                              const uint8_t *data, size_t size) {
    // Fast path: chunk already exists (dedup hit — no I/O needed)
    if (has_chunk(sha256)) return false;

    if (!ensure_shard_dir(sha256)) return false;

    std::string final_path = chunk_path(sha256);
    // Temp file in the same directory (same filesystem = atomic rename possible).
    // Include the thread id: two workers can race on the same chunk, and a
    // pid-only temp name would make one of them mistake the other's in-progress
    // temp file for a completed chunk (and then reference a chunk that may
    // never be renamed into place if that writer fails).
    std::string tmp_path = final_path + ".tmp." + std::to_string(getpid()) +
                           "." + std::to_string(
                               std::hash<std::thread::id>{}(std::this_thread::get_id()));

    // ── Step 1: Write to temp file ────────────────────────────────────────────
    // O_EXCL: fail if temp file already exists (avoid stomping concurrent write)
    // O_CLOEXEC: don't leak fd to child processes
    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
    if (fd < 0) {
        // Another worker may be writing the same chunk concurrently — that's fine
        if (errno == EEXIST) return false;
        LOG_ERROR("open tmp " + tmp_path + ": " + strerror(errno));
        return false;
    }

    ssize_t written = write(fd, data, size);
    if (written < 0 || (size_t)written != size) {
        LOG_ERROR("write chunk " + sha256 + ": " + strerror(errno));
        close(fd);
        unlink(tmp_path.c_str());
        return false;
    }

    CV_FAILPOINT("object_after_write");

    // ── Step 2: fsync — flush data to stable storage ──────────────────────────
    // WHY: Without fsync, the kernel may buffer the data. If the system crashes
    // before the buffer is flushed, the temp file contents are lost. We must
    // ensure the data is on disk before we rename.
    if (fsync(fd) != 0) {
        LOG_ERROR("fsync chunk " + sha256 + ": " + strerror(errno));
        close(fd);
        unlink(tmp_path.c_str());
        return false;
    }
    close(fd);

    CV_FAILPOINT("object_after_fsync");

    // ── Step 3: Atomic rename into final position ─────────────────────────────
    // WHY rename is atomic: POSIX guarantees rename(2) is atomic on the same
    // filesystem — the directory entry atomically switches from old to new.
    // A reader either sees the old file or the new file, never partial content.
    if (rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        LOG_ERROR("rename chunk " + sha256 + ": " + strerror(errno));
        unlink(tmp_path.c_str());
        return false;
    }

    CV_FAILPOINT("object_after_rename");

    // ── Step 4: fsync the directory ───────────────────────────────────────────
    // WHY: The rename updated the directory entry in memory. Without fsync(dir),
    // a crash after rename but before the directory write-back could leave the
    // inode allocated but no directory entry pointing to it (lost data).
    std::string shard_dir = objects_root_ + "/" + sha256.substr(0, 2);
    int dir_fd = open(shard_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        fsync(dir_fd); // ignore error — best effort; WAL will catch missing chunks
        close(dir_fd);
    }

    LOG_DEBUG("Stored chunk " + sha256.substr(0, 16) + "... (" +
              std::to_string(size) + " bytes)");
    return true;
}

bool ObjectStore::read_chunk(const std::string &sha256,
                             std::vector<uint8_t> &out) {
    std::string path = chunk_path(sha256);
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    struct stat st{};
    if (fstat(fd, &st) != 0) { close(fd); return false; }

    out.resize(static_cast<size_t>(st.st_size));
    ssize_t n = read(fd, out.data(), out.size());
    close(fd);
    if (n < 0 || (size_t)n != out.size()) return false;
    return true;
}

bool ObjectStore::verify_chunk(const std::string &sha256) const {
    std::string path = chunk_path(sha256);
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    struct stat st{};
    if (fstat(fd, &st) != 0) { close(fd); return false; }

    std::vector<uint8_t> data(static_cast<size_t>(st.st_size));
    ssize_t n = read(fd, data.data(), data.size());
    close(fd);
    if (n < 0 || (size_t)n != data.size()) return false;

    std::string actual = sha256_hex(data.data(), data.size());
    return actual == sha256;
}

} // namespace cv
