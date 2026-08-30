#include "common/logging.h"

#include <cstdio>
#include <cstring>

#include "common/fsutil.h"
#include "common/strutil.h"
#include "common/timeutil.h"

namespace ttd {

const char* logLevelName(LogLevel lv) {
    switch (lv) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        default: return "OFF";
    }
}

const char* logLevelNameVi(LogLevel lv) {
    switch (lv) {
        case LogLevel::Trace: return "Chi tiết";
        case LogLevel::Debug: return "Gỡ lỗi";
        case LogLevel::Info: return "Thông tin";
        case LogLevel::Warn: return "Cảnh báo";
        case LogLevel::Error: return "Lỗi";
        default: return "Tắt";
    }
}

LogLevel parseLogLevel(const std::string& s, LogLevel def) {
    std::string t = toLower(trim(s));
    if (t == "trace") return LogLevel::Trace;
    if (t == "debug") return LogLevel::Debug;
    if (t == "info") return LogLevel::Info;
    if (t == "warn" || t == "warning") return LogLevel::Warn;
    if (t == "error") return LogLevel::Error;
    if (t == "off" || t == "none") return LogLevel::Off;
    return def;
}

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::configure(LogLevel level, const std::string& filepath, uint64_t maxFileBytes,
                       int maxFiles, bool console, size_t memoryCapacity) {
    std::lock_guard<std::mutex> lk(mu_);
    level_.store(level, std::memory_order_relaxed);
    console_ = console;
    memoryCapacity_ = memoryCapacity ? memoryCapacity : 1000;
    maxFileBytes_ = maxFileBytes ? maxFileBytes : (16ull * 1024 * 1024);
    maxFiles_ = maxFiles > 0 ? maxFiles : 1;

    if (fileHandle_) {
        std::fclose(static_cast<FILE*>(fileHandle_));
        fileHandle_ = nullptr;
    }
    filepath_ = filepath;
    if (!filepath_.empty()) {
        ensureDirectoryExists(parentDirectoryOf(filepath_));
        FILE* f = fsutilOpenAppend(filepath_);
        if (f) {
            fileHandle_ = f;
            curFileBytes_ = fileSizeOf(filepath_);
        }
    }
    while (memory_.size() > memoryCapacity_) memory_.pop_front();
}

void Logger::setLevel(LogLevel level) { level_.store(level, std::memory_order_relaxed); }

void Logger::rotateLocked() {
    if (filepath_.empty()) return;
    if (fileHandle_) {
        std::fclose(static_cast<FILE*>(fileHandle_));
        fileHandle_ = nullptr;
    }
    for (int i = maxFiles_ - 1; i >= 1; --i) {
        std::string from = filepath_ + "." + std::to_string(i);
        std::string to = filepath_ + "." + std::to_string(i + 1);
        if (i + 1 > maxFiles_) {
            removeFileIfExists(from);
        } else if (pathExists(from)) {
            removeFileIfExists(to);
            renamePath(from, to);
        }
    }
    removeFileIfExists(filepath_ + "." + std::to_string(maxFiles_));
    renamePath(filepath_, filepath_ + ".1");
    FILE* f = fsutilOpenAppend(filepath_);
    if (f) {
        fileHandle_ = f;
        curFileBytes_ = 0;
    }
}

void Logger::writeFileLocked(const std::string& line) {
    if (!fileHandle_) return;
    FILE* f = static_cast<FILE*>(fileHandle_);
    std::fwrite(line.data(), 1, line.size(), f);
    std::fflush(f);
    curFileBytes_ += line.size();
    if (curFileBytes_ >= maxFileBytes_) rotateLocked();
}

