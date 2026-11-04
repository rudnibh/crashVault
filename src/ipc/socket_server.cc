// src/ipc/socket_server.cc — Unix domain socket IPC server implementation
#include "socket_server.h"
#include "../daemon/logger.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace cv {

SocketServer::SocketServer(const std::string &socket_path, IpcHandler handler)
    : socket_path_(socket_path), handler_(std::move(handler)) {}

SocketServer::~SocketServer() { stop(); }

void SocketServer::start() {
    // Create Unix domain socket (SOCK_STREAM = connection-oriented, like TCP but local)
    server_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd_ < 0) {
        throw std::runtime_error(std::string("socket: ") + strerror(errno));
    }

    // Remove stale socket file if it exists (daemon may have crashed)
    unlink(socket_path_.c_str());

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(server_fd_);
        throw std::runtime_error(std::string("bind: ") + strerror(errno));
    }

    // WHY chmod 0600: Only the owner can connect. This prevents other users
    // on the same machine from controlling or querying the daemon.
    chmod(socket_path_.c_str(), 0600);

    if (listen(server_fd_, 16) != 0) {
        close(server_fd_);
        throw std::runtime_error(std::string("listen: ") + strerror(errno));
    }

    running_.store(true);
    thread_ = std::thread(&SocketServer::serve_loop, this);
    LOG_INFO("IPC socket listening at " + socket_path_);
}

void SocketServer::stop() {
    if (!running_.exchange(false)) return;
    // Close server fd to unblock accept()
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    unlink(socket_path_.c_str());
}

void SocketServer::serve_loop() {
    while (running_.load()) {
        int client_fd = accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINVAL || errno == EBADF) break; // server_fd closed
            if (errno == EINTR) continue;
            LOG_WARN(std::string("accept: ") + strerror(errno));
            continue;
        }
        // Set timeout on client socket so a hung client doesn't block us forever
        struct timeval tv{};
        tv.tv_sec = 5;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        handle_client(client_fd);
        close(client_fd);
    }
}

void SocketServer::handle_client(int client_fd) {
    // Read until newline (max 65536 bytes)
    std::string request;
    request.reserve(256);
    char ch;
    while ((int)request.size() < 65536) {
        ssize_t n = recv(client_fd, &ch, 1, 0);
        if (n <= 0) return;
        if (ch == '\n') break;
        request += ch;
    }

    if (request.empty()) return;

    std::string response = handler_(request);
    response += "\n";
    // Write response — POSIX guarantees write() either succeeds or returns error
    size_t written = 0;
    while (written < response.size()) {
        ssize_t n = send(client_fd, response.data() + written,
                         response.size() - written, MSG_NOSIGNAL);
        if (n <= 0) return;
        written += n;
    }
}

} // namespace cv
