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
        "Commands (Phase 1):\n"
        "  status               Show daemon status\n"
        "  ping                 Check daemon is alive\n"
        "  set-policy POLICY    Set scheduling policy (fcfs|priority)\n"
        "  shutdown             Gracefully stop the daemon\n"
        "\n"
        "More commands available in later phases.\n";
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

    if (cmd == "status") {
        req["cmd"] = "status";
    } else if (cmd == "ping") {
        req["cmd"] = "ping";
    } else if (cmd == "shutdown") {
        req["cmd"] = "shutdown";
    } else if (cmd == "set-policy" && args.size() >= 2) {
        req["cmd"]    = "set-policy";
        req["policy"] = args[1];
    } else {
        std::cerr << "Unknown command: " << cmd << "\n";
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
            std::cout << "PID:            " << d.value("daemon_pid", 0) << "\n";
            std::cout << "Events received:" << d.value("events_received", 0) << "\n";
            std::cout << "Versions stored:" << d.value("versions_stored", 0) << "\n";
            std::cout << "Queue depth:    " << d.value("queue_depth", 0)
                      << " / " << d.value("queue_capacity", 0) << "\n";
            std::cout << "Active workers: " << d.value("active_workers", 0) << "\n";
            std::cout << "Watch count:    " << d.value("watch_count", 0) << "\n";
            std::cout << "Policy:         " << d.value("policy", "fcfs") << "\n";
        } else {
            if (resp.contains("msg")) std::cout << resp["msg"] << "\n";
            else print_json_pretty(resp_str);
        }
    } catch (...) {
        std::cout << resp_str << "\n";
    }
    return 0;
}
