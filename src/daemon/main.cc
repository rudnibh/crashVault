// src/daemon/main.cc — CrashVault daemon entry point (crashvaultd)
//
// OVERVIEW: This is the top-level glue that wires together all subsystems:
//   1. Parse command-line arguments
//   2. Load configuration
//   3. Daemonize (unless --foreground)
//   4. Initialize signal handling (self-pipe)
//   5. Start inotify watcher + debouncer
//   6. Start worker pool connected via bounded queue
//   7. Start IPC socket server
//   8. Wait for shutdown signal
//   9. Graceful drain and shutdown
//
// WHY this order: We must daemonize BEFORE creating threads. After fork(),
// only the calling thread survives — threads from the parent do NOT carry over.
// Creating the signal handler before the watcher ensures we can handle
// SIGTERM/SIGINT even if watcher initialization fails.

#include "config.h"
#include "daemonize.h"
#include "signal_handler.h"
#include "logger.h"
#include "../watcher/inotify_watcher.h"
#include "../watcher/debouncer.h"
#include "../pipeline/bounded_queue.h"
#include "../pipeline/worker_pool.h"
#include "../ipc/socket_server.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <memory>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ─── Global daemon state ────────────────────────────────────────────────────
namespace {
    std::atomic<uint64_t> g_events_received{0};
    std::atomic<uint64_t> g_versions_stored{0};
    std::atomic<uint64_t> g_queue_depth{0};
    std::string           g_config_path;
    cv::Config            g_config;
}

static void print_usage(const char *argv0) {
    std::cerr <<
        "Usage: " << argv0 << " [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --foreground, -f    Run in foreground (don't daemonize; log to stderr)\n"
        "  --config <path>     Config file path (default: ~/.crashvault/config.ini)\n"
        "  --store <path>      Store root directory (overrides config)\n"
        "  --log <path>        Log file path\n"
        "  --debug             Enable debug logging\n"
        "  --version           Print version and exit\n"
        "  --help, -h          Show this help\n"
        "\n"
        "Signals:\n"
        "  SIGTERM, SIGINT     Graceful shutdown\n"
        "  SIGHUP              Reload configuration\n";
}

// Make sure the store directory structure exists
static void ensure_store_dirs(const std::string &store_root) {
    auto mkdir_p = [](const std::string &path) {
        // Simple recursive mkdir
        for (size_t i = 1; i < path.size(); i++) {
            if (path[i] == '/') {
                std::string sub = path.substr(0, i);
                mkdir(sub.c_str(), 0755);
            }
        }
        mkdir(path.c_str(), 0755);
    };

    mkdir_p(store_root);
    mkdir_p(store_root + "/wal");
    mkdir_p(store_root + "/objects");
    mkdir_p(store_root + "/manifests");
    mkdir_p(store_root + "/snapshots");
    mkdir_p(store_root + "/meta");
    mkdir_p(store_root + "/logs");
}

// IPC command dispatcher (Phase 1: only "status" and "shutdown" implemented;
// subsequent phases add more commands)
static std::string handle_ipc(const std::string &req_str,
                               cv::BoundedQueue  &queue,
                               cv::WorkerPool    &pool,
                               cv::InotifyWatcher &watcher)
{
    json resp;
    try {
        json req = json::parse(req_str);
        std::string cmd = req.value("cmd", "");

        if (cmd == "status") {
            resp["ok"] = true;
            resp["data"] = {
                {"events_received",  g_events_received.load()},
                {"versions_stored",  g_versions_stored.load()},
                {"queue_depth",      queue.size()},
                {"queue_capacity",   queue.capacity()},
                {"active_workers",   pool.active_workers()},
                {"watch_count",      watcher.watch_count()},
                {"policy",           g_config.scheduling_policy},
                {"daemon_pid",       (int)getpid()}
            };
        } else if (cmd == "shutdown") {
            cv::g_shutdown_requested.store(true);
            resp["ok"] = true;
            resp["msg"] = "shutdown initiated";
        } else if (cmd == "ping") {
            resp["ok"] = true;
            resp["msg"] = "pong";
        } else if (cmd == "set-policy") {
            std::string policy = req.value("policy", "fcfs");
            queue.set_policy(policy);
            g_config.scheduling_policy = policy;
            resp["ok"] = true;
            resp["msg"] = "policy set to " + policy;
        } else {
            resp["ok"]    = false;
            resp["error"] = "Unknown command: " + cmd +
                            " (more commands available in Phase 5)";
        }
    } catch (const json::exception &e) {
        resp["ok"]    = false;
        resp["error"] = std::string("JSON parse error: ") + e.what();
    }
    return resp.dump();
}

