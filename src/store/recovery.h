// src/store/recovery.h — WAL replay / crash recovery
//
// WHY a separate helper: The daemon's startup path and the crash-recovery
// tests must execute exactly the same recovery algorithm. Keeping it here
// avoids the previous situation where recovery logic lived inline in main()
// and could not be unit-tested.
#pragma once
#include <cstddef>

namespace cv {

class Wal;
class ManifestStore;

struct RecoveryStats {
    size_t pending   = 0; // WAL entries that needed attention
    size_t repaired  = 0; // COMMIT entries whose history was re-appended
    size_t discarded = 0; // PREPARE-only entries safely discarded
    size_t failed    = 0; // entries that could not be repaired
};

// Replay every incomplete WAL entry:
//   - PREPARE-only: manifest was never committed → discard
//   - COMMIT without COMPLETE: re-append history (idempotent) → complete
//   - COMPLETE: should not be returned by Wal::replay()
RecoveryStats recover_wal(Wal &wal, ManifestStore &manifests);

} // namespace cv
