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
    std::string tmp = path + ".tmp." + std::to_string(getpid());

    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;

    if (write(fd, json_content.data(), json_content.size()) !=
            (ssize_t)json_content.size()) {
        close(fd); unlink(tmp.c_str()); return false;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp.c_str()); return false; }
    close(fd);

    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str()); return false;
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
    ssize_t n = data.empty() ? 0 : read(fd, data.data(), data.size());
    close(fd);
    if (n < 0 || static_cast<size_t>(n) != data.size()) {
        error = "short read"; return false;
    }

    std::string actual = sha256_hex(data.data(), data.size());
    if (actual != expected_sha) { error = "hash mismatch"; return false; }
    return true;
}

std::vector<std::string> ManifestStore::verify_all(
        const std::string &objects_root) const {
    std::vector<std::string> corrupt;
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
    return corrupt;
}

} // namespace cv
