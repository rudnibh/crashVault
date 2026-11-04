// src/ipc/socket_server.h — Unix domain socket IPC server
//
// WHY Unix domain socket: Provides efficient local IPC without network overhead.
// Permission 0600 (owner read/write only) ensures only the owning user can
// control the daemon.  This demonstrates IPC and file permission concepts.
//
// PROTOCOL: Newline-delimited JSON.
//   Request:  {"cmd": "status"}\n
//   Response: {"ok": true, "data": {...}}\n
//
// The server uses a dedicated thread that calls accept() in a loop.
// Each client connection is handled synchronously (read request, write response,
// close). For the SSE gateway (web dashboard) a persistent connection mode
// is used.
#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <unordered_map>

namespace cv {

// Handler function: receives a JSON request string, returns a JSON response string.
using IpcHandler = std::function<std::string(const std::string &)>;

class SocketServer {
public:
    explicit SocketServer(const std::string &socket_path, IpcHandler handler);
    ~SocketServer();

    void start();
    void stop();

private:
    void serve_loop();
    void handle_client(int client_fd);

    std::string   socket_path_;
    IpcHandler    handler_;
    int           server_fd_{-1};
    std::thread   thread_;
    std::atomic<bool> running_{false};
};

} // namespace cv
