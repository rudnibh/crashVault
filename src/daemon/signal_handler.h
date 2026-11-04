// src/daemon/signal_handler.h — Safe signal handling via self-pipe trick
//
// WHY: Signal handlers run asynchronously in any thread context. Almost no
// library functions are async-signal-safe, so doing real work inside a handler
// (logging, mutexes, malloc) is undefined behaviour.
//
// Solution — the SELF-PIPE TRICK:
//   A signal handler writes one byte to a non-blocking pipe. The event loop
//   reads that byte (via poll/epoll) and performs the actual work in normal
//   thread context. This makes signal handling 100% async-signal-safe while
//   still being responsive.
//
// Alternative: signalfd(2) achieves the same goal and integrates more cleanly
// with epoll; we use self-pipe here because it is portable to any POSIX system.
#pragma once
#include <atomic>
#include <functional>
#include <unistd.h>

namespace cv {

// Global flags — written only by signal handler (async-signal-safe)
extern std::atomic<bool> g_shutdown_requested;
extern std::atomic<bool> g_reload_requested;

// Initialize: creates the self-pipe, installs SA_RESTART handlers for
// SIGTERM, SIGINT, SIGHUP. Returns the read end of the pipe (to add to epoll).
int signal_handler_init();

// Called from the event loop when the read end becomes readable.
// Returns the signal number that was received (or 0 if spurious).
int signal_handler_drain();

// Close the self-pipe fds (cleanup at shutdown)
void signal_handler_cleanup();

// Ignore SIGPIPE — broken Unix socket writes should return EPIPE, not crash
void ignore_sigpipe();

} // namespace cv
