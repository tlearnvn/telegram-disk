#include "tg/backend_local.h"

#include <cstdio>

#include "common/fsutil.h"
#include "common/logging.h"
#include "common/strutil.h"
#include "crypto/random.h"

namespace ttd {
namespace tg {

namespace {
constexpr const char* kTag = "tg.local";
}

class LocalChunkWriter : public ChunkWriter {
public:
    LocalChunkWriter(LocalBackend& backend, int64_t documentId, std::string chunkName)
        : backend_(backend), documentId_(documentId), chunkName_(std::move(chunkName)) {
        path_ = backend_.pathFor(documentId_);
        ensureDirectoryExists(parentDirectoryOf(path_));
        file_ = fsutilOpen(path_ + ".part", "wb");
    }

    ~LocalChunkWriter() override { closeFile(); }

    bool write(const uint8_t* data, size_t len, std::string& error) override {
        if (!file_) {
            error = "Không mở được tệp mảnh dữ liệu để ghi";
            return false;
        }
        if (len == 0) return true;
        if (std::fwrite(data, 1, len, static_cast<FILE*>(file_)) != len) {
            error = "Ghi dữ liệu ra đĩa thất bại (có thể đã hết dung lượng)";
            return false;
        }
        written_ += len;
        return true;
    }

    bool finish(ChunkLocation& out, std::string& error) override {
        closeFile();
        if (!renamePath(path_ + ".part", path_)) {
            error = "Không hoàn tất được tệp mảnh dữ liệu";
            return false;
        }
        out.documentId = documentId_;
        out.messageId = documentId_;
        out.accessHash = 0;
        out.dcId = 0;
        out.size = written_;
        out.accountId = 0;
        out.fileName = chunkName_;
        backend_.bytesUploaded_.fetch_add(written_);
        LOG_DEBUG(kTag, "Đã lưu mảnh %s (%s)", chunkName_.c_str(),
                  formatBytes(written_).c_str());
        return true;
    }

    void abort() override {
        closeFile();
        removeFileIfExists(path_ + ".part");
    }

    uint64_t written() const override { return written_; }
    std::string sourceLabel() const override { return "nội bộ"; }

private:
    void closeFile() {
        if (file_) {
            std::fclose(static_cast<FILE*>(file_));
            file_ = nullptr;
        }
    }

    LocalBackend& backend_;
    int64_t documentId_;
    std::string chunkName_;
    std::string path_;
    void* file_ = nullptr;
    uint64_t written_ = 0;
};

LocalBackend::LocalBackend(std::string directory) : directory_(std::move(directory)) {
    ensureDirectoryExists(directory_);
    LOG_INFO(kTag, "Chế độ lưu trữ nội bộ, thư mục: %s", directory_.c_str());
}

std::string LocalBackend::pathFor(int64_t documentId) const {
    uint64_t v = static_cast<uint64_t>(documentId);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    std::string name = buf;
    // Chia thành hai cấp thư mục để tránh một thư mục có quá nhiều tệp.
    return joinPath(joinPath(directory_, name.substr(0, 2)), name + ".chunk");
}

bool LocalBackend::ready(std::string& why) const {
    if (!isDirectory(directory_)) {
        why = "Không tạo được thư mục lưu trữ nội bộ: " + directory_;
        return false;
    }
    return true;
}

std::unique_ptr<ChunkWriter> LocalBackend::beginChunk(uint64_t totalSize,
                                                      const std::string& chunkName,
                                                      std::string& error) {
    (void)totalSize;
    int64_t id = crypto::randomInt64();
    if (id == 0) id = 1;
    auto writer = std::unique_ptr<ChunkWriter>(new LocalChunkWriter(*this, id, chunkName));
    if (writer->written() > 0) {
        error = "Trạng thái ghi không hợp lệ";
        return nullptr;
    }
    return writer;
}

bool LocalBackend::readRange(const ChunkLocation& loc, uint64_t offset, uint32_t limit,
                             Bytes& out, std::string& error) {
    std::string path = pathFor(loc.documentId);
    FILE* f = fsutilOpen(path, "rb");
    if (!f) {
        error = "Không tìm thấy mảnh dữ liệu nội bộ: " + path;
        return false;
    }
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) {
        std::fclose(f);
        error = "Không nhảy được tới vị trí " + std::to_string(offset);
        return false;
    }
    out.resize(limit);
    size_t got = std::fread(out.data(), 1, limit, f);
    std::fclose(f);
    out.resize(got);
    bytesDownloaded_.fetch_add(got);
    return true;
}

bool LocalBackend::removeChunks(const std::vector<ChunkLocation>& locations,
                                std::string& error) {
    for (const auto& loc : locations) {
        if (!removeFileIfExists(pathFor(loc.documentId)))
            error = "Không xoá được một số mảnh dữ liệu";
    }
    return error.empty();
}

BackendStats LocalBackend::stats() const {
    BackendStats s;
    s.totalAccounts = 1;
    s.readyAccounts = 1;
    s.bytesUploaded = bytesUploaded_.load();
    s.bytesDownloaded = bytesDownloaded_.load();
    return s;
}

}  // namespace tg
}  // namespace ttd
