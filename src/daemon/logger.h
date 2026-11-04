// src/daemon/logger.h — Structured logging (thread-safe, async-signal-safe output)
//
// WHY: std::cout and printf are NOT async-signal-safe. We write to a file
// descriptor using write(2) which IS safe. All log lines are prefixed with
// ISO-8601 timestamp, level, and thread ID so multi-threaded output is
// parseable.
#pragma once
#include <string>
#include <cstdint>

namespace cv {

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

void log_init(const std::string &log_path, LogLevel min_level, bool also_stderr);
void log_close();

// Thread-safe log functions — write(2) on Linux with O_APPEND is atomic
// for writes <= PIPE_BUF (4096) bytes, so short log lines are never interleaved.
void log_msg(LogLevel level, const char *file, int line, const std::string &msg);

#define LOG_DEBUG(msg) cv::log_msg(cv::LogLevel::DEBUG, __FILE__, __LINE__, msg)
#define LOG_INFO(msg)  cv::log_msg(cv::LogLevel::INFO,  __FILE__, __LINE__, msg)
#define LOG_WARN(msg)  cv::log_msg(cv::LogLevel::WARN,  __FILE__, __LINE__, msg)
#define LOG_ERROR(msg) cv::log_msg(cv::LogLevel::ERROR, __FILE__, __LINE__, msg)

} // namespace cv
