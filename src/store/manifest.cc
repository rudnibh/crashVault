// src/store/manifest.cc — Manifest and snapshot serialization + store
#include "manifest.h"
#include "object_store.h"
#include "../../vendor/sha256.h"
#include "../daemon/logger.h"
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <random>
#include <iomanip>
#include <algorithm>
#include <mutex>

using json = nlohmann::json;

namespace cv {

// ── UUID generation ──────────────────────────────────────────────────────────
std::string generate_uuid() {
    // WHY: We use /dev/urandom for UUID v4 randomness.
    // A proper UUID v4 has specific bits set for version and variant.
    std::random_device rd;
    std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    uint32_t a = dist(rd), b = dist(rd), c = dist(rd), d = dist(rd);

    // Set version 4 (0100) and variant 10xx
    b = (b & 0xFFFF0FFF) | 0x00004000; // version 4
    c = (c & 0x3FFFFFFF) | 0x80000000; // variant 10xx

    char buf[37];
    snprintf(buf, sizeof(buf),
        "%08x-%04x-%04x-%04x-%04x%08x",
        a,
        (b >> 16) & 0xFFFF,
        b & 0xFFFF,
        (c >> 16) & 0xFFFF,
        c & 0xFFFF,
        d);
    return buf;
}

// ── Manifest serialization ───────────────────────────────────────────────────
std::string Manifest::to_json() const {
    json j;
    j["id"]        = id;
    j["path"]      = path;
    j["version"]   = version;
    j["file_size"] = file_size;
    j["mtime"]     = (int64_t)mtime;
    j["mode"]      = (uint32_t)mode;
    j["sha256"]    = sha256;
    j["chunks"]    = chunks;
    j["timestamp"] = (int64_t)timestamp;
    return j.dump(2);
}

bool Manifest::from_json(const std::string &json_str) {
    try {
        json j  = json::parse(json_str);
        id        = j.at("id").get<std::string>();
        path      = j.at("path").get<std::string>();
        version   = j.at("version").get<uint32_t>();
        file_size = j.at("file_size").get<size_t>();
        mtime     = (time_t)j.at("mtime").get<int64_t>();
        mode      = (mode_t)j.at("mode").get<uint32_t>();
        sha256    = j.at("sha256").get<std::string>();
        chunks    = j.at("chunks").get<std::vector<std::string>>();
        timestamp = (time_t)j.at("timestamp").get<int64_t>();
        return true;
    } catch (...) {
        return false;
    }
}

// ── Snapshot serialization ───────────────────────────────────────────────────
std::string Snapshot::to_json() const {
    json j;
    j["id"]        = id;
    j["root"]      = root;
    j["timestamp"] = (int64_t)timestamp;
    j["files"]     = files;
    return j.dump(2);
}

bool Snapshot::from_json(const std::string &json_str) {
    try {
        json j  = json::parse(json_str);
        id        = j.at("id").get<std::string>();
        root      = j.at("root").get<std::string>();
        timestamp = (time_t)j.at("timestamp").get<int64_t>();
        files     = j.at("files").get<std::unordered_map<std::string,std::string>>();
        return true;
    } catch (...) {
        return false;
    }
}

// ── ManifestStore ────────────────────────────────────────────────────────────
ManifestStore::ManifestStore(const std::string &store_root)
    : store_root_(store_root)
    , manifests_dir_(store_root + "/manifests")
    , snapshots_dir_(store_root + "/snapshots")
    , history_path_(store_root + "/meta/file_history.json")
{
    mkdir(manifests_dir_.c_str(), 0755);
    mkdir(snapshots_dir_.c_str(), 0755);
    mkdir((store_root + "/meta").c_str(), 0755);
}

// Write a JSON file using temp + fsync + rename (same crash-safe pattern as chunks)
bool ManifestStore::write_json_file(const std::string &path,
                                     const std::string &json_content) const {
    // Unique temp path: history writes are serialized, but manifests and
    // snapshots may be written concurrently. mkstemp also protects against
    // stale temp files left by a previous process that had the same PID.
    std::string tmp_template = path + ".tmp.XXXXXX";
    std::vector<char> tmp_buf(tmp_template.begin(), tmp_template.end());
    tmp_buf.push_back('\0');
    int fd = mkstemp(tmp_buf.data());
    if (fd < 0) return false;
    std::string tmp(tmp_buf.data());
    int fd_flags = fcntl(fd, F_GETFD);
    if (fd_flags >= 0) fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);

    size_t written = 0;
    while (written < json_content.size()) {
        ssize_t n = write(fd, json_content.data() + written,
                          json_content.size() - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd); unlink(tmp.c_str()); return false;
        }
        written += static_cast<size_t>(n);
    }
    if (fchmod(fd, 0644) != 0) {
        close(fd); unlink(tmp.c_str()); return false;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp.c_str()); return false; }
    close(fd);

    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str()); return false;
    }

    // fsync the containing directory so the rename itself is durable.
    size_t slash = path.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
    int dir_fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        fsync(dir_fd); // best effort
        close(dir_fd);
    }
    return true;
}

