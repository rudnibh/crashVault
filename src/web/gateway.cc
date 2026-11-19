// src/web/gateway.cc — minimal HTTP gateway + web dashboard
//
// The daemon deliberately speaks only a private Unix-socket JSON protocol.
// This small gateway exposes a read-mostly HTTP API on loopback and serves a
// self-contained dashboard. It forwards requests to the daemon over the same
// JSON protocol, so it cannot bypass permissions or invent state.
//
// Endpoints:
//   GET  /                     dashboard HTML
//   GET  /health               liveness probe
//   GET  /api/status           daemon status
//   GET  /api/list[?root=...]  tracked files
//   GET  /api/history?path=... version history
//   GET  /api/snapshots        snapshot list
//   GET  /api/verify           integrity check
//   POST /api/snapshot-now     create snapshot
//   POST /api/gc               run retention + GC
//
// SECURITY: binds to 127.0.0.1 by default. There is no authentication because
// the gateway is intended for the same user on the same machine; do not expose
// it on a public interface.

#include <nlohmann/json.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace {

std::atomic<bool> g_stop{false};
int g_listen_fd = -1;

void on_signal(int) {
    g_stop.store(true);
    if (g_listen_fd >= 0) shutdown(g_listen_fd, SHUT_RDWR);
}

std::string default_socket_path() {
    const char *home = getenv("HOME");
    return home ? std::string(home) + "/.crashvault/crashvaultd.sock"
                : "/tmp/crashvaultd.sock";
}

bool send_all(int fd, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

std::string http_response(int code, const std::string &reason,
                          const std::string &content_type,
                          const std::string &body) {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << code << " " << reason << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Cache-Control: no-store\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
       << "Access-Control-Allow-Headers: Content-Type\r\n"
       << "Connection: close\r\n\r\n"
       << body;
    return ss.str();
}

// Forward one newline-delimited JSON request to the daemon.
std::string ipc_call(const std::string &socket_path, const json &request) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return "";

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return "";
    }

    // Timeout so a wedged daemon cannot pin gateway threads forever.
    struct timeval tv{};
    tv.tv_sec = 5;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string msg = request.dump() + "\n";
    if (!send_all(fd, msg)) { close(fd); return ""; }

    std::string response;
    char ch;
    while (recv(fd, &ch, 1, 0) == 1) {
        if (ch == '\n') break;
        response += ch;
        if (response.size() > 64 * 1024) break;
    }
    close(fd);
    return response;
}

