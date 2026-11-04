// src/daemon/signal_handler.cc — Self-pipe signal handling implementation
#include "signal_handler.h"
#include <csignal>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <array>

namespace cv {

std::atomic<bool> g_shutdown_requested{false};
std::atomic<bool> g_reload_requested{false};

// Self-pipe: [0]=read end, [1]=write end
// WHY: We need a file descriptor that becomes readable when a signal arrives,
// so epoll can wake up the event loop without polling.
static int s_pipe_fds[2] = {-1, -1};

// The actual signal handler — must only use async-signal-safe functions.
// write(2) is async-signal-safe; we write the signal number as one byte.
static void pipe_signal_handler(int sig) {
    // Set atomic flag first (also async-signal-safe for lock-free atomics)
    if (sig == SIGTERM || sig == SIGINT) {
        g_shutdown_requested.store(true, std::memory_order_relaxed);
    } else if (sig == SIGHUP) {
        g_reload_requested.store(true, std::memory_order_relaxed);
    }

    // Write to self-pipe — wake up the event loop.
    // We ignore errors here: if the pipe is full, the flag is already set.
    uint8_t byte = static_cast<uint8_t>(sig);
    while (write(s_pipe_fds[1], &byte, 1) == -1 && errno == EINTR) {}
}

int signal_handler_init() {
    // Create the self-pipe with O_NONBLOCK on both ends.
    // O_CLOEXEC prevents leaking fds into child processes.
    if (pipe2(s_pipe_fds, O_NONBLOCK | O_CLOEXEC) != 0) {
        throw std::runtime_error(std::string("pipe2: ") + strerror(errno));
    }

    // Install signal handlers using sigaction(2) with SA_RESTART.
    // SA_RESTART means slow system calls restart after signal delivery rather
    // than returning EINTR — reduces error-handling boilerplate everywhere else.
    struct sigaction sa{};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = pipe_signal_handler;

    if (sigaction(SIGTERM, &sa, nullptr) != 0)
        throw std::runtime_error(std::string("sigaction(SIGTERM): ") + strerror(errno));
    if (sigaction(SIGINT,  &sa, nullptr) != 0)
        throw std::runtime_error(std::string("sigaction(SIGINT): ") + strerror(errno));
    if (sigaction(SIGHUP,  &sa, nullptr) != 0)
        throw std::runtime_error(std::string("sigaction(SIGHUP): ") + strerror(errno));

    return s_pipe_fds[0]; // return read end for epoll registration
}

int signal_handler_drain() {
    // Drain all pending bytes from the read end of the self-pipe.
    // Returns the last signal number read, or 0 if nothing.
    uint8_t buf[16];
    ssize_t n;
    int last_sig = 0;
    while ((n = read(s_pipe_fds[0], buf, sizeof(buf))) > 0) {
        last_sig = buf[n - 1];
    }
    return last_sig;
}

void signal_handler_cleanup() {
    if (s_pipe_fds[0] != -1) { close(s_pipe_fds[0]); s_pipe_fds[0] = -1; }
    if (s_pipe_fds[1] != -1) { close(s_pipe_fds[1]); s_pipe_fds[1] = -1; }
}

void ignore_sigpipe() {
    // WHY: Writing to a closed Unix socket returns EPIPE. Without this,
    // the kernel would also deliver SIGPIPE, killing the daemon. We suppress
    // it and check return values instead.
    signal(SIGPIPE, SIG_IGN);
}

} // namespace cv
