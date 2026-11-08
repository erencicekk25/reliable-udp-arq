#pragma once

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>

namespace rudp::log {

enum class Level { Info, Send, Recv, Retransmit, Loss, Duplicate, Warn, Error };

namespace detail {

inline bool& enabledFlag() {
    static bool enabled = true;
    return enabled;
}

inline std::mutex& outputMutex() {
    static std::mutex m;
    return m;
}

inline const std::chrono::steady_clock::time_point& startTime() {
    static const auto start = std::chrono::steady_clock::now();
    return start;
}

inline bool useColor() {
    static const bool color = ::isatty(STDERR_FILENO) != 0;
    return color;
}

inline const char* tag(Level level) {
    switch (level) {
        case Level::Info:       return "INFO ";
        case Level::Send:       return "SEND ";
        case Level::Recv:       return "RECV ";
        case Level::Retransmit: return "RETX ";
        case Level::Loss:       return "LOSS ";
        case Level::Duplicate:  return "DUP  ";
        case Level::Warn:       return "WARN ";
        case Level::Error:      return "ERROR";
    }
    return "?????";
}

inline const char* color(Level level) {
    switch (level) {
        case Level::Info:       return "\033[36m";  // cyan
        case Level::Send:       return "\033[32m";  // green
        case Level::Recv:       return "\033[34m";  // blue
        case Level::Retransmit: return "\033[35m";  // magenta
        case Level::Loss:       return "\033[31m";  // red
        case Level::Duplicate:  return "\033[33m";  // yellow
        case Level::Warn:       return "\033[33m";  // yellow
        case Level::Error:      return "\033[1;31m";  // bold red
    }
    return "";
}

}  // namespace detail

// Enables/disables protocol-level logging. Errors are always printed.
inline void setEnabled(bool enabled) { detail::enabledFlag() = enabled; }

// All logs go to stderr so stdout stays free for application data.
inline void write(Level level, const std::string& message) {
    if (!detail::enabledFlag() && level != Level::Error) return;

    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - detail::startTime()).count();

    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "[%9.3f]", elapsed);

    std::lock_guard<std::mutex> lock(detail::outputMutex());
    if (detail::useColor()) {
        std::cerr << stamp << ' ' << detail::color(level) << '[' << detail::tag(level) << "]\033[0m "
                  << message << '\n';
    } else {
        std::cerr << stamp << " [" << detail::tag(level) << "] " << message << '\n';
    }
}

}  // namespace rudp::log
