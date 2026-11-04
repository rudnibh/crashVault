// src/watcher/inotify_watcher.cc — inotify + epoll implementation
//
// DESIGN NOTE: We use epoll (not select/poll) because:
//   1. O(1) scalability: epoll_wait is O(active events), not O(watched fds).
//   2. Level-triggered mode: we don't miss events if we don't drain immediately.
//   3. We multiplex three fds: inotify_fd, eventfd(shutdown), self-pipe(signals).
//
// RECURSIVE WATCHING: inotify watches directories, not individual files.
// When a new directory is created (IN_CREATE + IN_ISDIR), we immediately
// add a watch for it.  On rename (IN_MOVED_TO + IN_ISDIR), same.
// This covers the common case but has a TOCTOU window: files created between
// the CREATE event and our add_watch call may be missed.  On IN_Q_OVERFLOW
// we do a full rescan to close this gap.

#include "inotify_watcher.h"
#include "../daemon/config.h"
#include "../daemon/logger.h"
#include <sys/inotify.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cassert>
#include <fnmatch.h>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <sstream>

namespace cv {

// inotify mask for file events we care about.
// WHY each flag:
//   IN_CLOSE_WRITE  - Editor wrote and closed the file (reliable "done writing")
//   IN_MOVED_TO     - Atomic rename pattern: editor writes tmp, renames to target
//   IN_CREATE       - New file created (also need to watch new subdirs)
//   IN_DELETE       - File removed
//   IN_MOVED_FROM   - File moved out (pair with MOVED_TO via cookie)
//   IN_EXCL_UNLINK  - Don't report events for already-unlinked files (temp files)
//   IN_DONT_FOLLOW  - Don't follow symlinks (avoid infinite loops)
static constexpr uint32_t WATCH_MASK =
    IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE |
    IN_MOVED_FROM | IN_EXCL_UNLINK | IN_DONT_FOLLOW;

static uint64_t now_ms() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

InotifyWatcher::InotifyWatcher(const Config &cfg, RawEventCallback cb)
    : callback_(std::move(cb))
    , ignore_patterns_(cfg.ignore_patterns)
{
    // Create inotify instance
    // WHY IN_NONBLOCK: so reads in the event loop don't block; we use
    // epoll_wait to block instead.
    // WHY IN_CLOEXEC: don't leak fd into child processes.
    inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ < 0) {
        throw std::runtime_error(std::string("inotify_init1: ") + strerror(errno));
    }

    // Create epoll instance
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        close(inotify_fd_);
        throw std::runtime_error(std::string("epoll_create1: ") + strerror(errno));
    }

    // Create eventfd for shutdown notification
    // WHY eventfd: a single write of value 1 makes it readable; epoll can
    // multiplex it alongside inotify_fd.
    shutdown_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (shutdown_fd_ < 0) {
        close(epoll_fd_); close(inotify_fd_);
        throw std::runtime_error(std::string("eventfd: ") + strerror(errno));
    }

    // Register fds with epoll (level-triggered, EPOLLIN)
    auto epoll_add = [&](int fd, uint64_t user_data) {
        struct epoll_event ev{};
        ev.events   = EPOLLIN;
        ev.data.u64 = user_data;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
            throw std::runtime_error(std::string("epoll_ctl ADD: ") + strerror(errno));
        }
    };
    epoll_add(inotify_fd_, 1); // user_data=1 → inotify events
    epoll_add(shutdown_fd_, 2); // user_data=2 → shutdown

    // Add configured roots
    for (auto &root : cfg.watched_roots) {
        roots_.push_back(root);
    }
}

InotifyWatcher::~InotifyWatcher() {
    stop();
    if (shutdown_fd_ >= 0) close(shutdown_fd_);
    if (epoll_fd_   >= 0) close(epoll_fd_);
    if (inotify_fd_ >= 0) close(inotify_fd_);
}

bool InotifyWatcher::is_ignored(const std::string &name) const {
    for (auto &pat : ignore_patterns_) {
        // fnmatch with FNM_PATHNAME=0 so * matches slashes too within pattern
        if (fnmatch(pat.c_str(), name.c_str(), FNM_NOESCAPE) == 0) return true;
        // Also check just the basename
        size_t slash = name.rfind('/');
        std::string base = (slash != std::string::npos) ? name.substr(slash+1) : name;
        if (fnmatch(pat.c_str(), base.c_str(), FNM_NOESCAPE) == 0) return true;
    }
    return false;
}

