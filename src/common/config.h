// Cấu hình ứng dụng. Đọc từ tệp JSON, có thể chỉnh trực tiếp trên giao diện quản trị.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "common/json.h"

namespace ttd {

struct ServerConfig {
    std::string bindAddress = "0.0.0.0";
    uint16_t port = 8088;
    int workerThreads = 16;
    int maxRequestBodyMb = 64;      // cho các yêu cầu JSON thông thường
    int idleTimeoutSeconds = 120;
    std::string publicUrl;          // dùng khi tạo liên kết chia sẻ
    bool enableWebdav = true;
    std::string webdavPrefix = "/webdav";
    bool trustProxyHeaders = false;
};

struct StorageConfig {
    // Kích thước mảnh dữ liệu đẩy lên Telegram.
    uint64_t chunkSize = 500ull * 1024 * 1024;   // 500 MB
    // Chế độ đệm: "stream" (ít RAM nhất) | "memory" | "disk"
    std::string bufferMode = "stream";
    // Giới hạn RAM cho toàn bộ vùng đệm tải lên.
    uint64_t memoryBudget = 1024ull * 1024 * 1024;
    // Thư mục chứa tệp tạm khi bufferMode = disk.
    std::string spoolDirectory = "data/spool";
    // Kích thước gói dữ liệu trình duyệt gửi lên mỗi lần.
    uint64_t browserChunkSize = 8ull * 1024 * 1024;
    // Số mảnh tải song song (mỗi mảnh dùng một tài khoản khác nhau).
    int parallelChunks = 2;
    // Bộ nhớ đệm khối tải xuống.
    uint64_t downloadCacheBytes = 256ull * 1024 * 1024;
    std::string downloadCacheDirectory = "data/cache";
    // Tự động dọn phiên tải lên bỏ dở sau bao nhiêu giây không hoạt động.
    int uploadIdleTimeoutSeconds = 1800;
    // Số ngày giữ tệp trong thùng rác (0 = giữ mãi).
    int trashRetentionDays = 30;
    // Khử trùng lặp: nếu tệp đã tồn tại thì liên kết lại thay vì tải lên bản mới.
    bool deduplicate = true;
};

struct TelegramConfig {
    int32_t apiId = 0;
    std::string apiHash;
    std::string deviceModel = "Tuan Telegram Disk";
    std::string systemVersion = "1.0";
    std::string appVersion = "1.0";
    std::string langCode = "vi";
    int layer = 158;
    bool testMode = false;
    bool obfuscated = false;
    int connectionsPerAccount = 1;
    int requestTimeoutSeconds = 90;
    // Siêu nhóm dùng làm nơi lưu trữ.
    int64_t channelId = 0;
    int64_t channelAccessHash = 0;
    std::string channelTitle;
    std::string channelUsername;
    // "telegram" hoặc "local" (chế độ thử nghiệm, lưu trên đĩa).
    std::string backend = "telegram";
    std::string localDirectory = "data/local-store";
    // Tệp schema TL bên ngoài (bỏ trống = dùng bản nhúng sẵn).
    std::string schemaFile;
    std::vector<std::string> extraRsaKeys;
};

struct LoggingConfig {
    std::string level = "info";
    bool console = true;
    std::string file = "logs/tuan-telegram-disk.log";
    uint64_t maxFileBytes = 32ull * 1024 * 1024;
    int maxFiles = 5;
    int memoryRecords = 5000;
    bool logRequests = true;
};

struct SecurityConfig {
    int sessionDays = 14;
    int passwordIterations = 120000;
    bool allowRegistration = false;
    // Cho phép truy cập liên kết chia sẻ mà không cần đăng nhập.
    bool publicShareLinks = true;
};

class Config {
public:
    static Config& instance();

    bool loadFromFile(const std::string& path, std::string& error);
    bool saveToFile(const std::string& path, std::string& error) const;
    // Áp dụng một khối JSON (dùng cho trang cài đặt).
    bool applyJson(const Json& json, std::string& error);
    Json toJson() const;

    const std::string& path() const { return path_; }
    void setPath(const std::string& p) { path_ = p; }
    // Thư mục dữ liệu gốc; mọi đường dẫn tương đối tính từ đây.
    const std::string& dataRoot() const { return dataRoot_; }
    void setDataRoot(const std::string& p) { dataRoot_ = p; }
    std::string resolvePath(const std::string& relative) const;

    ServerConfig server;
    StorageConfig storage;
    TelegramConfig telegram;
    LoggingConfig logging;
    SecurityConfig security;
    struct DbConfig {
        std::string kind = "sqlite";
        std::string sqlitePath = "data/tuan-telegram-disk.db";
        std::string mysqlHost = "127.0.0.1";
        uint16_t mysqlPort = 3306;
        std::string mysqlUser = "root";
        std::string mysqlPassword;
        std::string mysqlDatabase = "tuan_telegram_disk";
    } database;

    mutable std::mutex mutex;

private:
    Config() = default;
    std::string path_;
    std::string dataRoot_ = ".";
};

}  // namespace ttd
