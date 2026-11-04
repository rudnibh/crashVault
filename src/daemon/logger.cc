// src/daemon/logger.cc — Thread-safe logger via write(2) + O_APPEND
#include "logger.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <atomic>
#include <string>
#include <sstream>

namespace cv {

static int      s_log_fd    = STDERR_FILENO;
static LogLevel s_min_level = LogLevel::INFO;
static bool     s_also_stderr = true;

static const char *level_str(LogLevel l) {
    switch (l) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
    }
    return "?????";
}

void log_init(const std::string &log_path, LogLevel min_level, bool also_stderr) {
    s_min_level    = min_level;
    s_also_stderr  = also_stderr;
    if (!log_path.empty()) {
        // O_APPEND: each write atomically seeks to end before writing.
        // This makes concurrent writes from multiple threads safe without locks,
        // as long as each write is <= PIPE_BUF (4096) bytes.
        int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (fd >= 0) s_log_fd = fd;
    }
}

void log_close() {
    if (s_log_fd != STDERR_FILENO && s_log_fd >= 0) {
        close(s_log_fd);
        s_log_fd = STDERR_FILENO;
    }
}

void log_msg(LogLevel level, const char *file, int line, const std::string &msg) {
    if (level < s_min_level) return;

    // Get wall-clock time
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info{};
    gmtime_r(&ts.tv_sec, &tm_info);

    // Get kernel TID (more useful than pthread_self() for strace/perf)
    pid_t tid = (pid_t)syscall(SYS_gettid);

    // Format: 2024-01-15T12:34:56.789Z [INFO ] (tid=12345) message [file:line]
    char buf[2048];
    int n = snprintf(buf, sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ [%s] (tid=%d) %s [%s:%d]\n",
        tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
        tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
        ts.tv_nsec / 1000000,
        level_str(level), (int)tid,
        msg.c_str(),
        file, line);

    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;

    // write(2) with O_APPEND is atomic for <= PIPE_BUF bytes on Linux
    (void)write(s_log_fd, buf, n);
    if (s_also_stderr && s_log_fd != STDERR_FILENO) {
        (void)write(STDERR_FILENO, buf, n);
    }
}

} // namespace cv