int InotifyWatcher::add_watch(const std::string &dirpath) {
    if (is_ignored(dirpath)) return -1;

    int wd = inotify_add_watch(inotify_fd_, dirpath.c_str(), WATCH_MASK);
    if (wd < 0) {
        if (errno == ENOSPC) {
            // WHY: /proc/sys/fs/inotify/max_user_watches exhausted.
            // Log a clear error — user must raise the kernel limit.
            LOG_ERROR("inotify watch limit exhausted! Run: "
                      "echo 524288 | sudo tee /proc/sys/fs/inotify/max_user_watches");
        } else if (errno != ENOENT && errno != ENOTDIR) {
            LOG_WARN("inotify_add_watch(" + dirpath + "): " + strerror(errno));
        }
        return -1;
    }

    // inotify reuses watch descriptors when you re-add an existing path.
    // Store the mapping in both directions.
    wd_to_path_[wd] = dirpath;
    path_to_wd_[dirpath] = wd;
    LOG_DEBUG("Watching: " + dirpath + " (wd=" + std::to_string(wd) + ")");
    return wd;
}

void InotifyWatcher::add_watches_recursive(const std::string &dirpath) {
    add_watch(dirpath);

    // Recurse using opendir/readdir (safe with inotify — no fd leak risk)
    DIR *dir = opendir(dirpath.c_str());
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue; // skip . .. .hidden
        if (ent->d_type != DT_DIR) continue;
        std::string child = dirpath + "/" + ent->d_name;
        if (is_ignored(child) || is_ignored(ent->d_name)) continue;
        add_watches_recursive(child);
    }
    closedir(dir);
}

void InotifyWatcher::add_root(const std::string &path) {
    roots_.push_back(path);
    add_watches_recursive(path);
}

void InotifyWatcher::remove_root(const std::string &path) {
    roots_.erase(std::remove(roots_.begin(), roots_.end(), path), roots_.end());
    // Remove all watches whose path starts with this root
    std::vector<int> to_remove;
    for (auto &[wd, dir] : wd_to_path_) {
        if (dir.rfind(path, 0) == 0) to_remove.push_back(wd);
    }
    for (int wd : to_remove) remove_watch(wd);
}

void InotifyWatcher::remove_watch(int wd) {
    auto it = wd_to_path_.find(wd);
    if (it != wd_to_path_.end()) {
        path_to_wd_.erase(it->second);
        wd_to_path_.erase(it);
    }
    inotify_rm_watch(inotify_fd_, wd);
}

void InotifyWatcher::start() {
    // Add all configured roots
    for (auto &root : roots_) {
        add_watches_recursive(root);
    }
    running_.store(true);
    thread_ = std::thread(&InotifyWatcher::event_loop, this);
}

void InotifyWatcher::stop() {
    if (!running_.exchange(false)) return;
    // Write to eventfd to wake up epoll_wait in the event loop
    uint64_t val = 1;
    (void)write(shutdown_fd_, &val, sizeof(val));
    if (thread_.joinable()) thread_.join();
}

void InotifyWatcher::handle_overflow() {
    // WHY: When the inotify event queue overflows (too many events queued
    // faster than we drain them), the kernel reports IN_Q_OVERFLOW.
    // We CANNOT know what events were dropped, so we must do a full rescan:
    // compare the current filesystem state to what we know, and emit
    // MODIFIED events for any files whose mtime changed.
    LOG_WARN("inotify queue overflow — triggering full rescan");
    overflow_.store(true);

    // Emit a synthetic OVERFLOW event so the debouncer can trigger a rescan
    RawEvent ev;
    ev.kind = RawEvent::Kind::OVERFLOW;
    callback_(std::move(ev));
}

