// Giao diện chung cho nơi lưu trữ mảnh dữ liệu.
// Có hai cài đặt: qua Telegram (AccountPool) và lưu nội bộ để thử nghiệm (LocalBackend).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common/strutil.h"
#include "tg/tg_account.h"

namespace ttd {
namespace tg {

// Bộ ghi một mảnh dữ liệu theo luồng.
class ChunkWriter {
public:
    virtual ~ChunkWriter() = default;
    virtual bool write(const uint8_t* data, size_t len, std::string& error) = 0;
    virtual bool finish(ChunkLocation& out, std::string& error) = 0;
    virtual void abort() = 0;
    virtual uint64_t written() const = 0;
    // Tài khoản (hoặc nguồn) đang phục vụ mảnh này — hiển thị trên giao diện.
    virtual std::string sourceLabel() const = 0;
};

struct BackendStats {
    int totalAccounts = 0;
    int readyAccounts = 0;
    uint64_t bytesUploaded = 0;
    uint64_t bytesDownloaded = 0;
};

class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    virtual std::string name() const = 0;
    // Kiểm tra sẵn sàng; nếu không, `why` giải thích lý do bằng tiếng Việt.
    virtual bool ready(std::string& why) const = 0;

    // Mở một mảnh mới để ghi. totalSize = 0 nghĩa là chưa biết kích thước.
    virtual std::unique_ptr<ChunkWriter> beginChunk(uint64_t totalSize,
                                                    const std::string& chunkName,
                                                    std::string& error) = 0;

    // Đọc một đoạn của mảnh. offset chia hết cho 4096, limit tối đa 1 MB.
    virtual bool readRange(const ChunkLocation& loc, uint64_t offset, uint32_t limit,
                           Bytes& out, std::string& error) = 0;

    // Xoá các mảnh khỏi nơi lưu trữ.
    virtual bool removeChunks(const std::vector<ChunkLocation>& locations,
                              std::string& error) = 0;

    virtual BackendStats stats() const = 0;
};

}  // namespace tg
}  // namespace ttd
