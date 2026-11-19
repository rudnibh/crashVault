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
// Version store + crash consistency
#include "../store/object_store.h"
#include "../store/manifest.h"
#include "../store/versioner.h"
#include "../store/wal.h"
#include "../store/recovery.h"
#include "../store/gc.h"
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
#include <ctime>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ─── Global daemon state ────────────────────────────────────────────────────
namespace {
    std::atomic<uint64_t> g_events_received{0};
    std::atomic<uint64_t> g_versions_stored{0};
    std::atomic<uint64_t> g_queue_depth{0};
    std::atomic<bool>     g_snapshot_dirty{false};
    std::atomic<bool>     g_crash_after_next_version{false};
    std::atomic<int64_t>  g_last_event_epoch{0};
    std::string           g_config_path;
    cv::Config            g_config;
}

// Everything the IPC command handler needs. Keeping this explicit avoids
// scattering globals and makes each command's dependencies obvious.
struct IpcContext {
    cv::BoundedQueue   *queue      = nullptr;
    cv::WorkerPool     *pool       = nullptr;
    cv::InotifyWatcher *watcher    = nullptr;
    cv::ManifestStore  *manifests  = nullptr;
    cv::ObjectStore    *objects    = nullptr;
    cv::Versioner      *versioner  = nullptr;
    cv::Wal            *wal        = nullptr;
    cv::Config         *config     = nullptr;
};

