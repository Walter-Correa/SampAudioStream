#pragma once

#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <utility>

namespace sampaudiostream {

enum class LogSeverity : int {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3
};

class Logger {
public:
    using SinkFn = std::function<void(LogSeverity, const std::string&)>;

    static Logger& Instance();

    void SetSink(SinkFn sink);
    void SetDebugEnabled(bool enabled);
    bool IsDebugEnabled() const;

    void Log(LogSeverity severity, const std::string& message);
    void Debug(const std::string& message);
    void Info(const std::string& message);
    void Warn(const std::string& message);
    void Error(const std::string& message);

    // Must be called from the server's main thread tick to safely flush cross-thread logs
    void Flush();

private:
    Logger() = default;

    mutable std::mutex mutex_;
    SinkFn sink_;
    bool debug_enabled_{false};
    std::queue<std::pair<LogSeverity, std::string>> pending_logs_;
};

} // namespace sampaudiostream
