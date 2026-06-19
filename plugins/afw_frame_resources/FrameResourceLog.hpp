// FrameResourceLog.hpp
//
// Tiny, sink-based, level-gated logger shared by the tracker core, the providers, and the
// standalone self-test exe. The core stays decoupled from the UEVR SDK: Plugin.cpp installs a
// sink that routes lines to API::get()->log_info/warn/error; the self-test exe installs a sink
// that prints to stdout. If no sink is installed, lines fall back to stderr.
//
// Every emitted line is prefixed with "[FrameResources] " by the sink dispatcher.
#pragma once

#include <cstdint>

namespace afw_fr {

enum class LogLevel : int {
    Silent = 0,
    Info = 1,
    Debug = 2,
    Trace = 3,
};

// severity passed to the sink so it can pick log_info vs log_warn vs log_error.
enum class LogSeverity : int {
    Info = 0,
    Warn = 1,
    Error = 2,
};

// sink receives an already-formatted, already-prefixed message.
using LogSinkFn = void (*)(LogSeverity severity, const char* message);

void set_log_sink(LogSinkFn sink);
void set_log_level(LogLevel level);
LogLevel log_level();

// printf-style. Gated: dropped when `level` is more verbose than the active log level.
void log_at(LogLevel level, LogSeverity severity, const char* fmt, ...);

// Convenience wrappers (Info severity unless noted).
void log_info(const char* fmt, ...);
void log_debug(const char* fmt, ...);
void log_trace(const char* fmt, ...);
void log_warn(const char* fmt, ...);
void log_error(const char* fmt, ...);

// Returns true the first time it sees a given key (per process), false afterwards.
// Used to dedup high-frequency candidate/bind lines in trace mode.
bool log_once_key(const char* key);
void reset_log_once();

} // namespace afw_fr
