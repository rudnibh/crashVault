// src/daemon/daemonize.h — Proper POSIX daemonization
//
// WHY: A daemon is a background process that:
//  1. Detaches from the controlling terminal so it isn't killed by SIGHUP
//     when the user logs out.
//  2. Has no controlling terminal (setsid creates a new session).
//  3. Has its standard I/O redirected to /dev/null (so accidental writes
//     don't go to the terminal or block waiting).
//  4. Writes its PID to a file locked with flock(2) so only one instance
//     can run at a time.
//
// DOUBLE FORK: After the first fork, the parent exits (so the shell returns).
// The child calls setsid() to become a session leader.  It then forks again
// so the grandchild is NOT a session leader and therefore cannot accidentally
// acquire a controlling terminal via open(2) on a terminal device.
#pragma once
#include <string>

namespace cv {

struct DaemonOptions {
    std::string pid_file;        // e.g. /run/crashvaultd.pid or ~/.crashvault/crashvaultd.pid
    bool        foreground;      // skip double-fork, keep stdio, stay in foreground
    bool        chdir_root;      // chdir("/") to avoid keeping mountpoints busy
};

// Daemonize the process according to options.
// After this call returns (in the grandchild), stdio is redirected and
// a PID file has been written and locked.
// Throws std::runtime_error on failure.
// Returns the fd of the locked PID file (keep it open for the lifetime
// of the process to maintain the lock).
int daemonize(const DaemonOptions &opts);

// Remove the PID file (call at shutdown, while still holding the lock fd)
void remove_pid_file(const std::string &pid_file);

} // namespace cv
