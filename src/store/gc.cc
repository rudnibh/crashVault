// src/store/gc.cc — Garbage collection implementation
#include "gc.h"
#include "manifest.h"
#include "object_store.h"
#include "../daemon/logger.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <ctime>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <shared_mutex>

namespace cv {

static bool is_sha_filename(const std::string &name) {
    if (name.size() != 62) return false;
    for (char c : name) {
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

// Remove stale temp files left by an interrupted atomic write in a flat
// metadata directory (manifests/, snapshots/, meta/). Only files older than an
// hour are touched, so a live writer's temp file is never removed.
static void sweep_metadata_temps(const std::string &dir, time_t now,
                                 GcStats &stats) {
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name.find(".tmp.") == std::string::npos) continue;
        std::string full = dir + "/" + name;
        struct stat st{};
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
            now - st.st_mtime > 3600) {
            if (unlink(full.c_str()) == 0) {
                stats.temp_files_deleted++;
                stats.bytes_reclaimed += (uint64_t)st.st_size;
            }
        }
    }
    closedir(d);
}

GcStats run_gc(ManifestStore &manifests, ObjectStore &objects,
               int retention_versions, int retention_days) {
    // Exclusive store gate: no version/restore/snapshot/verify may run while
    // we are deciding which objects are unreachable.
    std::unique_lock<std::shared_mutex> store_lock(manifests.store_mutex());
    GcStats stats;
    time_t now = time(nullptr);

    // Remove temp files left by interrupted metadata writes. The store gate is
    // exclusive here, so these cannot be live writer temp files.
    sweep_metadata_temps(manifests.store_root() + "/manifests", now, stats);
    sweep_metadata_temps(manifests.store_root() + "/snapshots", now, stats);
    sweep_metadata_temps(manifests.store_root() + "/meta", now, stats);

    // ── 1. Expire old snapshots ───────────────────────────────────────────────
    auto snapshot_ids = manifests.list_snapshots();
    time_t cutoff = now - (time_t)retention_days * 24 * 60 * 60;

    for (const auto &id : snapshot_ids) {
        Snapshot snap;
        if (!manifests.read_snapshot(id, snap)) {
            manifests.delete_snapshot(id);
            stats.snapshots_deleted++;
            continue;
        }
        if (retention_days > 0 && snap.timestamp < cutoff) {
            if (manifests.delete_snapshot(id)) stats.snapshots_deleted++;
        }
    }

    // ── 2. Trim per-file history ──────────────────────────────────────────────
    auto paths = manifests.list_history_paths();
    for (const auto &path : paths) {
        auto history = manifests.get_history(path);

        // Drop entries whose manifest file is gone (e.g. prior failed run).
        std::vector<std::string> existing;
        existing.reserve(history.size());
        for (const auto &id : history) {
            if (manifests.has_manifest(id)) existing.push_back(id);
        }

        if (retention_versions > 0 &&
            existing.size() > (size_t)retention_versions) {
            size_t remove = existing.size() - (size_t)retention_versions;
            stats.history_entries_trimmed += remove;
            existing.erase(existing.begin(), existing.begin() + remove);
        }

        if (existing != history) manifests.set_history(path, existing);
    }

    // ── 3. Delete manifests no longer referenced by history or snapshots ──────
    std::set<std::string> referenced_manifests;
    for (const auto &path : manifests.list_history_paths()) {
        for (const auto &id : manifests.get_history(path))
            referenced_manifests.insert(id);
    }
    for (const auto &id : manifests.list_snapshots()) {
        Snapshot snap;
        if (!manifests.read_snapshot(id, snap)) continue;
        for (const auto &[path, manifest_id] : snap.files) {
            (void)path;
            referenced_manifests.insert(manifest_id);
        }
    }

    for (const auto &id : manifests.list_manifest_ids()) {
        if (referenced_manifests.count(id)) continue;
        if (manifests.delete_manifest(id)) stats.manifests_deleted++;
    }

    // ── 4. Build the set of chunks referenced by surviving manifests ──────────
    std::set<std::string> referenced_chunks;
    for (const auto &id : manifests.list_manifest_ids()) {
        Manifest m;
        if (!manifests.read_manifest(id, m)) continue;
        for (const auto &sha : m.chunks) referenced_chunks.insert(sha);
    }

    // ── 5. Sweep unreferenced chunks ──────────────────────────────────────────
    DIR *root = opendir(objects.objects_root().c_str());
    if (!root) return stats;

    struct dirent *shard_ent;
    while ((shard_ent = readdir(root)) != nullptr) {
        std::string shard = shard_ent->d_name;
        if (shard.size() != 2) continue;
        std::string shard_path = objects.objects_root() + "/" + shard;

        struct stat shard_st{};
        if (stat(shard_path.c_str(), &shard_st) != 0 || !S_ISDIR(shard_st.st_mode))
            continue;

        DIR *sd = opendir(shard_path.c_str());
        if (!sd) continue;
        struct dirent *ent;
        while ((ent = readdir(sd)) != nullptr) {
            std::string name = ent->d_name;
            std::string full = shard_path + "/" + name;

            // A crashed chunk writer can leave a temp file behind. Remove
            // only stale ones so a currently-running writer is never touched.
            if (name.find(".tmp.") != std::string::npos) {
                struct stat st{};
                if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                    now - st.st_mtime > 3600) {
                    if (unlink(full.c_str()) == 0) {
                        stats.temp_files_deleted++;
                        stats.bytes_reclaimed += (uint64_t)st.st_size;
                    }
                }
                continue;
            }

            if (!is_sha_filename(name)) continue; // skip . and ..

            std::string sha = shard + name;
            if (referenced_chunks.count(sha)) continue;

            struct stat st{};
            if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                if (unlink(full.c_str()) == 0) {
                    stats.chunks_deleted++;
                    stats.bytes_reclaimed += (uint64_t)st.st_size;
                }
            }
        }
        closedir(sd);
    }
    closedir(root);

    LOG_INFO("GC: trimmed " + std::to_string(stats.history_entries_trimmed) +
             " history entries, deleted " +
             std::to_string(stats.manifests_deleted) + " manifests, " +
             std::to_string(stats.snapshots_deleted) + " snapshots, " +
             std::to_string(stats.chunks_deleted) + " chunks, " +
             std::to_string(stats.temp_files_deleted) + " stale temp files (" +
             std::to_string(stats.bytes_reclaimed) + " bytes)");
    return stats;
}

} // namespace cv
