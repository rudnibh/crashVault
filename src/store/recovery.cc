// src/store/recovery.cc — WAL replay implementation
#include "recovery.h"
#include "wal.h"
#include "manifest.h"
#include "../daemon/logger.h"

namespace cv {

RecoveryStats recover_wal(Wal &wal, ManifestStore &manifests) {
    RecoveryStats stats;
    auto pending = wal.replay();
    stats.pending = pending.size();

    for (auto &re : pending) {
        if (re.needs_history_update) {
            // COMMIT: manifest is durable, history may be missing.
            if (!manifests.has_manifest(re.entry.manifest_id)) {
                // Can happen if the filesystem lost the manifest after commit
                // (e.g. external corruption). There is nothing safe to redo.
                LOG_ERROR("WAL replay: committed manifest missing for " +
                          re.entry.file_path + " (" + re.entry.manifest_id + ")");
                wal.complete(re.entry.seq);
                stats.failed++;
                continue;
            }

            if (manifests.append_version(re.entry.file_path,
                                         re.entry.manifest_id)) {
                LOG_INFO("WAL replay: re-appended history for " +
                         re.entry.file_path);
                stats.repaired++;
            } else {
                LOG_ERROR("WAL replay: history update failed for " +
                          re.entry.file_path + " — will retry next restart");
                stats.failed++;
                continue; // leave COMMIT entry pending
            }
        } else {
            // PREPARE-only. If the manifest is already on disk, the crash
            // happened after write_manifest() but before WAL COMMIT. The data
            // is durable, so finish the operation instead of losing a version.
            if (manifests.has_manifest(re.entry.manifest_id)) {
                if (manifests.append_version(re.entry.file_path,
                                             re.entry.manifest_id)) {
                    LOG_INFO("WAL replay: completed PREPARE with durable manifest for " +
                             re.entry.file_path);
                    stats.repaired++;
                } else {
                    LOG_ERROR("WAL replay: history update failed for " +
                              re.entry.file_path + " — will retry next restart");
                    stats.failed++;
                    continue;
                }
            } else {
                // No manifest: chunks may exist but are content-addressed and
                // harmless; GC reclaims them later.
                LOG_INFO("WAL replay: discarding incomplete PREPARE seq=" +
                         std::to_string(re.entry.seq));
                stats.discarded++;
            }
        }

        wal.complete(re.entry.seq);
    }

    return stats;
}

} // namespace cv
