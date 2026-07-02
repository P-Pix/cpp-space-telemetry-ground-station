#include "stgs/Logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace stgs {

Logger::Logger(std::optional<std::filesystem::path> filePath, LogLevel minLevel)
    : minLevel_(minLevel) {
    if (filePath.has_value()) {
        file_.open(*filePath, std::ios::out | std::ios::app);
        if (!file_) {
            throw std::runtime_error("failed to open log file: " + filePath->string());
        }
    }
}

Logger::~Logger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        file_.flush();
    }
}

void Logger::setMinLevel(LogLevel level) {
    std::lock_guard<std::mutex> lock(mutex_);
    minLevel_ = level;
}

void Logger::log(LogLevel level, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(level) < static_cast<int>(minLevel_)) {
        return;
    }

    const auto line = nowIso8601() + " [" + logLevelToString(level) + "] " + std::string(message);
    if (level >= LogLevel::Warning) {
        std::cerr << line << '\n';
    } else {
        std::cout << line << '\n';
    }
    if (file_) {
        file_ << line << '\n';
    }
}

const char* logLevelToString(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Trace:
        return "TRACE";
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "UNKNOWN";
}

std::string nowIso8601() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto time = clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms << 'Z';
    return oss.str();
}

std::optional<LogLevel> parseLogLevel(std::string_view value) {
    if (value == "trace") {
        return LogLevel::Trace;
    }
    if (value == "debug") {
        return LogLevel::Debug;
    }
    if (value == "info") {
        return LogLevel::Info;
    }
    if (value == "warning" || value == "warn") {
        return LogLevel::Warning;
    }
    if (value == "error") {
        return LogLevel::Error;
    }
    return std::nullopt;
}

} // namespace stgs
