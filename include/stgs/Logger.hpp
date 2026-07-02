#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace stgs {

enum class LogLevel {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4
};

class Logger {
public:
    explicit Logger(std::optional<std::filesystem::path> filePath = std::nullopt,
                    LogLevel minLevel = LogLevel::Info);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void setMinLevel(LogLevel level);
    void log(LogLevel level, std::string_view message);
    void trace(std::string_view message) { log(LogLevel::Trace, message); }
    void debug(std::string_view message) { log(LogLevel::Debug, message); }
    void info(std::string_view message) { log(LogLevel::Info, message); }
    void warning(std::string_view message) { log(LogLevel::Warning, message); }
    void error(std::string_view message) { log(LogLevel::Error, message); }

private:
    std::mutex mutex_;
    std::ofstream file_;
    LogLevel minLevel_;
};

const char* logLevelToString(LogLevel level) noexcept;
std::string nowIso8601();
std::optional<LogLevel> parseLogLevel(std::string_view value);

} // namespace stgs
