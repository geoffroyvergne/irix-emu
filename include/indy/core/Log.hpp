#pragma once

#include <cstdio>
#include <cstdlib>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

// Minimal thread-safe logger. Each message is formatted up front and emitted
// with a single fwrite, so lines from the UI and emulation threads never interleave.
namespace indy::log {

enum class Level { Debug, Info, Warn, Error };

[[nodiscard]] constexpr const char* tag(Level level) noexcept {
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    }
    return "?????";
}

// INDY_VERBOSE=1: also report what IRIX does routinely during bring-up and
// normal running (hardware probes taking bus errors, unemulated registers it
// tolerates, exceptions the kernel handles), which is noise otherwise.
[[nodiscard]] inline bool verbose() noexcept {
    static const bool enabled = std::getenv("INDY_VERBOSE") != nullptr;
    return enabled;
}

// Emit "[tag] message" - used directly by subsystems with their own channel
// names (e.g. "CPU WARNING", "CPU TRACE") so their output is easy to grep.
template <typename... Args>
void tagged(std::string_view tagText, std::format_string<Args...> fmt, Args&&... args) {
    std::string line = std::format("[{}] ", tagText);
    std::format_to(std::back_inserter(line), fmt, std::forward<Args>(args)...);
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), stderr);
}

template <typename... Args>
void write(Level level, std::format_string<Args...> fmt, Args&&... args) {
    tagged(tag(level), fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
#ifndef NDEBUG
    write(Level::Debug, fmt, std::forward<Args>(args)...);
#else
    (void)fmt;
    ((void)args, ...);
#endif
}

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Info, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Warn, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write(Level::Error, fmt, std::forward<Args>(args)...);
}

} // namespace indy::log
