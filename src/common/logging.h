// Hệ thống ghi nhật ký: nhiều mức, ghi ra màn hình + tệp xoay vòng + bộ đệm cho giao diện.
#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ttd {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Off = 5,
};

const char* logLevelName(LogLevel lv);
const char* logLevelNameVi(LogLevel lv);
LogLevel parseLogLevel(const std::string& s, LogLevel def = LogLevel::Info);

struct LogRecord {
    uint64_t seq = 0;
    int64_t timeMillis = 0;
    LogLevel level = LogLevel::Info;
    std::string tag;
    std::string message;
};

// Bộ ghi nhật ký toàn cục (singleton).
class Logger {
public:
    static Logger& instance();

    void configure(LogLevel level, const std::string& filepath, uint64_t maxFileBytes,
                   int maxFiles, bool console, size_t memoryCapacity);
    void setLevel(LogLevel level);
    LogLevel level() const { return level_.load(std::memory_order_relaxed); }
    bool enabled(LogLevel lv) const { return static_cast<int>(lv) >= static_cast<int>(level()); }

    void log(LogLevel lv, const std::string& tag, const std::string& message);
    void logf(LogLevel lv, const std::string& tag, const char* fmt, ...);

    // Lấy các bản ghi có seq > afterSeq (tối đa `limit`), lọc theo mức và từ khoá.
    std::vector<LogRecord> recent(uint64_t afterSeq, size_t limit, LogLevel minLevel,
                                  const std::string& filter) const;
    uint64_t lastSeq() const;
    void clearMemory();

    // Đăng ký hàm nhận bản ghi mới (dùng cho luồng SSE tới trình duyệt).
    int addListener(std::function<void(const LogRecord&)> fn);
    void removeListener(int id);

    // Thống kê số lượng theo mức, để hiển thị trên giao diện.
    void counters(uint64_t out[5]) const;

private:
    Logger() = default;
    void writeFileLocked(const std::string& line);
    void rotateLocked();

    std::atomic<LogLevel> level_{LogLevel::Info};
    mutable std::mutex mu_;
    std::deque<LogRecord> memory_;
    size_t memoryCapacity_ = 5000;
    uint64_t seq_ = 0;
    uint64_t counters_[5] = {0, 0, 0, 0, 0};

    std::string filepath_;
    uint64_t maxFileBytes_ = 16 * 1024 * 1024;
    int maxFiles_ = 5;
    uint64_t curFileBytes_ = 0;
    void* fileHandle_ = nullptr;  // FILE*
    bool console_ = true;

    mutable std::mutex listenerMu_;
    std::vector<std::pair<int, std::function<void(const LogRecord&)>>> listeners_;
    int nextListenerId_ = 1;
};

// Hàm tiện dụng.
void logTrace(const std::string& tag, const std::string& msg);
void logDebug(const std::string& tag, const std::string& msg);
void logInfo(const std::string& tag, const std::string& msg);
void logWarn(const std::string& tag, const std::string& msg);
void logError(const std::string& tag, const std::string& msg);

#define TTD_LOG(level, tag, ...)                                       \
    do {                                                               \
        if (::ttd::Logger::instance().enabled(level))                  \
            ::ttd::Logger::instance().logf(level, tag, __VA_ARGS__);   \
    } while (0)

#define LOG_TRACE(tag, ...) TTD_LOG(::ttd::LogLevel::Trace, tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...) TTD_LOG(::ttd::LogLevel::Debug, tag, __VA_ARGS__)
#define LOG_INFO(tag, ...) TTD_LOG(::ttd::LogLevel::Info, tag, __VA_ARGS__)
#define LOG_WARN(tag, ...) TTD_LOG(::ttd::LogLevel::Warn, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) TTD_LOG(::ttd::LogLevel::Error, tag, __VA_ARGS__)

}  // namespace ttd
