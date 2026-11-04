// src/daemon/daemonize.cc — POSIX daemonization with PID file + flock
#include "daemonize.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <sstream>

namespace cv {

// Helper: write the current PID into fd, then fsync it.
// WHY: We fsync the PID file so its content is durable before we publish it
// to external tools that might read it.
static void write_pid(int fd) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    if (write(fd, buf, n) != n) {
        throw std::runtime_error(std::string("write pid: ") + strerror(errno));
    }
    if (fsync(fd) != 0) {
        throw std::runtime_error(std::string("fsync pid: ") + strerror(errno));
    }
}

// Helper: open and flock the PID file.
// WHY: flock(LOCK_EX|LOCK_NB) is an advisory exclusive lock.  If another
// instance already holds the lock, flock returns EWOULDBLOCK and we bail out.
// The lock is automatically released when the fd is closed (process exits),
// so we keep the fd open for the lifetime of the daemon.
static int open_and_lock_pidfile(const std::string &path) {
    // O_CLOEXEC: don't leak this fd into child processes
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        throw std::runtime_error("Cannot open PID file " + path + ": " + strerror(errno));
    }

    // Try non-blocking exclusive lock
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            // Another instance is running — read its PID for the error message
            char buf[32] = {};
            lseek(fd = open(path.c_str(), O_RDONLY), 0, SEEK_SET);
            (void)read(fd, buf, sizeof(buf)-1);
            close(fd);
            throw std::runtime_error(
                "Another crashvaultd is already running (PID file: " + path +
                ", pid=" + std::string(buf) + ")");
        }
        throw std::runtime_error(std::string("flock: ") + strerror(errno));
    }

    // Truncate before writing (old PID might be longer)
    if (ftruncate(fd, 0) != 0) {
        throw std::runtime_error(std::string("ftruncate pid: ") + strerror(errno));
    }

    return fd;
}

int daemonize(const DaemonOptions &opts) {
    if (opts.foreground) {
        // Skip double-fork — stay in the foreground for debugging.
        // Still create/lock the PID file so only one instance runs.
        int pid_fd = open_and_lock_pidfile(opts.pid_file);
        write_pid(pid_fd);
        // Keep stdio as-is
        return pid_fd;
    }

    // ── DOUBLE FORK ──────────────────────────────────────────────────────────
    // Fork #1: let the shell's wait() complete
    pid_t pid1 = fork();
    if (pid1 < 0) throw std::runtime_error(std::string("fork #1: ") + strerror(errno));
    if (pid1 > 0) _exit(0); // Parent exits immediately

    // Child: become a session leader (detaches from controlling terminal)
    if (setsid() < 0) throw std::runtime_error(std::string("setsid: ") + strerror(errno));

    // Fork #2: the grandchild is NOT a session leader, so it cannot
    // accidentally re-acquire a controlling terminal.
    pid_t pid2 = fork();
    if (pid2 < 0) throw std::runtime_error(std::string("fork #2: ") + strerror(errno));
    if (pid2 > 0) _exit(0); // Session leader exits

    // Grandchild continues as the daemon
    // ─────────────────────────────────────────────────────────────────────────

    // Change to root directory so we don't block unmounting any filesystem
    if (opts.chdir_root) {
        if (chdir("/") != 0) {
            throw std::runtime_error(std::string("chdir /: ") + strerror(errno));
        }
    }

    // Set a permissive umask so the daemon can create files with intended perms
    umask(0022);

    // Redirect stdio to /dev/null — accidental writes/reads won't block or
    // corrupt the terminal. The daemon uses structured logging instead.
    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0) throw std::runtime_error(std::string("open /dev/null: ") + strerror(errno));
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > 2) close(devnull);

    // Write and lock PID file AFTER the final fork so we write the right PID
    int pid_fd = open_and_lock_pidfile(opts.pid_file);
    write_pid(pid_fd);

    return pid_fd;
}

void remove_pid_file(const std::string &pid_file) {
    // WHY: We call unlink BEFORE releasing the flock so there is no window
    // where another process could open the (now-unlocked) file and see a stale
    // PID. The fd is closed by the caller immediately after this returns.
    unlink(pid_file.c_str());
}

} // namespace cv