void InotifyWatcher::handle_inotify_event(const ::inotify_event *ev) {
    // Find the directory path for this watch descriptor
    auto it = wd_to_path_.find(ev->wd);
    if (it == wd_to_path_.end()) return; // stale wd (dir was deleted)

    const std::string &dir = it->second;
    std::string name = (ev->len > 0) ? std::string(ev->name) : "";
    std::string full_path = dir + (name.empty() ? "" : "/" + name);

    // IN_Q_OVERFLOW: ev->wd == -1
    if (ev->mask & IN_Q_OVERFLOW) {
        handle_overflow();
        return;
    }

    // Skip ignored files/directories
    if (!name.empty() && is_ignored(name)) return;

    // Handle new subdirectory — add it to our watch set
    if ((ev->mask & IN_CREATE) && (ev->mask & IN_ISDIR)) {
        add_watches_recursive(full_path);
        return; // directory itself not versioned
    }
    if ((ev->mask & IN_MOVED_TO) && (ev->mask & IN_ISDIR)) {
        add_watches_recursive(full_path);
        // Also need to remove the old watch if it was a rename within our tree
        return;
    }
    if ((ev->mask & IN_DELETE) && (ev->mask & IN_ISDIR)) {
        // The watch is automatically removed by inotify when dir is deleted
        auto wit = path_to_wd_.find(full_path);
        if (wit != path_to_wd_.end()) {
            wd_to_path_.erase(wit->second);
            path_to_wd_.erase(wit);
        }
        return;
    }

    // File events
    if (ev->mask & (IN_CLOSE_WRITE | IN_CREATE)) {
        if (ev->mask & IN_ISDIR) return;
        RawEvent re;
        re.kind = RawEvent::Kind::MODIFIED;
        re.path = full_path;
        callback_(std::move(re));
    }
    else if (ev->mask & IN_MOVED_FROM) {
        // Stash this rename — we'll see MOVED_TO with same cookie shortly
        pending_moves_[ev->cookie] = { full_path, now_ms() };
        // Also emit DELETED for now; if MOVED_TO arrives, we'll emit MOVED_TO
        RawEvent re;
        re.kind   = RawEvent::Kind::MOVED_FROM;
        re.path   = full_path;
        re.cookie = ev->cookie;
        callback_(std::move(re));
    }
    else if (ev->mask & IN_MOVED_TO) {
        RawEvent re;
        re.kind   = RawEvent::Kind::MOVED_TO;
        re.path   = full_path;
        re.cookie = ev->cookie;
        // Pair with pending MOVED_FROM if available
        auto mit = pending_moves_.find(ev->cookie);
        if (mit != pending_moves_.end()) {
            re.dest_path = mit->second.from_path;
            pending_moves_.erase(mit);
        }
        callback_(std::move(re));
    }
    else if (ev->mask & IN_DELETE) {
        RawEvent re;
        re.kind = RawEvent::Kind::DELETED;
        re.path = full_path;
        callback_(std::move(re));
    }
}

void InotifyWatcher::event_loop() {
    // Buffer for reading inotify events.
    // WHY 4096+sizeof(inotify_event)+NAME_MAX+1: the kernel copies whole events;
    // each event is variable-length (name can be up to NAME_MAX=255 bytes).
    constexpr size_t BUF_SIZE = 4096 + sizeof(struct inotify_event) + NAME_MAX + 1;
    alignas(struct inotify_event) char buf[BUF_SIZE];

    struct epoll_event events[16];

    // Expire pending moves after 1 second (cookie pairing window)
    constexpr uint64_t MOVE_EXPIRY_MS = 1000;

    while (running_.load()) {
        // epoll_wait blocks until an event arrives or timeout expires.
        // Timeout = 500ms so we can periodically expire stale pending_moves.
        int nfds = epoll_wait(epoll_fd_, events, 16, 500);

        if (nfds < 0) {
            if (errno == EINTR) continue; // signal interrupted us — loop back
            LOG_ERROR(std::string("epoll_wait: ") + strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; i++) {
            uint64_t ud = events[i].data.u64;
            if (ud == 2) {
                // Shutdown eventfd triggered
                return;
            }
            if (ud != 1) continue; // shouldn't happen

            // Drain all available inotify events from the fd
            ssize_t len;
            while ((len = read(inotify_fd_, buf, BUF_SIZE)) > 0) {
                for (char *p = buf; p < buf + len; ) {
                    auto *ev = reinterpret_cast<struct inotify_event *>(p);
                    handle_inotify_event(ev);
                    p += sizeof(struct inotify_event) + ev->len;
                }
            }
            if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR(std::string("read inotify: ") + strerror(errno));
            }
        }

        // Expire stale pending_moves (MOVED_FROM without a matching MOVED_TO)
        uint64_t now = now_ms();
        for (auto it = pending_moves_.begin(); it != pending_moves_.end(); ) {
            if (now - it->second.timestamp_ms > MOVE_EXPIRY_MS) {
                // The file was moved out of the watched tree — treat as DELETE
                RawEvent re;
                re.kind   = RawEvent::Kind::DELETED;
                re.path   = it->second.from_path;
                re.cookie = it->first;
                callback_(std::move(re));
                it = pending_moves_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

} // namespace cv
