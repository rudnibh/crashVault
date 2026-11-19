// src/store/manifest.h — Per-file version manifests and project snapshots
//
// MANIFEST: records a specific version of one file
//   { "path": "/abs/path", "version": 3, "size": 12345,
//     "mtime": 1700000000, "mode": 33188, "sha256": "<whole-file>",
//     "chunks": ["sha256_1", "sha256_2", ...], "timestamp": 1700000001 }
//
// SNAPSHOT: a consistent set of {path -> manifest_id} for a project root
//   { "id": "uuid", "root": "/project", "timestamp": 1700000001,
//     "files": {"/project/main.cc": "manifest_uuid", ...} }
//
// WHY JSON for manifests: human-readable, easy to debug, no binary parsing.
// The WAL records the commit of each manifest/snapshot atomically.
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cstddef>
#include <ctime>

namespace cv {

struct Manifest {
    std::string              id;         // UUID v4
    std::string              path;       // absolute path to the file
    uint32_t                 version;    // monotonically increasing per path
    size_t                   file_size;
    time_t                   mtime;
    mode_t                   mode;
    std::string              sha256;     // whole-file SHA-256
    std::vector<std::string> chunks;     // ordered list of chunk SHA-256s
    time_t                   timestamp;  // when this version was stored

    // Serialise to JSON string
    std::string to_json() const;

    // Deserialise from JSON string; returns false on parse failure
    bool from_json(const std::string &json_str);
};

struct Snapshot {
    std::string                              id;         // UUID v4
    std::string                              root;       // watched root path
    time_t                                   timestamp;
    std::unordered_map<std::string,std::string> files;  // path -> manifest_id

    std::string to_json() const;
    bool from_json(const std::string &json_str);
};

// ── Manifest store ───────────────────────────────────────────────────────────
// Writes manifests and snapshots to disk (temp + fsync + rename pattern)
class ManifestStore {
public:
    explicit ManifestStore(const std::string &store_root);

    // Write a manifest; returns false on I/O error
    bool write_manifest(const Manifest &m);

    // Read a manifest by id
    bool read_manifest(const std::string &id, Manifest &out) const;

    // Check if manifest exists
    bool has_manifest(const std::string &id) const;

    // Write a snapshot
    bool write_snapshot(const Snapshot &s);

    // Read a snapshot by id
    bool read_snapshot(const std::string &id, Snapshot &out) const;

    // List all snapshot ids (sorted by timestamp)
    std::vector<std::string> list_snapshots() const;

    // Per-path version history: path -> [manifest_id, ...] in chronological order
    // Stored in meta/file_history.json (read on startup, written on each new version)
    bool append_version(const std::string &path, const std::string &manifest_id);
    std::vector<std::string> get_history(const std::string &path) const;

    // Verify all manifests: every chunk exists and hashes correctly
    // Returns list of paths with corruption
    std::vector<std::string> verify_all(const std::string &objects_root) const;

private:
    bool write_json_file(const std::string &path, const std::string &json) const;
    bool read_json_file(const std::string &path, std::string &out) const;

    std::string store_root_;
    std::string manifests_dir_;
    std::string snapshots_dir_;
    std::string history_path_;
};

// Generate a new UUID v4 string (8-4-4-4-12 hex format)
std::string generate_uuid();

} // namespace cv
