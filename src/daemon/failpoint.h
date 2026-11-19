// src/daemon/failpoint.h — deterministic crash injection for recovery tests
//
// WHY: Waiting for a random power loss is not a reproducible test. A failpoint
// lets tests crash the daemon at an exact point in the write protocol, then
// restart it and assert that the on-disk state is still consistent.
//
// USAGE:
//   CV_FAILPOINT=versioner_after_manifest ./build/crashvaultd --foreground ...
//
// The process calls _exit(90) immediately: no destructors run, no buffers are
// flushed, and no shutdown path executes. That is as close to a crash as a
// userspace test can get.
//
// Failpoints are compiled out in Release builds (no runtime overhead).
#pragma once
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace cv {

// Return true if the environment requests this exact failpoint.
inline bool failpoint_enabled(const char *name) {
    const char *env = std::getenv("CV_FAILPOINT");
    return env != nullptr && *env != '\0' && std::strcmp(env, name) == 0;
}

// Crash immediately if this failpoint is armed.
inline void failpoint_hit(const char *name) {
    if (failpoint_enabled(name)) _exit(90);
}

} // namespace cv

#ifdef CV_FAILPOINTS_ENABLED
#define CV_FAILPOINT(name) ::cv::failpoint_hit(name)
#else
#define CV_FAILPOINT(name) ((void)0)
#endif
