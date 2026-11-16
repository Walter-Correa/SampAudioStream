#include "sampaudiostream/core/logger.hpp"

#include <iostream>

namespace sampaudiostream {

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::SetSink(SinkFn sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

void Logger::SetDebugEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    debug_enabled_ = enabled;
}

bool Logger::IsDebugEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return debug_enabled_;
}

void Logger::Log(LogSeverity severity, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (severity == LogSeverity::Debug && !debug_enabled_) {
        return;
    }
    pending_logs_.emplace(severity, message);
}

void Logger::Debug(const std::string& message) {
    Log(LogSeverity::Debug, message);
}

void Logger::Info(const std::string& message) {
    Log(LogSeverity::Info, message);
}

void Logger::Warn(const std::string& message) {
    Log(LogSeverity::Warning, message);
}

void Logger::Error(const std::string& message) {
    Log(LogSeverity::Error, message);
}

void Logger::Flush() {
    std::queue<std::pair<LogSeverity, std::string>> local_queue;
    SinkFn local_sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_logs_.empty()) {
            return;
        }
        std::swap(local_queue, pending_logs_);
        local_sink = sink_;
    }

    while (!local_queue.empty()) {
        auto [severity, msg] = std::move(local_queue.front());
        local_queue.pop();

        if (local_sink) {
            local_sink(severity, msg);
        } else {
            const char* prefix = "[INFO]";
            switch (severity) {
                case LogSeverity::Debug: prefix = "[DEBUG]"; break;
                case LogSeverity::Info: prefix = "[INFO]"; break;
                case LogSeverity::Warning: prefix = "[WARN]"; break;
                case LogSeverity::Error: prefix = "[ERROR]"; break;
            }
            std::cout << "[SampAudioStream] " << prefix << " " << msg << '\n';
        }
    }
}

} // namespace sampaudiostream
