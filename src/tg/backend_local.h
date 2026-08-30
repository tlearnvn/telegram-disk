// Nơi lưu trữ nội bộ (ghi ra thư mục trên đĩa) — dùng để chạy thử toàn bộ ứng dụng
// mà chưa cần cấu hình tài khoản Telegram. Mọi tính năng khác (cắt mảnh, phát trực
// tuyến theo dải byte, WebDAV, huỷ giữa chừng…) hoạt động y hệt chế độ Telegram.
#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "tg/storage_backend.h"

namespace ttd {
namespace tg {

class LocalBackend : public StorageBackend {
public:
    explicit LocalBackend(std::string directory);

    std::string name() const override { return "Nội bộ (thử nghiệm)"; }
    bool ready(std::string& why) const override;
    std::unique_ptr<ChunkWriter> beginChunk(uint64_t totalSize, const std::string& chunkName,
                                            std::string& error) override;
    bool readRange(const ChunkLocation& loc, uint64_t offset, uint32_t limit, Bytes& out,
                   std::string& error) override;
    bool removeChunks(const std::vector<ChunkLocation>& locations, std::string& error) override;
    BackendStats stats() const override;

    const std::string& directory() const { return directory_; }
    std::string pathFor(int64_t documentId) const;

private:
    friend class LocalChunkWriter;
    std::string directory_;
    std::atomic<uint64_t> bytesUploaded_{0};
    std::atomic<uint64_t> bytesDownloaded_{0};
};

}  // namespace tg
}  // namespace ttd