void Logger::log(LogLevel lv, const std::string& tag, const std::string& message) {
    if (!enabled(lv)) return;

    LogRecord rec;
    rec.timeMillis = nowUnixMillis();
    rec.level = lv;
    rec.tag = tag;
    rec.message = message;

    std::string line;
    {
        std::lock_guard<std::mutex> lk(mu_);
        rec.seq = ++seq_;
        int idx = static_cast<int>(lv);
        if (idx >= 0 && idx < 5) counters_[idx]++;

        line.reserve(message.size() + 64);
        line += formatDateTimeMillis(rec.timeMillis);
        line += " [";
        line += logLevelName(lv);
        line += "] [";
        line += tag;
        line += "] ";
        line += message;
        line += "\n";

        memory_.push_back(rec);
        while (memory_.size() > memoryCapacity_) memory_.pop_front();
        writeFileLocked(line);
    }

    if (console_) {
        const char* color = "";
        const char* reset = "";
#if !defined(_WIN32)
        reset = "\033[0m";
        switch (lv) {
            case LogLevel::Trace: color = "\033[90m"; break;
            case LogLevel::Debug: color = "\033[36m"; break;
            case LogLevel::Info: color = "\033[32m"; break;
            case LogLevel::Warn: color = "\033[33m"; break;
            case LogLevel::Error: color = "\033[31m"; break;
            default: break;
        }
#endif
        std::fputs(color, stdout);
        std::fwrite(line.data(), 1, line.size() - 1, stdout);
        std::fputs(reset, stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }

    std::vector<std::function<void(const LogRecord&)>> snapshot;
    {
        std::lock_guard<std::mutex> lk(listenerMu_);
        snapshot.reserve(listeners_.size());
        for (auto& kv : listeners_) snapshot.push_back(kv.second);
    }
    for (auto& fn : snapshot) {
        if (fn) fn(rec);
    }
}

void Logger::logf(LogLevel lv, const std::string& tag, const char* fmt, ...) {
    if (!enabled(lv)) return;
    char stackBuf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(stackBuf, sizeof(stackBuf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (static_cast<size_t>(n) < sizeof(stackBuf)) {
        log(lv, tag, std::string(stackBuf, static_cast<size_t>(n)));
        return;
    }
    std::vector<char> heap(static_cast<size_t>(n) + 1);
    va_start(ap, fmt);
    std::vsnprintf(heap.data(), heap.size(), fmt, ap);
    va_end(ap);
    log(lv, tag, std::string(heap.data(), static_cast<size_t>(n)));
}

std::vector<LogRecord> Logger::recent(uint64_t afterSeq, size_t limit, LogLevel minLevel,
                                      const std::string& filter) const {
    std::vector<LogRecord> out;
    std::string needle = toLower(filter);
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& r : memory_) {
        if (r.seq <= afterSeq) continue;
        if (static_cast<int>(r.level) < static_cast<int>(minLevel)) continue;
        if (!needle.empty()) {
            if (toLower(r.message).find(needle) == std::string::npos &&
                toLower(r.tag).find(needle) == std::string::npos)
                continue;
        }
        out.push_back(r);
        if (out.size() >= limit) break;
    }
    return out;
}

uint64_t Logger::lastSeq() const {
    std::lock_guard<std::mutex> lk(mu_);
    return seq_;
}

void Logger::clearMemory() {
    std::lock_guard<std::mutex> lk(mu_);
    memory_.clear();
}

int Logger::addListener(std::function<void(const LogRecord&)> fn) {
    std::lock_guard<std::mutex> lk(listenerMu_);
    int id = nextListenerId_++;
    listeners_.emplace_back(id, std::move(fn));
    return id;
}

void Logger::removeListener(int id) {
    std::lock_guard<std::mutex> lk(listenerMu_);
    for (size_t i = 0; i < listeners_.size(); ++i) {
        if (listeners_[i].first == id) {
            listeners_.erase(listeners_.begin() + static_cast<long>(i));
            return;
        }
    }
}

void Logger::counters(uint64_t out[5]) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (int i = 0; i < 5; ++i) out[i] = counters_[i];
}

void logTrace(const std::string& tag, const std::string& msg) {
    Logger::instance().log(LogLevel::Trace, tag, msg);
}
void logDebug(const std::string& tag, const std::string& msg) {
    Logger::instance().log(LogLevel::Debug, tag, msg);
}
void logInfo(const std::string& tag, const std::string& msg) {
    Logger::instance().log(LogLevel::Info, tag, msg);
}
void logWarn(const std::string& tag, const std::string& msg) {
    Logger::instance().log(LogLevel::Warn, tag, msg);
}
void logError(const std::string& tag, const std::string& msg) {
    Logger::instance().log(LogLevel::Error, tag, msg);
}

}  // namespace ttd