std::string url_decode(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
            } else {
                out += s[i];
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string query_param(const std::string &query, const std::string &key) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        std::string pair = query.substr(pos, amp == std::string::npos
                                              ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key)
            return url_decode(pair.substr(eq + 1));
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

const char *DASHBOARD_HTML = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CrashVault Dashboard</title>
<style>
  :root { color-scheme: dark; --bg:#0f141b; --panel:#171f2b; --line:#263243;
          --text:#dce6f2; --muted:#8595a8; --accent:#5ee0a0; --warn:#ffb454; }
  * { box-sizing:border-box; }
  body { margin:0; font-family:ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif;
         background:var(--bg); color:var(--text); }
  header { padding:22px 28px; border-bottom:1px solid var(--line);
           display:flex; align-items:center; gap:14px; flex-wrap:wrap; }
  h1 { font-size:19px; margin:0; letter-spacing:.3px; }
  .dot { width:9px; height:9px; border-radius:50%; background:#666; }
  .dot.ok { background:var(--accent); box-shadow:0 0 10px var(--accent); }
  .dot.bad { background:#ff5f6d; box-shadow:0 0 10px #ff5f6d; }
  main { padding:24px 28px; max-width:1180px; margin:0 auto; }
  .cards { display:grid; grid-template-columns:repeat(auto-fit,minmax(170px,1fr));
           gap:14px; margin-bottom:22px; }
  .card { background:var(--panel); border:1px solid var(--line); border-radius:12px;
          padding:16px 18px; }
  .card .label { color:var(--muted); font-size:12px; text-transform:uppercase;
                 letter-spacing:.8px; }
  .card .value { font-size:26px; font-weight:650; margin-top:6px; }
  section { background:var(--panel); border:1px solid var(--line); border-radius:12px;
            margin-bottom:20px; overflow:hidden; }
  section h2 { font-size:14px; margin:0; padding:14px 18px; border-bottom:1px solid var(--line);
               color:var(--muted); text-transform:uppercase; letter-spacing:.8px; }
  table { width:100%; border-collapse:collapse; font-size:13px; }
  th,td { text-align:left; padding:10px 18px; border-bottom:1px solid var(--line);
          vertical-align:top; }
  th { color:var(--muted); font-weight:500; }
  tr:last-child td { border-bottom:none; }
  td.mono { font-family:ui-monospace,SFMono-Regular,monospace; font-size:12px; }
  .toolbar { display:flex; gap:10px; flex-wrap:wrap; margin-bottom:20px; }
  button { background:#223046; color:var(--text); border:1px solid var(--line);
           border-radius:9px; padding:9px 14px; cursor:pointer; font-size:13px; }
  button:hover { border-color:var(--accent); }
  .empty { color:var(--muted); padding:16px 18px; font-size:13px; }
  code { color:var(--accent); }
</style>
</head>
<body>
<header>
  <span id="health" class="dot"></span>
  <h1>CrashVault</h1>
  <span id="sub" style="color:var(--muted);font-size:13px">connecting…</span>
</header>
<main>
  <div class="toolbar">
    <button onclick="post('/api/snapshot-now')">Snapshot now</button>
    <button onclick="post('/api/gc')">Run GC</button>
    <button onclick="post('/api/verify')">Verify store</button>
  </div>
  <div class="cards" id="cards"></div>
  <section><h2>Tracked files</h2><div id="files"></div></section>
  <section><h2>Snapshots</h2><div id="snaps"></div></section>
  <section><h2>Integrity</h2><div id="verify" class="empty">Click “Verify store”.</div></section>
</main>
<script>
const $ = (id) => document.getElementById(id);
function esc(s) { return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }
function card(label, value) { return `<div class="card"><div class="label">${label}</div><div class="value">${esc(value)}</div></div>`; }
function table(rows, cols) {
  if (!rows.length) return '<div class="empty">Nothing here yet.</div>';
  return '<table><thead><tr>' + cols.map(c => `<th>${c[0]}</th>`).join('') + '</tr></thead><tbody>' +
    rows.map(r => '<tr>' + cols.map(c => `<td class="${c[2]?'mono':''}">${esc(c[1](r))}</td>`).join('') + '</tr>').join('') +
    '</tbody></table>';
}
async function get(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return r.json();
}
async function post(url) {
  try {
    const r = await fetch(url, {method:'POST'});
    const data = await r.json();
    $('verify').innerHTML = data.ok ? '<div class="empty">✓ ' + esc(url) + ' completed</div>'
                                    : '<div class="empty">✗ ' + esc(JSON.stringify(data)) + '</div>';
    refresh();
  } catch (e) {
    $('verify').innerHTML = '<div class="empty">✗ ' + esc(e.message) + '</div>';
  }
}
async function refresh() {
  try {
    const status = await get('/api/status');
    const d = status.data || {};
    $('health').className = 'dot ok';
    $('sub').textContent = `pid ${d.daemon_pid} · ${d.policy} · ${d.store_root}`;
    $('cards').innerHTML = [
      card('Tracked files', d.tracked_files ?? 0),
      card('Manifests', d.manifests ?? 0),
      card('Snapshots', d.snapshots ?? 0),
      card('Versions stored', d.versions_stored ?? 0),
      card('Queue', `${d.queue_depth ?? 0}/${d.queue_capacity ?? 0}`),
      card('Pending WAL', d.pending_wal ?? 0)
    ].join('');

    const list = await get('/api/list');
    $('files').innerHTML = table(list.data.files || [], [
      ['Path', r => r.path, true], ['Versions', r => r.versions],
      ['On disk', r => r.exists ? 'yes' : 'deleted']
    ]);

    const snaps = await get('/api/snapshots');
    $('snaps').innerHTML = table(snaps.data.snapshots || [], [
      ['ID', r => r.id, true], ['Root', r => r.root, true],
      ['Files', r => r.files], ['Timestamp', r => new Date(r.timestamp*1000).toLocaleString()]
    ]);
  } catch (e) {
    $('health').className = 'dot bad';
    $('sub').textContent = 'daemon unreachable: ' + e.message;
  }
}
refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>)HTML";

void handle_client(int fd, const std::string &socket_path) {
    std::string request;
    char buf[4096];
    while (request.find("\r\n\r\n") == std::string::npos &&
           request.size() < 16384) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { close(fd); return; }
        request.append(buf, (size_t)n);
    }

    size_t line_end = request.find("\r\n");
    std::string first = request.substr(0, line_end);
    std::istringstream ls(first);
    std::string method, target;
    ls >> method >> target;
    if (method.empty()) { close(fd); return; }
    if (method == "OPTIONS") {
        send_all(fd, http_response(204, "No Content", "text/plain", ""));
        close(fd);
        return;
    }

    std::string path = target;
    std::string query;
    size_t q = target.find('?');
    if (q != std::string::npos) {
        path  = target.substr(0, q);
        query = target.substr(q + 1);
    }

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        send_all(fd, http_response(200, "OK", "text/html; charset=utf-8",
                                   DASHBOARD_HTML));
        close(fd);
        return;
    }
    if (method == "GET" && path == "/health") {
        send_all(fd, http_response(200, "OK", "text/plain", "ok\n"));
        close(fd);
        return;
    }

    json cmd;
    if (path == "/api/status") {
        cmd["cmd"] = "status";
    } else if (path == "/api/list") {
        cmd["cmd"] = "list";
        std::string root = query_param(query, "root");
        if (!root.empty()) cmd["root"] = root;
    } else if (path == "/api/history") {
        cmd["cmd"] = "history";
        cmd["path"] = query_param(query, "path");
    } else if (path == "/api/snapshots") {
        cmd["cmd"] = "snapshots";
    } else if (path == "/api/verify") {
        cmd["cmd"] = "verify";
    } else if (path == "/api/snapshot-now" && method == "POST") {
        cmd["cmd"] = "snapshot-now";
    } else if (path == "/api/gc" && method == "POST") {
        cmd["cmd"] = "gc";
    } else {
        send_all(fd, http_response(404, "Not Found", "application/json",
                                   R"({"ok":false,"error":"not found"})"));
        close(fd);
        return;
    }

    std::string response = ipc_call(socket_path, cmd);
    if (response.empty()) {
        send_all(fd, http_response(503, "Service Unavailable", "application/json",
            R"({"ok":false,"error":"daemon unavailable"})"));
    } else {
        send_all(fd, http_response(200, "OK", "application/json", response));
    }
    close(fd);
}

} // namespace

int main(int argc, char *argv[]) {
    std::string socket_path = default_socket_path();
    std::string bind_addr = "127.0.0.1";
    int port = 8080;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--socket" && i + 1 < argc) socket_path = argv[++i];
        else if (a == "--bind" && i + 1 < argc) bind_addr = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " [--socket PATH] [--bind ADDR] [--port PORT]\n"
                      << "  Default: http://127.0.0.1:8080 serving the dashboard\n";
            return 0;
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "Invalid bind address: " << bind_addr << "\n";
        return 1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }
    if (listen(fd, 32) != 0) { perror("listen"); return 1; }
    g_listen_fd = fd;

    std::cout << "CrashVault dashboard: http://" << bind_addr << ":" << port
              << "  (daemon socket: " << socket_path << ")\n";

    while (!g_stop.load()) {
        int client = accept(fd, nullptr, nullptr);
        if (client < 0) {
            if (g_stop.load()) break;
            continue;
        }
        std::thread(handle_client, client, socket_path).detach();
    }

    close(fd);
    g_listen_fd = -1;
    return 0;
}