int main(int argc, char *argv[]) {
    bool        foreground = false;
    bool        debug_log  = false;
    std::string config_path;
    std::string store_override;
    std::string log_path;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--foreground" || arg == "-f") {
            foreground = true;
        } else if (arg == "--debug") {
            debug_log = true;
        } else if (arg == "--config" && i+1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--store" && i+1 < argc) {
            store_override = argv[++i];
        } else if (arg == "--log" && i+1 < argc) {
            log_path = argv[++i];
        } else if (arg == "--version") {
            std::cout << "crashvaultd 0.1.0 (Phase 1)\n";
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // Default config path
    if (config_path.empty()) {
        const char *home = getenv("HOME");
        config_path = home ? std::string(home) + "/.crashvault/config.ini" : "";
    }

    // Load config
    std::string cfg_err;
    if (!g_config.load(config_path, cfg_err)) {
        // Config file not found is fine (we use defaults); parse errors are not
        if (!cfg_err.empty() && cfg_err.find("Cannot open") == std::string::npos) {
            std::cerr << "Config error: " << cfg_err << "\n";
            return 1;
        }
    }

    if (!store_override.empty()) g_config.store_root = store_override;
    g_config_path = config_path;

    // Ensure store directories exist BEFORE daemonizing
    ensure_store_dirs(g_config.store_root);

    // Daemonize
    cv::DaemonOptions daemon_opts;
    daemon_opts.pid_file   = g_config.store_root + "/crashvaultd.pid";
    daemon_opts.foreground = foreground;
    daemon_opts.chdir_root = !foreground;

    int pid_fd = -1;
    try {
        pid_fd = cv::daemonize(daemon_opts);
    } catch (const std::exception &e) {
        std::cerr << "Daemonize failed: " << e.what() << "\n";
        return 1;
    }

    // Initialize logging (after daemonize so we know stdio disposition)
    if (log_path.empty()) log_path = g_config.store_root + "/logs/daemon.log";
    cv::log_init(log_path,
                 debug_log ? cv::LogLevel::DEBUG : cv::LogLevel::INFO,
                 foreground /*also_stderr*/);

    LOG_INFO("crashvaultd starting (pid=" + std::to_string(getpid()) + ")");
    if (foreground) g_config.dump();

    // Signal handling — must happen before creating any threads
    cv::ignore_sigpipe();
    int sig_pipe_read = -1;
    try {
        sig_pipe_read = cv::signal_handler_init();
    } catch (const std::exception &e) {
        LOG_ERROR(std::string("signal_handler_init: ") + e.what());
        return 1;
    }
    (void)sig_pipe_read; // used by epoll in a future phase; signal flags checked in loop

    // Create bounded queue
    // Queue capacity: heuristically, 2× worker count per watched root, min 64
    size_t q_cap = std::max((size_t)64,
                            (size_t)g_config.worker_count * g_config.watched_roots.size() * 4);
    cv::BoundedQueue queue(q_cap, g_config.scheduling_policy);

    // Create debouncer → pushes settled events into the queue
    auto debouncer = std::make_unique<cv::Debouncer>(
        g_config.debounce_ms,
        [&](cv::SettledEvent ev) {
            g_events_received.fetch_add(1);
            int prio = 0; // Phase 5 will compute based on file size
            queue.push({ ev.path, ev.deleted, prio, 0 });
        }
    );

    // Create watcher → feeds raw events to debouncer
    auto watcher = std::make_unique<cv::InotifyWatcher>(
        g_config,
        [&](cv::RawEvent rev) {
            if (rev.kind == cv::RawEvent::Kind::OVERFLOW) {
                // On overflow: scan all watched roots for changed files
                // (Phase 1: just log; full rescan in Phase 2)
                LOG_WARN("Event queue overflow — rescan triggered");
                return;
            }
            bool deleted = (rev.kind == cv::RawEvent::Kind::DELETED);
            debouncer->push(rev.path, deleted);
        }
    );

    // Worker pool — processes work items (Phase 1: just counts them)
    cv::WorkerPool pool(
        g_config.worker_count, queue,
        [&](const cv::WorkItem &item) {
            // Phase 1: log and count; Phase 2 will do actual versioning
            LOG_INFO("Processing: " + item.path + (item.deleted ? " [deleted]" : ""));
            g_versions_stored.fetch_add(1);
        },
        10 // nice level
    );

    // IPC socket server
    std::string sock_path = g_config.store_root + "/crashvaultd.sock";
    auto ipc = std::make_unique<cv::SocketServer>(
        sock_path,
        [&](const std::string &req) {
            return handle_ipc(req, queue, pool, *watcher);
        }
    );

    // Start all subsystems
    try {
        debouncer->start();
        watcher->start();
        for (auto &root : g_config.watched_roots) {
            watcher->add_root(root);
        }
        pool.start();
        ipc->start();
    } catch (const std::exception &e) {
        LOG_ERROR(std::string("Startup error: ") + e.what());
        cv::remove_pid_file(daemon_opts.pid_file);
        close(pid_fd);
        return 1;
    }

    LOG_INFO("crashvaultd ready. Watching " +
             std::to_string(g_config.watched_roots.size()) + " root(s).");

    // ── MAIN LOOP ─────────────────────────────────────────────────────────────
    // Block here until SIGTERM/SIGINT sets g_shutdown_requested.
    // We use a simple sleep-and-check loop here; Phase 1 is about correctness,
    // not about minimal latency for the main thread.
    // The event loop for signals will be tightened in Phase 2 using epoll.
    while (!cv::g_shutdown_requested.load()) {
        // Handle SIGHUP: reload config
        if (cv::g_reload_requested.exchange(false)) {
            LOG_INFO("SIGHUP received — reloading config");
            std::string err;
            if (!g_config.reload(g_config_path, err)) {
                LOG_WARN("Config reload failed: " + err);
            } else {
                LOG_INFO("Config reloaded");
                queue.set_policy(g_config.scheduling_policy);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // ─────────────────────────────────────────────────────────────────────────

    LOG_INFO("Shutdown requested — draining...");

    // Graceful shutdown: stop accepting new events, drain the queue, then exit
    ipc->stop();
    watcher->stop();
    debouncer->stop();

    // Signal queue shutdown; workers drain remaining items then exit
    // (In Phase 1 the queue is already lightweight; this is the right pattern)
    pool.stop();

    LOG_INFO("crashvaultd stopped cleanly");
    cv::log_close();

    // Release PID file (remove then close — holds lock until close)
    cv::remove_pid_file(daemon_opts.pid_file);
    close(pid_fd);

    cv::signal_handler_cleanup();
    return 0;
}
