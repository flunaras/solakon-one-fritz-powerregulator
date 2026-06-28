#include "logger.h"

#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

// static
Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::setLevel(int level) {
    if (level < 0) level = 0;
    if (level > 5) level = 5;
    m_level.store(level, std::memory_order_relaxed);
}

bool Logger::is_enabled(LogLevel level) const {
    return static_cast<int>(level) <= m_level.load(std::memory_order_relaxed);
}

void Logger::log(LogLevel level, const std::string& msg) {
    if (!is_enabled(level)) return;

    // Map level to label string.
    static constexpr std::array<const char*, 6> labels{
        "SILENT", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"
    };
    const char* label = labels[static_cast<int>(level)];

    // Build timestamp (ISO 8601 with milliseconds) outside the lock.
    auto now   = std::chrono::system_clock::now();
    auto ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now.time_since_epoch()) % 1000;
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm     tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif

    std::ostringstream line;
    line << '[' << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.'
         << std::setfill('0') << std::setw(3) << ms.count() << "] ["
         << label << "] " << msg << '\n';

    const std::string str = line.str();
    std::lock_guard<std::mutex> lk(m_mutex);
    std::cerr << str;
}
