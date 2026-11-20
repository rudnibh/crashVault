// src/daemon/config.cc — INI config parser
// WHY: A simple hand-written INI parser avoids external dependencies while
// remaining straightforward to audit.  Format:
//   [section]
//   key = value
//   key = value   # trailing comments stripped
//   # full-line comments
//   ; also comments

#include "config.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <climits>
#include <exception>
#include <stdexcept>

namespace cv {

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string strip_comment(const std::string &s) {
    // Strip trailing # or ; comments (not inside quotes — simple version)
    size_t p = s.find('#');
    if (p != std::string::npos) return trim(s.substr(0, p));
    p = s.find(';');
    if (p != std::string::npos) return trim(s.substr(0, p));
    return s;
}

static std::vector<std::string> split_csv(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = trim(tok);
        if (!tok.empty()) out.push_back(tok);
    }
    return out;
}

static int parse_int(const std::string &s) {
    size_t pos = 0;
    int v = std::stoi(s, &pos);
    if (pos != s.size())
        throw std::invalid_argument("not an integer: " + s);
    return v;
}

static size_t parse_megabytes(const std::string &s) {
    size_t pos = 0;
    long long mb = std::stoll(s, &pos);
    if (pos != s.size())
        throw std::invalid_argument("not an integer: " + s);
    if (mb < 0 || mb > 1024 * 1024)
        throw std::out_of_range("megabyte value out of range");
    return static_cast<size_t>(mb) * 1024 * 1024;
}

bool Config::load(const std::string &path, std::string &error) {
    // Set store_root default to ~/.crashvault
    if (store_root.empty()) {
        const char *home = getenv("HOME");
        store_root = home ? std::string(home) + "/.crashvault" : "/tmp/.crashvault";
    }

    // Set sensible default ignore patterns
    ignore_patterns = {"*.swp", "*~", ".git/", ".crashvault/",
                       "*.tmp", "*.lock", ".DS_Store"};

    if (path.empty()) return true; // No config file, use defaults

    std::ifstream f(path);
    if (!f) {
        error = "Cannot open config file: " + path;
        return false;
    }

    std::string section;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end == std::string::npos) {
                error = "Malformed section at line " + std::to_string(lineno);
                return false;
            }
            section = trim(line.substr(1, end - 1));
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue; // skip bare keys
        std::string key = trim(line.substr(0, eq));
        std::string val = strip_comment(trim(line.substr(eq + 1)));

        try {
            // [daemon] section
            if (section == "daemon") {
                if (key == "worker_count")        worker_count = parse_int(val);
                else if (key == "memory_cap_mb")  memory_cap_bytes = parse_megabytes(val);
                else if (key == "debounce_ms")    debounce_ms = parse_int(val);
                else if (key == "scheduling_policy") scheduling_policy = val;
                else if (key == "store_root")     store_root = val;
                else if (key == "mmap_threshold_mb") mmap_threshold_bytes = parse_megabytes(val);
                else if (key == "snapshot_quiescence_s") snapshot_quiescence_s = parse_int(val);
                else if (key == "gateway_port")   gateway_port = parse_int(val);
            }
            // [retention] section
            else if (section == "retention") {
                if (key == "versions")            retention_versions = parse_int(val);
                else if (key == "days")           retention_days = parse_int(val);
            }
            // [watch] section — multi-value keys
            else if (section == "watch") {
                if (key == "root") {
                    watched_roots.push_back(val);
                } else if (key == "roots") {
                    auto roots = split_csv(val);
                    watched_roots.insert(watched_roots.end(), roots.begin(), roots.end());
                } else if (key == "ignore") {
                    ignore_patterns.push_back(val);
                } else if (key == "ignores") {
                    auto pats = split_csv(val);
                    ignore_patterns.insert(ignore_patterns.end(), pats.begin(), pats.end());
                }
            }
        } catch (const std::exception &e) {
            error = "Invalid value at line " + std::to_string(lineno) +
                    " (" + key + " = " + val + "): " + e.what();
            return false;
        }
    }

    // Validation. Reject values that would make a subsystem misbehave rather
    // than silently accepting them and failing later at runtime.
    if (worker_count < 1 || worker_count > 64) {
        error = "worker_count must be 1–64";
        return false;
    }
    if (debounce_ms < 0 || debounce_ms > 600000) {
        error = "debounce_ms must be between 0 and 600000";
        return false;
    }
    if (memory_cap_bytes < 1024 * 1024) {
        error = "memory_cap_mb must be at least 1";
        return false;
    }
    if (snapshot_quiescence_s < 0) {
        error = "snapshot_quiescence_s must be >= 0 (0 disables auto-snapshots)";
        return false;
    }
    if (gateway_port < 0 || gateway_port > 65535) {
        error = "gateway_port must be 0–65535";
        return false;
    }
    if (scheduling_policy != "fcfs" && scheduling_policy != "priority") {
        error = "scheduling_policy must be 'fcfs' or 'priority'";
        return false;
    }
    return true;
}

bool Config::reload(const std::string &path, std::string &error) {
    Config fresh;
    fresh.store_root = store_root; // keep existing store root if not in file
    if (!fresh.load(path, error)) return false;
    *this = fresh;
    return true;
}

void Config::dump() const {
    std::cerr << "[config] store_root=" << store_root << "\n";
    std::cerr << "[config] worker_count=" << worker_count << "\n";
    std::cerr << "[config] memory_cap=" << (memory_cap_bytes/1024/1024) << "MiB\n";
    std::cerr << "[config] debounce_ms=" << debounce_ms << "\n";
    std::cerr << "[config] scheduling_policy=" << scheduling_policy << "\n";
    std::cerr << "[config] watched_roots=";
    for (auto &r : watched_roots) std::cerr << r << " ";
    std::cerr << "\n";
    std::cerr << "[config] ignore_patterns=";
    for (auto &p : ignore_patterns) std::cerr << p << " ";
    std::cerr << "\n";
}

} // namespace cv
