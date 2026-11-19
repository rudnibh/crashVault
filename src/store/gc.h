// src/store/gc.h — Garbage collection and retention enforcement
//
// WHY GC is required:
// The object store is content-addressed and append-only. Superseded versions
// leave chunks behind, and deleted snapshots leave manifests behind. Without
// GC, disk usage grows forever.
//
// SAFETY RULES:
//   1. A manifest is kept if any file history or snapshot still references it.
//   2. A chunk is kept if any surviving manifest references it.
//   3. Only regular chunk/manifest/snapshot files are removed; temp files are
//      ignored (they are reclaimed by the next GC run).
#pragma once
#include <cstddef>
#include <cstdint>

namespace cv {

class ManifestStore;
class ObjectStore;

struct GcStats {
    size_t   history_entries_trimmed = 0;
    size_t   manifests_deleted       = 0;
    size_t   snapshots_deleted       = 0;
    size_t   chunks_deleted          = 0;
    uint64_t bytes_reclaimed         = 0;
};

// Run retention + garbage collection.
//   retention_versions <= 0 : keep every version per file
//   retention_days     <= 0 : keep every snapshot
GcStats run_gc(ManifestStore &manifests, ObjectStore &objects,
               int retention_versions, int retention_days);

} // namespace cv
