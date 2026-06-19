// FrameResourceLog.cpp
#include "FrameResourceLog.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>

namespace afw_fr {
namespace {
std::mutex g_mutex;
LogSinkFn g_sink = nullptr;
LogLevel g_level = LogLevel::Info;
std::unordered_set<std::string> g_seen_keys;

void default_sink(LogSeverity severity, const char* message) {
    const char* tag = severity == LogSeverity::Error   ? "ERROR"
                      : severity == LogSeverity::Warn   ? "WARN"
                                                        : "INFO";
    std::fprintf(stderr, "[%s] %s\n", tag, message);
}

void dispatch(LogSeverity severity, const char* body) {
    char line[1024];
    // Prefix every line so logs are greppable regardless of sink.
    std::snprintf(line, sizeof(line), "[FrameResources] %s", body);

    LogSinkFn sink = nullptr;
    {
        std::scoped_lock lock(g_mutex);
        sink = g_sink;
    }
    if (sink != nullptr) {
        sink(severity, line);
    } else {
        default_sink(severity, line);
    }
}
} // namespace

void set_log_sink(LogSinkFn sink) {
    std::scoped_lock lock(g_mutex);
    g_sink = sink;
}

void set_log_level(LogLevel level) {
    std::scoped_lock lock(g_mutex);
    g_level = level;
}

LogLevel log_level() {
    std::scoped_lock lock(g_mutex);
    return g_level;
}

void log_at(LogLevel level, LogSeverity severity, const char* fmt, ...) {
    {
        std::scoped_lock lock(g_mutex);
        if (static_cast<int>(level) > static_cast<int>(g_level)) {
            return;
        }
    }
    char body[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    dispatch(severity, body);
}

void log_info(const char* fmt, ...) {
    {
        std::scoped_lock lock(g_mutex);
        if (static_cast<int>(LogLevel::Info) > static_cast<int>(g_level)) return;
    }
    char body[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    dispatch(LogSeverity::Info, body);
}

void log_debug(const char* fmt, ...) {
    {
        std::scoped_lock lock(g_mutex);
        if (static_cast<int>(LogLevel::Debug) > static_cast<int>(g_level)) return;
    }
    char body[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    dispatch(LogSeverity::Info, body);
}

void log_trace(const char* fmt, ...) {
    {
        std::scoped_lock lock(g_mutex);
        if (static_cast<int>(LogLevel::Trace) > static_cast<int>(g_level)) return;
    }
    char body[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    dispatch(LogSeverity::Info, body);
}

void log_warn(const char* fmt, ...) {
    {
        std::scoped_lock lock(g_mutex);
        if (static_cast<int>(LogLevel::Info) > static_cast<int>(g_level)) return;
    }
    char body[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    dispatch(LogSeverity::Warn, body);
}

void log_error(const char* fmt, ...) {
    // Errors are emitted regardless of level (except Silent).
    {
        std::scoped_lock lock(g_mutex);
        if (g_level == LogLevel::Silent) return;
    }
    char body[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    dispatch(LogSeverity::Error, body);
}

bool log_once_key(const char* key) {
    std::scoped_lock lock(g_mutex);
    auto [it, inserted] = g_seen_keys.insert(key);
    (void)it;
    return inserted;
}

void reset_log_once() {
    std::scoped_lock lock(g_mutex);
    g_seen_keys.clear();
}

} // namespace afw_fr
