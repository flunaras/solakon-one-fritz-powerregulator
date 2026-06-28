#pragma once

#include <atomic>
#include <mutex>
#include <string>

// Log levels — ERR not ERROR to avoid platform macro collision.
enum class LogLevel { SILENT = 0, ERR = 1, WARN = 2, INFO = 3, DBG = 4, TRACE = 5 };

// Thread-safe logger singleton.  All output goes to stderr only.
// Log line format: [2026-06-27T14:03:01.123] [INFO] <message>
class Logger {
public:
    // Returns the singleton instance (thread-safe, C++11 function-local static).
    static Logger& instance();

    // Sets the active log level (0–5).  Values outside range are clamped.
    void setLevel(int level);

    // Returns true if messages at the given level would actually be emitted.
    // Callers should guard expensive string construction with this check.
    [[nodiscard]] bool is_enabled(LogLevel level) const;

    // Emit one log message at the given level.
    void log(LogLevel level, const std::string& msg);

    // Convenience wrappers.
    void error(const std::string& m) { log(LogLevel::ERR,   m); }
    void warn (const std::string& m) { log(LogLevel::WARN,  m); }
    void info (const std::string& m) { log(LogLevel::INFO,  m); }
    void debug(const std::string& m) { log(LogLevel::DBG,   m); }
    void trace(const std::string& m) { log(LogLevel::TRACE, m); }

private:
    Logger() = default;
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    std::atomic<int> m_level{1};  // default: ERR
    std::mutex       m_mutex;
};
