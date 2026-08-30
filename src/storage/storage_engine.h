// Bộ máy lưu trữ: nối cơ sở dữ liệu với nơi lưu (Telegram hoặc nội bộ).
// Chịu trách nhiệm cắt mảnh, ghi siêu dữ liệu, đọc theo dải byte và xoá dữ liệu.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "db/database.h"
#include "storage/block_cache.h"
#include "tg/storage_backend.h"

namespace ttd {
namespace storage {

struct EngineStats {
    uint64_t totalBytes = 0;
    uint64_t fileCount = 0;
    uint64_t folderCount = 0;
    uint64_t chunkCount = 0;
    uint64_t trashedBytes = 0;
    uint64_t trashedCount = 0;
    uint64_t physicalBytes = 0;
    uint64_t uniqueChunkCount = 0;
    uint64_t cacheUsed = 0;
    uint64_t cacheCapacity = 0;
    uint64_t cacheHits = 0;
    uint64_t cacheMisses = 0;
    uint64_t uploadedBytes = 0;
    uint64_t downloadedBytes = 0;
    int readyAccounts = 0;
    int totalAccounts = 0;
    std::string backendName;
    bool backendReady = false;
    std::string backendMessage;
};

class StorageEngine {
public:
    StorageEngine(db::Database& database, tg::StorageBackend& backend, const Config& config);

    db::Database& db() { return db_; }
    tg::StorageBackend& backend() { return backend_; }
    BlockCache& cache() { return cache_; }

    // Đọc một đoạn byte bất kỳ của tệp (dùng cho tải xuống và WebDAV Range).
    // Trả về số byte thực đọc; 0 nghĩa là hết tệp hoặc lỗi (error được điền).
    size_t readFileRange(const db::FileEntry& file, uint64_t offset, size_t length, Bytes& out,
                         std::string& error);

    // Đọc theo luồng, gọi `sink` cho từng khối. Dừng khi sink trả về false.
    bool streamFileRange(const db::FileEntry& file, uint64_t offset, uint64_t length,
                         const std::function<bool(const uint8_t*, size_t)>& sink,
                         std::string& error);

    // Xoá toàn bộ mảnh dữ liệu của một tệp khỏi nơi lưu và khỏi cơ sở dữ liệu.
    bool purgeFileData(const db::FileEntry& file, std::string& error);

    EngineStats stats() const;

    // Chuyển ChunkEntry (cơ sở dữ liệu) thành ChunkLocation (nơi lưu).
    static tg::ChunkLocation toLocation(const db::ChunkEntry& c);
    static void fromLocation(const tg::ChunkLocation& loc, db::ChunkEntry& c);

private:
    db::Database& db_;
    tg::StorageBackend& backend_;
    const Config& config_;
    BlockCache cache_;
    mutable std::mutex mu_;
};

}  // namespace storage
}  // namespace ttd
