#include "storage/storage_engine.h"

#include <algorithm>
#include <cstring>

#include "common/logging.h"
#include "common/strutil.h"

namespace ttd {
namespace storage {

namespace {
constexpr const char* kTag = "storage";
constexpr uint64_t kBlockSize = 1024 * 1024;  // khối 1 MB khớp giới hạn của Telegram
}  // namespace

StorageEngine::StorageEngine(db::Database& database, tg::StorageBackend& backend,
                             const Config& config)
    : db_(database), backend_(backend), config_(config),
      cache_(config.storage.downloadCacheBytes) {}

tg::ChunkLocation StorageEngine::toLocation(const db::ChunkEntry& c) {
    tg::ChunkLocation loc;
    loc.messageId = c.messageId;
    loc.documentId = c.documentId;
    loc.accessHash = c.accessHash;
    loc.fileReference = fromHex(c.fileReferenceHex);
    loc.dcId = c.dcId;
    loc.size = c.size;
    loc.accountId = c.accountId;
    return loc;
}

void StorageEngine::fromLocation(const tg::ChunkLocation& loc, db::ChunkEntry& c) {
    c.messageId = loc.messageId;
    c.documentId = loc.documentId;
    c.accessHash = loc.accessHash;
    c.fileReferenceHex = toHex(loc.fileReference);
    c.dcId = loc.dcId;
    c.accountId = loc.accountId;
    if (loc.size) c.size = loc.size;
}

bool StorageEngine::streamFileRange(const db::FileEntry& file, uint64_t offset, uint64_t length,
                                    const std::function<bool(const uint8_t*, size_t)>& sink,
                                    std::string& error) {
    if (file.isFolder) {
        error = "Không đọc được nội dung của thư mục";
        return false;
    }
    if (offset >= file.size || length == 0) return true;
    if (offset + length > file.size) length = file.size - offset;

    std::vector<db::ChunkEntry> chunks;
    if (!db_.listChunks(file.id, chunks, error)) return false;
    if (chunks.empty()) {
        error = "Tệp chưa có dữ liệu (danh sách mảnh rỗng)";
        return false;
    }

    uint64_t remaining = length;
    uint64_t pos = offset;

    while (remaining > 0) {
        // Tìm mảnh chứa vị trí `pos`.
        const db::ChunkEntry* chunk = nullptr;
        for (const auto& c : chunks) {
            if (pos >= c.offset && pos < c.offset + c.size) {
                chunk = &c;
                break;
            }
        }
        if (!chunk) {
            error = "Thiếu mảnh dữ liệu tại vị trí " + std::to_string(pos) +
                    " — tệp có thể bị hỏng";
            return false;
        }

        uint64_t inChunk = pos - chunk->offset;
        uint64_t blockStart = (inChunk / kBlockSize) * kBlockSize;
        uint64_t blockLen = std::min<uint64_t>(kBlockSize, chunk->size - blockStart);

        Bytes block;
        bool fromCache = cache_.get(chunk->documentId, blockStart, block);
        if (!fromCache) {
            tg::ChunkLocation loc = toLocation(*chunk);
            std::string readError;
            if (!backend_.readRange(loc, blockStart, static_cast<uint32_t>(blockLen), block,
                                    readError)) {
                error = "Không đọc được dữ liệu từ " + backend_.name() + ": " + readError;
                return false;
            }
            if (block.empty()) {
                error = "Máy chủ trả về khối rỗng tại vị trí " + std::to_string(pos);
                return false;
            }
            // Ghi lại tham chiếu mới nếu nơi lưu đã làm mới nó.
            if (loc.accessHash != chunk->accessHash ||
                toHex(loc.fileReference) != chunk->fileReferenceHex) {
                std::string updateError;
                db_.updateChunkReference(chunk->id, toHex(loc.fileReference), loc.accessHash,
                                         loc.dcId, updateError);
            }
            cache_.put(chunk->documentId, blockStart, block);
        }

        uint64_t inBlock = inChunk - blockStart;
        if (inBlock >= block.size()) {
            error = "Khối dữ liệu ngắn hơn dự kiến tại vị trí " + std::to_string(pos);
            return false;
        }
        uint64_t take = std::min<uint64_t>(block.size() - inBlock, remaining);
        if (!sink(block.data() + inBlock, static_cast<size_t>(take))) {
            // Người nhận yêu cầu dừng (ví dụ trình duyệt đóng kết nối).
            return true;
        }
        pos += take;
        remaining -= take;
    }
    return true;
}

size_t StorageEngine::readFileRange(const db::FileEntry& file, uint64_t offset, size_t length,
                                    Bytes& out, std::string& error) {
    out.clear();
    out.reserve(length);
    bool ok = streamFileRange(file, offset, length,
                              [&](const uint8_t* data, size_t len) {
                                  out.insert(out.end(), data, data + len);
                                  return true;
                              },
                              error);
    if (!ok) return 0;
    return out.size();
}

bool StorageEngine::purgeFileData(const db::FileEntry& file, std::string& error) {
    if (file.isFolder) return true;

    // Nếu còn tệp khác cùng nội dung (khử trùng lặp) thì chỉ xoá siêu dữ liệu.
    if (config_.storage.deduplicate && !file.sha256.empty()) {
        uint64_t count = 0;
        std::string countError;
        if (db_.countFilesWithHash(file.sha256, count, countError) && count > 1) {
            LOG_INFO(kTag,
                     "Giữ nguyên dữ liệu trên %s vì còn %llu tệp khác dùng chung nội dung",
                     backend_.name().c_str(), static_cast<unsigned long long>(count - 1));
            return db_.deleteChunks(file.id, error);
        }
    }

    std::vector<db::ChunkEntry> chunks;
    if (!db_.listChunks(file.id, chunks, error)) return false;
    if (chunks.empty()) return true;

    std::vector<tg::ChunkLocation> locations;
    locations.reserve(chunks.size());
    for (const auto& c : chunks) {
        locations.push_back(toLocation(c));
        cache_.invalidate(c.documentId);
    }

    std::string removeError;
    if (!backend_.removeChunks(locations, removeError)) {
        // Vẫn xoá siêu dữ liệu để người dùng không thấy tệp treo lại,
        // nhưng ghi cảnh báo để quản trị viên biết còn rác trên Telegram.
        LOG_WARN(kTag, "Không xoá được hết dữ liệu trên %s: %s", backend_.name().c_str(),
                 removeError.c_str());
    }
    return db_.deleteChunks(file.id, error);
}

EngineStats StorageEngine::stats() const {
    EngineStats s;
    db::StorageStats dbStats;
    std::string error;
    if (db_.stats(dbStats, error)) {
        s.totalBytes = dbStats.totalBytes;
        s.fileCount = dbStats.fileCount;
        s.folderCount = dbStats.folderCount;
        s.chunkCount = dbStats.chunkCount;
        s.trashedBytes = dbStats.trashedBytes;
        s.trashedCount = dbStats.trashedCount;
        s.physicalBytes = dbStats.physicalBytes;
        s.uniqueChunkCount = dbStats.uniqueChunkCount;
    }
    s.cacheUsed = cache_.used();
    s.cacheCapacity = cache_.capacity();
    s.cacheHits = cache_.hits();
    s.cacheMisses = cache_.misses();

    tg::BackendStats bs = backend_.stats();
    s.uploadedBytes = bs.bytesUploaded;
    s.downloadedBytes = bs.bytesDownloaded;
    s.readyAccounts = bs.readyAccounts;
    s.totalAccounts = bs.totalAccounts;
    s.backendName = backend_.name();
    std::string why;
    s.backendReady = backend_.ready(why);
    s.backendMessage = why;
    return s;
}

}  // namespace storage
}  // namespace ttd
