// src/watcher/inotify_watcher.h — inotify + epoll filesystem event watcher
//
// WHY: inotify is the Linux kernel mechanism for watching filesystem events
// without polling. The kernel maintains a watch descriptor table and queues
// events into a character device (/proc/self/fd/N backed by an inotify fd).
// epoll multiplexes the inotify fd with the self-pipe (signals) and an
// eventfd (shutdown notification) so the watcher thread blocks efficiently
// in epoll_wait and is woken by any of them.
//
// Key inotify flags used:
//   IN_CLOSE_WRITE  — file closed after writing (most reliable for editors)
//   IN_MOVED_TO     — file renamed into the watch dir (atomic rename pattern)
//   IN_CREATE       — new file or directory created
//   IN_DELETE       — file deleted
//   IN_MOVED_FROM   — file renamed out of watch dir (paired with MOVED_TO by cookie)
//   IN_Q_OVERFLOW   — event queue overflowed; we must rescan
//   IN_ONLYDIR      — fail if path is not a directory (safe recursive watch add)
//   IN_DONT_FOLLOW  — don't follow symlinks (avoid loops)
//   IN_EXCL_UNLINK  — don't report events for files already unlinked
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <thread>
#include <atomic>
#include <cstdint>
#include <sys/inotify.h>  // for struct inotify_event — must be in header

namespace cv {

// A settled file event passed from the watcher to the debouncer.
struct RawEvent {
    enum class Kind { MODIFIED, DELETED, MOVED_FROM, MOVED_TO, OVERFLOW };
    Kind        kind;
    std::string path;      // absolute path
    std::string dest_path; // only for MOVED_TO (rename destination)
    uint32_t    cookie;    // inotify rename cookie for pairing MOVED_FROM/TO
};

class Config; // forward decl

// Callback type: called when a raw event is ready
using RawEventCallback = std::function<void(RawEvent)>;

class InotifyWatcher {
public:
    explicit InotifyWatcher(const Config &cfg, RawEventCallback cb);
    ~InotifyWatcher();

    // Add a root directory (and recurse into subdirectories)
    void add_root(const std::string &path);

    // Remove all watches for a root directory
    void remove_root(const std::string &path);

    // Start the watcher thread (blocks internally via epoll_wait)
    void start();

    // Signal the watcher thread to stop and join it
    void stop();

    // Returns the current count of active watches
    size_t watch_count() const { return wd_to_path_.size(); }

    // Returns true if the event queue overflowed since last call
    bool check_and_clear_overflow() {
        return overflow_.exchange(false);
    }

private:
    // Add a single directory watch; returns watch descriptor
    int add_watch(const std::string &dirpath);

    // Recursively add watches for dirpath and all subdirectories
    void add_watches_recursive(const std::string &dirpath);

    // Remove watch by watch descriptor
    void remove_watch(int wd);

    // The main event loop — runs in watcher_thread_
    void event_loop();

    // Process one inotify_event struct (from <sys/inotify.h>)
    void handle_inotify_event(const struct inotify_event *ev);

    // Handle IN_Q_OVERFLOW: log, rescan all roots
    void handle_overflow();

    // Check if a filename matches any ignore pattern
    bool is_ignored(const std::string &name) const;

    // Pending MOVED_FROM events waiting to be paired with MOVED_TO
    struct PendingMove {
        std::string from_path;
        uint64_t    timestamp_ms;
    };

    int                                     inotify_fd_{-1};
    int                                     epoll_fd_{-1};
    int                                     shutdown_fd_{-1}; // eventfd
    std::unordered_map<int, std::string>    wd_to_path_;      // wd -> absolute dir
    std::unordered_map<std::string, int>    path_to_wd_;      // dir -> wd
    std::unordered_map<uint32_t, PendingMove> pending_moves_; // cookie -> move info
    std::vector<std::string>                roots_;
    RawEventCallback                        callback_;        // declared before ignore_patterns_
    std::vector<std::string>                ignore_patterns_;
    std::thread                             thread_;
    std::atomic<bool>                       running_{false};
    std::atomic<bool>                       overflow_{false};
    int                                     signal_pipe_fd_{-1}; // read end of self-pipe
};

} // namespace cv