static json manifest_summary(const cv::Manifest &m) {
    return json{
        {"id",        m.id},
        {"version",   m.version},
        {"file_size", m.file_size},
        {"timestamp", (int64_t)m.timestamp},
        {"sha256",    m.sha256},
        {"chunks",    m.chunks.size()}
    };
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

// IPC command dispatcher
static std::string handle_ipc(const std::string &req_str, IpcContext &ctx) {
    json resp;
    try {
        json req = json::parse(req_str);
        std::string cmd = req.value("cmd", "");

        if (cmd == "status") {
            resp["ok"] = true;
            resp["data"] = {
                {"events_received",  g_events_received.load()},
                {"versions_stored",  g_versions_stored.load()},
                {"queue_depth",      ctx.queue->size()},
                {"queue_capacity",   ctx.queue->capacity()},
                {"active_workers",   ctx.pool->active_workers()},
                {"watch_count",      ctx.watcher->watch_count()},
                {"policy",           ctx.config->scheduling_policy},
                {"daemon_pid",       (int)getpid()},
                {"tracked_files",    ctx.manifests->list_history_paths().size()},
                {"manifests",        ctx.manifests->list_manifest_ids().size()},
                {"snapshots",        ctx.manifests->list_snapshots().size()},
                {"pending_wal",      ctx.wal->replay().size()},
                {"read_budget_in_use",    ctx.versioner->read_budget_in_use()},
                {"read_budget_capacity",  ctx.versioner->read_budget_capacity()},
                {"store_root",       ctx.config->store_root}
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
            if (policy != "fcfs" && policy != "priority") {
                resp["ok"] = false;
                resp["error"] = "policy must be 'fcfs' or 'priority'";
            } else {
                ctx.queue->set_policy(policy);
                ctx.config->scheduling_policy = policy;
                resp["ok"] = true;
                resp["msg"] = "policy set to " + policy;
            }
        } else if (cmd == "reload") {
            cv::g_reload_requested.store(true);
            resp["ok"] = true;
            resp["msg"] = "config reload requested";
        } else if (cmd == "list") {
            std::string root = req.value("root", "");
            json files = json::array();
            for (const auto &path : ctx.manifests->list_history_paths()) {
                if (!root.empty() && path.rfind(root, 0) != 0) continue;
                files.push_back({
                    {"path",     path},
                    {"versions", ctx.manifests->get_history(path).size()},
                    {"exists",   access(path.c_str(), F_OK) == 0}
                });
            }
            resp["ok"] = true;
            resp["data"] = {{"files", files}, {"count", files.size()}};
        } else if (cmd == "history") {
            std::string path = req.value("path", "");
            if (path.empty()) {
                resp["ok"] = false;
                resp["error"] = "history requires a path";
            } else {
                auto history = ctx.manifests->get_history(path);
                json versions = json::array();
                for (auto it = history.rbegin(); it != history.rend(); ++it) {
                    cv::Manifest m;
                    if (!ctx.manifests->read_manifest(*it, m)) continue;
                    versions.push_back(manifest_summary(m));
                }
                resp["ok"] = true;
                resp["data"] = {
                    {"path",     path},
                    {"exists",   access(path.c_str(), F_OK) == 0},
                    {"versions", versions}
                };
            }
        } else if (cmd == "snapshots") {
            json snaps = json::array();
            for (const auto &id : ctx.manifests->list_snapshots()) {
                cv::Snapshot s;
                if (!ctx.manifests->read_snapshot(id, s)) continue;
                snaps.push_back({
                    {"id",        s.id},
                    {"root",      s.root},
                    {"timestamp", (int64_t)s.timestamp},
                    {"files",     s.files.size()}
                });
            }
            resp["ok"] = true;
            resp["data"] = {{"snapshots", snaps}, {"count", snaps.size()}};
        } else if (cmd == "snapshot-now") {
            std::string root = req.value("root", "");
            if (root.empty() && !ctx.config->watched_roots.empty())
                root = ctx.config->watched_roots.front();
            cv::Snapshot snap;
            if (!ctx.manifests->create_snapshot(root, snap)) {
                resp["ok"] = false;
                resp["error"] = "failed to write snapshot";
            } else {
                g_snapshot_dirty.store(false);
                resp["ok"] = true;
                resp["data"] = {
                    {"id",        snap.id},
                    {"root",      snap.root},
                    {"timestamp", (int64_t)snap.timestamp},
                    {"files",     snap.files.size()}
                };
            }
        } else if (cmd == "restore") {
            std::string path = req.value("path", "");
            std::string dest = req.value("dest", "");
            std::string manifest_id = req.value("manifest_id", "");
            int version = req.value("version", 0);

            bool ok = false;
            std::string used_id;
            if (!manifest_id.empty()) {
                ok = ctx.versioner->restore(manifest_id, dest);
                used_id = manifest_id;
            } else if (!path.empty() && version > 0) {
                auto history = ctx.manifests->get_history(path);
                if (version > (int)history.size()) {
                    resp["ok"] = false;
                    resp["error"] = "version out of range (1.." +
                                    std::to_string(history.size()) + ")";
                    return resp.dump();
                }
                used_id = history[(size_t)version - 1];
                ok = ctx.versioner->restore(used_id, dest);
            } else if (!path.empty()) {
                ok = ctx.versioner->restore_latest(path, dest);
            }

            if (ok) {
                g_snapshot_dirty.store(true);
                resp["ok"] = true;
                resp["data"] = {
                    {"path",        path},
                    {"dest",        dest.empty() ? path : dest},
                    {"manifest_id", used_id}
                };
            } else {
                resp["ok"] = false;
                resp["error"] = "restore failed (file/version not found or chunks missing)";
            }
        } else if (cmd == "restore-snapshot") {
            std::string id = req.value("id", "");
            std::string dest_root = req.value("dest_root", "");
            if (id == "latest") {
                auto ids = ctx.manifests->list_snapshots();
                if (!ids.empty()) id = ids.back();
            }
            cv::Snapshot snap;
            if (id.empty() || !ctx.manifests->read_snapshot(id, snap)) {
                resp["ok"] = false;
                resp["error"] = "snapshot not found";
            } else if (!ctx.versioner->restore_snapshot(snap, dest_root)) {
                resp["ok"] = false;
                resp["error"] = "some files could not be restored (see daemon log)";
            } else {
                resp["ok"] = true;
                resp["data"] = {
                    {"id",    snap.id},
                    {"root",  snap.root},
                    {"files", snap.files.size()},
                    {"dest_root", dest_root}
                };
            }
        } else if (cmd == "verify") {
            auto problems = ctx.manifests->verify_all(ctx.objects->objects_root());
            resp["ok"] = true;
            resp["data"] = {
                {"healthy",  problems.empty()},
                {"problems", problems}
            };
        } else if (cmd == "gc") {
            auto stats = cv::run_gc(*ctx.manifests, *ctx.objects,
                                    ctx.config->retention_versions,
                                    ctx.config->retention_days);
            resp["ok"] = true;
            resp["data"] = {
                {"history_entries_trimmed", stats.history_entries_trimmed},
                {"manifests_deleted",       stats.manifests_deleted},
                {"snapshots_deleted",       stats.snapshots_deleted},
                {"chunks_deleted",          stats.chunks_deleted},
                {"bytes_reclaimed",         stats.bytes_reclaimed}
            };
        } else if (cmd == "simulate-crash") {
            g_crash_after_next_version.store(true);
            resp["ok"] = true;
            resp["msg"] = "crash armed: daemon will exit abruptly after the next version";
        } else {
            resp["ok"]    = false;
            resp["error"] = "Unknown command: " + cmd;
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
            std::cout << "crashvaultd 1.0.0\n";
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
            g_last_event_epoch.store((int64_t)time(nullptr));

            // Priority policy: small files first, large files last. This keeps
            // interactive edits (usually small source files) responsive while
            // a multi-gigabyte build artefact is versioned in the background.
            int prio = 3;
            struct stat st{};
            if (!ev.deleted && stat(ev.path.c_str(), &st) == 0) {
                if      (st.st_size <=   64 * 1024) prio = 0;
                else if (st.st_size <= 1024 * 1024) prio = 1;
                else if (st.st_size <= 16 * 1024 * 1024) prio = 2;
            }
            queue.push({ ev.path, ev.deleted, prio, 0 });
        }
    );

    // Create watcher → feeds raw events to debouncer
    auto watcher = std::make_unique<cv::InotifyWatcher>(
        g_config,
        [&](cv::RawEvent rev) {
            if (rev.kind == cv::RawEvent::Kind::OVERFLOW) {
                // The watcher itself performs the full rescan and emits a
                // MODIFIED event per regular file; this is just an observer.
                LOG_WARN("Event queue overflow — watcher is rescanning roots");
                return;
            }
            bool deleted = (rev.kind == cv::RawEvent::Kind::DELETED);
            debouncer->push(rev.path, deleted);
        }
    );

    // Version store with WAL for crash consistency
    auto obj_store      = std::make_shared<cv::ObjectStore>(g_config.store_root);
    auto manifest_store = std::make_shared<cv::ManifestStore>(g_config.store_root);
    auto wal            = std::make_shared<cv::Wal>(g_config.store_root);

    // WAL replay on startup — recover any incomplete operations
    {
        auto stats = cv::recover_wal(*wal, *manifest_store);
        if (stats.pending > 0) {
            LOG_WARN("WAL replay: " + std::to_string(stats.pending) +
                     " incomplete operation(s): " +
                     std::to_string(stats.repaired) + " repaired, " +
                     std::to_string(stats.discarded) + " discarded, " +
                     std::to_string(stats.failed) + " failed");
        }
        wal->gc(200); // keep last 200 completed entries
    }

    auto versioner = std::make_shared<cv::Versioner>(
                        g_config, *obj_store, *manifest_store, *wal);

    // Worker pool — now calls the real versioner
    cv::WorkerPool pool(
        g_config.worker_count, queue,
        [&, versioner](const cv::WorkItem &item) {
            auto res = versioner->process(item.path, item.deleted);
            using S  = cv::VersionResult::Status;
            if (res.status == S::OK) {
                g_versions_stored.fetch_add(1);
                g_snapshot_dirty.store(true);
            } else if (res.status == S::SKIPPED_UNSTABLE) {
                LOG_WARN("Skipped (unstable): " + item.path);
            } else if (res.status == S::ERROR) {
                LOG_ERROR("Versioning error: " + item.path);
            }

            // simulate-crash: deliberately die without any cleanup so the next
            // startup exercises WAL replay.
            if (g_crash_after_next_version.exchange(false)) {
                LOG_WARN("simulate-crash: aborting process after versioning " +
                         item.path);
                _exit(90);
            }
        },
        10 // nice level
    );

    // IPC socket server
    IpcContext ipc_ctx;
    ipc_ctx.queue     = &queue;
    ipc_ctx.pool      = &pool;
    ipc_ctx.watcher   = watcher.get();
    ipc_ctx.manifests = manifest_store.get();
    ipc_ctx.objects   = obj_store.get();
    ipc_ctx.versioner = versioner.get();
    ipc_ctx.wal       = wal.get();
    ipc_ctx.config    = &g_config;

    std::string sock_path = g_config.store_root + "/crashvaultd.sock";
    auto ipc = std::make_unique<cv::SocketServer>(
        sock_path,
        [&](const std::string &req) {
            return handle_ipc(req, ipc_ctx);
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
    // The main thread only handles signals and quiescence snapshots; keeping
    // it on a 100 ms tick avoids adding another epoll thread for two flags.
    int64_t last_snapshot_epoch = 0;
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

        // Automatic quiescence snapshots: once writes have stopped for
        // snapshot_quiescence_s, capture a consistent project-level snapshot.
        if (g_config.snapshot_quiescence_s > 0 && g_snapshot_dirty.load()) {
            int64_t now      = (int64_t)time(nullptr);
            int64_t last_ev  = g_last_event_epoch.load();
            int64_t quiet    = now - last_ev;
            if (quiet >= g_config.snapshot_quiescence_s &&
                now - last_snapshot_epoch >= g_config.snapshot_quiescence_s) {
                if (g_config.watched_roots.empty()) {
                    cv::Snapshot snap;
                    if (manifest_store->create_snapshot("", snap)) {
                        LOG_INFO("Auto-snapshot " + snap.id + " (" +
                                 std::to_string(snap.files.size()) + " files)");
                    }
                } else {
                    for (const auto &root : g_config.watched_roots) {
                        cv::Snapshot snap;
                        if (manifest_store->create_snapshot(root, snap)) {
                            LOG_INFO("Auto-snapshot " + snap.id + " for " + root +
                                     " (" + std::to_string(snap.files.size()) +
                                     " files)");
                        }
                    }
                }
                g_snapshot_dirty.store(false);
                last_snapshot_epoch = now;
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

    // Signal queue shutdown; workers drain remaining items then exit.
    pool.stop();

    LOG_INFO("crashvaultd stopped cleanly");
    cv::log_close();

    // Release PID file (remove then close — holds lock until close)
    cv::remove_pid_file(daemon_opts.pid_file);
    close(pid_fd);

    cv::signal_handler_cleanup();
    return 0;
}