bool ManifestStore::read_json_file(const std::string &path,
                                    std::string &out) const {
    std::ifstream f(path);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f),
               std::istreambuf_iterator<char>());
    return !out.empty();
}

bool ManifestStore::write_manifest(const Manifest &m) {
    std::string path = manifests_dir_ + "/" + m.id + ".json";
    return write_json_file(path, m.to_json());
}

bool ManifestStore::read_manifest(const std::string &id, Manifest &out) const {
    std::string json_str;
    if (!read_json_file(manifests_dir_ + "/" + id + ".json", json_str)) return false;
    return out.from_json(json_str);
}

bool ManifestStore::has_manifest(const std::string &id) const {
    return access((manifests_dir_ + "/" + id + ".json").c_str(), F_OK) == 0;
}

bool ManifestStore::write_snapshot(const Snapshot &s) {
    std::string path = snapshots_dir_ + "/" + s.id + ".json";
    return write_json_file(path, s.to_json());
}

bool ManifestStore::read_snapshot(const std::string &id, Snapshot &out) const {
    std::string json_str;
    if (!read_json_file(snapshots_dir_ + "/" + id + ".json", json_str)) return false;
    return out.from_json(json_str);
}

std::vector<std::string> ManifestStore::list_snapshots() const {
    std::vector<std::pair<time_t, std::string>> entries;
    DIR *dir = opendir(snapshots_dir_.c_str());
    if (!dir) return {};

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name.size() < 5 || name.substr(name.size()-5) != ".json") continue;
        std::string id = name.substr(0, name.size()-5);
        Snapshot s;
        if (read_snapshot(id, s)) {
            entries.push_back({s.timestamp, id});
        }
    }
    closedir(dir);

    std::sort(entries.begin(), entries.end());
    std::vector<std::string> ids;
    for (auto &[ts, id] : entries) ids.push_back(id);
    return ids;
}

// History is stored as { "/abs/path": ["manifest_id_1", "manifest_id_2", ...] }
// Protected by a mutex since multiple workers may append concurrently
static std::mutex g_history_mutex;

bool ManifestStore::append_version(const std::string &path,
                                    const std::string &manifest_id) {
    std::lock_guard<std::mutex> lk(g_history_mutex);

    // Load existing history
    json history = json::object();
    std::string existing;
    if (read_json_file(history_path_, existing)) {
        try { history = json::parse(existing); } catch (...) {}
    }

    // Append the new manifest_id. WHY the idempotency check: WAL replay may
    // re-append a manifest whose history update already succeeded before the
    // crash. Duplicate entries would create phantom versions.
    if (!history.contains(path)) history[path] = json::array();
    for (const auto &existing_id : history[path]) {
        if (existing_id.get<std::string>() == manifest_id) return true;
    }
    history[path].push_back(manifest_id);

    return write_json_file(history_path_, history.dump(2));
}

bool ManifestStore::set_history(const std::string &path,
                                const std::vector<std::string> &manifest_ids) {
    std::lock_guard<std::mutex> lk(g_history_mutex);

    json history = json::object();
    std::string existing;
    if (read_json_file(history_path_, existing)) {
        try { history = json::parse(existing); } catch (...) {}
    }

    if (manifest_ids.empty()) history.erase(path);
    else history[path] = manifest_ids;

    return write_json_file(history_path_, history.dump(2));
}

std::vector<std::string> ManifestStore::list_history_paths() const {
    std::lock_guard<std::mutex> lk(g_history_mutex);
    std::vector<std::string> paths;
    std::string existing;
    if (!read_json_file(history_path_, existing)) return paths;
    try {
        json h = json::parse(existing);
        for (auto it = h.begin(); it != h.end(); ++it) paths.push_back(it.key());
    } catch (...) {}
    std::sort(paths.begin(), paths.end());
    return paths;
}

std::vector<std::string> ManifestStore::get_history(const std::string &path) const {
    std::lock_guard<std::mutex> lk(g_history_mutex);
    std::string existing;
    if (!read_json_file(history_path_, existing)) return {};
    try {
        json h = json::parse(existing);
        if (h.contains(path))
            return h[path].get<std::vector<std::string>>();
    } catch (...) {}
    return {};
}

bool ManifestStore::delete_manifest(const std::string &id) {
    if (id.empty() || id.find('/') != std::string::npos) return false;
    return unlink((manifests_dir_ + "/" + id + ".json").c_str()) == 0;
}

bool ManifestStore::delete_snapshot(const std::string &id) {
    if (id.empty() || id.find('/') != std::string::npos) return false;
    return unlink((snapshots_dir_ + "/" + id + ".json").c_str()) == 0;
}

