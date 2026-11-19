// src/cli/main.cc — cvctl command-line client
//
// WHY a separate binary: The daemon runs as a background process. cvctl
// connects to it via the Unix socket and translates human commands to JSON.
// This demonstrates IPC (the daemon is the server, cvctl is the client).

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static std::string default_socket_path() {
    const char *home = getenv("HOME");
    return home ? std::string(home) + "/.crashvault/crashvaultd.sock" : "/tmp/crashvaultd.sock";
}

static int connect_to_daemon(const std::string &sock_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path)-1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        std::cerr << "Cannot connect to daemon at " << sock_path
                  << "\nIs crashvaultd running?\n";
        return -1;
    }
    return fd;
}

static std::string send_recv(int fd, const json &req) {
    std::string msg = req.dump() + "\n";
    // Send
    size_t sent = 0;
    while (sent < msg.size()) {
        ssize_t n = send(fd, msg.data() + sent, msg.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return "";
        sent += n;
    }
    // Recv until newline
    std::string resp;
    char ch;
    while (recv(fd, &ch, 1, 0) == 1) {
        if (ch == '\n') break;
        resp += ch;
    }
    return resp;
}

static void print_json_pretty(const std::string &s) {
    try {
        json j = json::parse(s);
        std::cout << j.dump(2) << "\n";
    } catch (...) {
        std::cout << s << "\n";
    }
}

static void usage(const char *argv0) {
    std::cerr <<
        "Usage: " << argv0 << " [--socket PATH] [--json] COMMAND [ARGS...]\n"
        "\n"
        "Commands:\n"
        "  status                       Show daemon status\n"
        "  ping                         Check daemon is alive\n"
        "  set-policy POLICY            Set scheduling policy (fcfs|priority)\n"
        "  reload                       Reload the configuration file\n"
        "  shutdown                     Gracefully stop the daemon\n"
        "  list [ROOT]                  List tracked files\n"
        "  history PATH                 Show version history for a file\n"
        "  snapshots                    List project snapshots\n"
        "  snapshot-now [ROOT]          Create a snapshot immediately\n"
        "  restore PATH [VERSION]       Restore latest or a 1-based version\n"
        "      --dest PATH              Write somewhere other than the original\n"
        "      --manifest ID            Restore an exact manifest id\n"
        "  restore-snapshot ID [DEST]   Restore a snapshot (ID may be 'latest')\n"
        "  verify                       Verify manifests and chunk hashes\n"
        "  gc                           Apply retention and reclaim disk space\n"
        "  simulate-crash               Arm a crash after the next version\n";
}

int main(int argc, char *argv[]) {
    std::string sock_path = default_socket_path();
    bool        as_json   = false;
    std::vector<std::string> args;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--socket" && i+1 < argc) { sock_path = argv[++i]; }
        else if (a == "--json") { as_json = true; }
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else args.push_back(a);
    }

    if (args.empty()) { usage(argv[0]); return 1; }

    std::string cmd = args[0];
    json req;

    if (cmd == "status" || cmd == "ping" || cmd == "shutdown" ||
        cmd == "reload" || cmd == "snapshots" || cmd == "verify" ||
        cmd == "gc" || cmd == "simulate-crash") {
        req["cmd"] = cmd;
    } else if (cmd == "set-policy" && args.size() >= 2) {
        req["cmd"]    = "set-policy";
        req["policy"] = args[1];
    } else if (cmd == "list") {
        req["cmd"] = "list";
        if (args.size() >= 2) req["root"] = args[1];
    } else if (cmd == "history" && args.size() >= 2) {
        req["cmd"]  = "history";
        req["path"] = args[1];
    } else if (cmd == "snapshot-now") {
        req["cmd"] = "snapshot-now";
        if (args.size() >= 2) req["root"] = args[1];
    } else if (cmd == "restore" && args.size() >= 2) {
        req["cmd"]  = "restore";
        req["path"] = args[1];
        for (size_t i = 2; i < args.size(); i++) {
            if (args[i] == "--dest" && i + 1 < args.size()) {
                req["dest"] = args[++i];
            } else if (args[i] == "--manifest" && i + 1 < args.size()) {
                req["manifest_id"] = args[++i];
            } else {
                try {
                    req["version"] = std::stoi(args[i]);
                } catch (...) {
                    std::cerr << "Invalid version: " << args[i] << "\n";
                    return 1;
                }
            }
        }
    } else if (cmd == "restore-snapshot" && args.size() >= 2) {
        req["cmd"] = "restore-snapshot";
        req["id"]  = args[1];
        if (args.size() >= 3) req["dest_root"] = args[2];
    } else {
        std::cerr << "Unknown command or missing arguments: " << cmd << "\n";
        usage(argv[0]);
        return 1;
    }

    int fd = connect_to_daemon(sock_path);
    if (fd < 0) return 1;

    std::string resp_str = send_recv(fd, req);
    close(fd);

    if (resp_str.empty()) {
        std::cerr << "No response from daemon\n";
        return 1;
    }

    if (as_json) {
        std::cout << resp_str << "\n";
        return 0;
    }

    // Pretty-print for humans
    int rc = 0;
    try {
        json resp = json::parse(resp_str);
        bool ok = resp.value("ok", false);
        if (!ok) {
            std::cerr << "Error: " << resp.value("error", "unknown") << "\n";
            return 1;
        }
        if (cmd == "status") {
            auto &d = resp["data"];
            std::cout << "CrashVault Daemon Status\n";
            std::cout << "========================\n";
            std::cout << "PID:             " << d.value("daemon_pid", 0) << "\n";
            std::cout << "Events received: " << d.value("events_received", 0) << "\n";
            std::cout << "Versions stored: " << d.value("versions_stored", 0) << "\n";
            std::cout << "Tracked files:   " << d.value("tracked_files", 0) << "\n";
            std::cout << "Manifests:       " << d.value("manifests", 0) << "\n";
            std::cout << "Snapshots:       " << d.value("snapshots", 0) << "\n";
            std::cout << "Pending WAL:     " << d.value("pending_wal", 0) << "\n";
            std::cout << "Read budget:     " << d.value("read_budget_in_use", (size_t)0)
                      << " / " << d.value("read_budget_capacity", (size_t)0) << " bytes\n";
            std::cout << "Queue depth:     " << d.value("queue_depth", 0)
                      << " / " << d.value("queue_capacity", 0) << "\n";
            std::cout << "Active workers:  " << d.value("active_workers", 0) << "\n";
            std::cout << "Watch count:     " << d.value("watch_count", 0) << "\n";
            std::cout << "Policy:          " << d.value("policy", "fcfs") << "\n";
        } else if (cmd == "list") {
            auto &d = resp["data"];
            auto files = d["files"];
            std::cout << files.size() << " tracked file(s)\n";
            for (auto &f : files) {
                std::cout << "  " << f.value("versions", 0) << " version(s)  "
                          << f.value("path", "") << "\n";
            }
        } else if (cmd == "history") {
            auto &d = resp["data"];
            std::cout << "History for " << d.value("path", "") << "\n";
            auto versions = d["versions"];
            if (versions.empty()) std::cout << "  (no versions)\n";
            for (auto &v : versions) {
                std::cout << "  v" << v.value("version", 0)
                          << "  " << v.value("id", "")
                          << "  " << v.value("file_size", 0) << " bytes"
                          << "  chunks=" << v.value("chunks", 0)
                          << "  ts=" << v.value("timestamp", (int64_t)0) << "\n";
            }
        } else if (cmd == "snapshots") {
            auto &d = resp["data"];
            auto snaps = d["snapshots"];
            std::cout << snaps.size() << " snapshot(s)\n";
            for (auto &s : snaps) {
                std::cout << "  " << s.value("id", "")
                          << "  files=" << s.value("files", 0)
                          << "  root=" << s.value("root", "")
                          << "  ts=" << s.value("timestamp", (int64_t)0) << "\n";
            }
        } else if (cmd == "verify") {
            auto &d = resp["data"];
            if (d.value("healthy", false)) {
                std::cout << "OK: all manifests and chunks are valid\n";
            } else {
                std::cout << "CORRUPTION DETECTED:\n";
                for (auto &p : d["problems"])
                    std::cout << "  - " << p.get<std::string>() << "\n";
                rc = 1;
            }
        } else if (cmd == "gc") {
            auto &d = resp["data"];
            std::cout << "Garbage collection complete\n";
            std::cout << "  history entries trimmed: "
                      << d.value("history_entries_trimmed", 0) << "\n";
            std::cout << "  manifests deleted:       "
                      << d.value("manifests_deleted", 0) << "\n";
            std::cout << "  snapshots deleted:       "
                      << d.value("snapshots_deleted", 0) << "\n";
            std::cout << "  chunks deleted:          "
                      << d.value("chunks_deleted", 0) << "\n";
            std::cout << "  bytes reclaimed:         "
                      << d.value("bytes_reclaimed", (uint64_t)0) << "\n";
        } else if (cmd == "snapshot-now" || cmd == "restore" ||
                   cmd == "restore-snapshot") {
            auto &d = resp["data"];
            if (d.contains("id")) std::cout << "Snapshot: " << d["id"] << "\n";
            if (d.contains("manifest_id") && !d["manifest_id"].get<std::string>().empty())
                std::cout << "Manifest: " << d["manifest_id"] << "\n";
            if (d.contains("dest") && !d["dest"].get<std::string>().empty())
                std::cout << "Wrote: " << d["dest"] << "\n";
            if (d.contains("files")) std::cout << "Files: " << d["files"] << "\n";
        } else {
            if (resp.contains("msg")) std::cout << resp["msg"] << "\n";
            else print_json_pretty(resp_str);
        }
    } catch (...) {
        std::cout << resp_str << "\n";
    }
    return rc;
}