std::vector<std::string> ManifestStore::list_manifest_ids() const {
    std::vector<std::string> ids;
    DIR *dir = opendir(manifests_dir_.c_str());
    if (!dir) return ids;

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name.size() < 5 || name.substr(name.size()-5) != ".json") continue;
        ids.push_back(name.substr(0, name.size()-5));
    }
    closedir(dir);
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool ManifestStore::create_snapshot(const std::string &root, Snapshot &out) {
    // Exclusive: a snapshot is a point-in-time mapping across every tracked
    // file, so no versioning/restore may be halfway through while we collect
    // the newest committed manifest for each path.
    std::unique_lock<std::shared_mutex> store_lock(store_mutex_);

    out.id        = generate_uuid();
    out.root      = root;
    out.timestamp = time(nullptr);
    out.files.clear();

    const std::string prefix = root.empty() ? "" :
        (root.back() == '/' ? root : root + "/");

    for (const auto &path : list_history_paths()) {
        if (!prefix.empty() && path.rfind(prefix, 0) != 0) continue;
        auto hist = get_history(path);
        if (hist.empty()) continue;
        // Use the newest manifest that still exists on disk.
        for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
            if (has_manifest(*it)) {
                out.files[path] = *it;
                break;
            }
        }
    }

    return write_snapshot(out);
}

// Verify one content-addressed file: open, read, re-hash, compare.
static bool verify_file_hash(const std::string &path,
                             const std::string &expected_sha,
                             std::string &error) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { error = "missing"; return false; }

    struct stat st{};
    if (fstat(fd, &st) != 0) { close(fd); error = "stat failed"; return false; }

    std::vector<uint8_t> data(static_cast<size_t>(st.st_size));
    size_t got = 0;
    while (got < data.size()) {
        ssize_t n = read(fd, data.data() + got, data.size() - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); error = "short read"; return false; }
        got += static_cast<size_t>(n);
    }
    close(fd);

    std::string actual = sha256_hex(data.data(), data.size());
    if (actual != expected_sha) { error = "hash mismatch"; return false; }
    return true;
}

std::vector<std::string> ManifestStore::verify_all(
        const std::string &objects_root) const {
    std::shared_lock<std::shared_mutex> store_lock(store_mutex_);
    std::vector<std::string> corrupt;

    // Validate the history file's structure explicitly. The normal history
    // accessors fail soft (returning an empty map) so one corrupt file does not
    // take the daemon down, but verification must still report it.
    if (access(history_path_.c_str(), F_OK) == 0) {
        std::string raw;
        bool history_ok = read_json_file(history_path_, raw);
        if (history_ok) {
            try {
                json h = json::parse(raw);
                if (!h.is_object()) history_ok = false;
                else {
                    for (auto it = h.begin(); it != h.end(); ++it) {
                        if (!it.value().is_array()) { history_ok = false; break; }
                        for (const auto &id : it.value()) {
                            if (!id.is_string()) { history_ok = false; break; }
                        }
                        if (!history_ok) break;
                    }
                }
            } catch (...) {
                history_ok = false;
            }
        }
        if (!history_ok)
            corrupt.push_back("corrupt history JSON: meta/file_history.json");
    }

    DIR *dir = opendir(manifests_dir_.c_str());
    if (!dir) return corrupt;

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name.size() < 5 || name.substr(name.size()-5) != ".json") continue;
        std::string id = name.substr(0, name.size()-5);
        Manifest m;
        if (!read_manifest(id, m)) {
            corrupt.push_back("corrupt manifest JSON: " + id);
            continue;
        }

        for (auto &ch : m.chunks) {
            if (ch.size() != 64) {
                corrupt.push_back(m.path + " (malformed chunk id)");
                continue;
            }
            std::string chunk_path = objects_root + "/" +
                                     ch.substr(0,2) + "/" + ch.substr(2);
            std::string error;
            if (!verify_file_hash(chunk_path, ch, error)) {
                corrupt.push_back(m.path + " (chunk " + ch.substr(0,16) +
                                  "...: " + error + ")");
            }
        }
    }
    closedir(dir);

    // A history entry pointing at a missing manifest is also corruption.
    for (const auto &path : list_history_paths()) {
        for (const auto &id : get_history(path)) {
            if (!has_manifest(id))
                corrupt.push_back(path + " (history references missing manifest " +
                                  id + ")");
        }
    }

    // Snapshots are part of the durable state too: a JSON file that no longer
    // parses, or a manifest reference that has disappeared, is corruption.
    for (const auto &snap_id : list_snapshots()) {
        Snapshot snap;
        if (!read_snapshot(snap_id, snap)) {
            corrupt.push_back("corrupt snapshot JSON: " + snap_id);
            continue;
        }
        for (const auto &[path, manifest_id] : snap.files) {
            (void)path;
            if (!has_manifest(manifest_id))
                corrupt.push_back("snapshot " + snap_id +
                                  " references missing manifest " + manifest_id);
        }
    }
    return corrupt;
}

} // namespace cv
