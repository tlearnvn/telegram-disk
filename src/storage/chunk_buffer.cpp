#include "storage/chunk_buffer.h"

#include <algorithm>

#include "common/fsutil.h"
#include "common/logging.h"
#include "common/strutil.h"

namespace ttd {
namespace storage {

namespace {
constexpr const char* kTag = "storage.buf";
constexpr size_t kDiskReadBlock = 1024 * 1024;
}  // namespace

BufferMode parseBufferMode(const std::string& s) {
    std::string t = toLower(trim(s));
    if (t == "memory" || t == "ram") return BufferMode::Memory;
    if (t == "disk" || t == "dia") return BufferMode::Disk;
    return BufferMode::Stream;
}

const char* bufferModeName(BufferMode mode) {
    switch (mode) {
        case BufferMode::Memory: return "memory";
        case BufferMode::Disk: return "disk";
        default: return "stream";
    }
}

ChunkBuffer::ChunkBuffer(BufferMode mode, uint64_t capacity, std::string spoolPath, SinkFn sink)
    : mode_(mode), capacity_(capacity), spoolPath_(std::move(spoolPath)),
      sink_(std::move(sink)) {
    if (mode_ == BufferMode::Memory) {
        // Đặt trước một phần bộ nhớ để giảm số lần cấp phát lại.
        size_t reserve = static_cast<size_t>(std::min<uint64_t>(capacity_, 16ull * 1024 * 1024));
        memory_.reserve(reserve);
    } else if (mode_ == BufferMode::Disk) {
        ensureDirectoryExists(parentDirectoryOf(spoolPath_));
        file_ = fsutilOpen(spoolPath_, "w+b");
        if (!file_) {
            LOG_ERROR(kTag, "Không tạo được tệp tạm %s, chuyển sang chế độ stream",
                      spoolPath_.c_str());
            mode_ = BufferMode::Stream;
        }
    }
}

ChunkBuffer::~ChunkBuffer() { discard(); }

bool ChunkBuffer::append(const uint8_t* data, size_t len, std::string& error) {
    if (discarded_) {
        error = "Vùng đệm đã bị huỷ";
        return false;
    }
    if (len == 0) return true;
    total_ += len;

    switch (mode_) {
        case BufferMode::Stream:
            return sink_(data, len, error);

        case BufferMode::Memory:
            memory_.insert(memory_.end(), data, data + len);
            buffered_ += len;
            return true;

        case BufferMode::Disk: {
            if (!file_) {
                error = "Tệp tạm không khả dụng";
                return false;
            }
            if (std::fwrite(data, 1, len, file_) != len) {
                error = "Ghi tệp tạm thất bại (có thể đã hết dung lượng đĩa)";
                return false;
            }
            buffered_ += len;
            return true;
        }
    }
    return false;
}

bool ChunkBuffer::flushMemory(std::string& error) {
    if (memory_.empty()) return true;
    // Đẩy theo từng khối để không giữ hai bản sao lớn cùng lúc.
    size_t offset = 0;
    const size_t step = 1024 * 1024;
    while (offset < memory_.size()) {
        size_t take = std::min(step, memory_.size() - offset);
        if (!sink_(memory_.data() + offset, take, error)) return false;
        offset += take;
    }
    memory_.clear();
    memory_.shrink_to_fit();
    buffered_ = 0;
    return true;
}

bool ChunkBuffer::flushDisk(std::string& error) {
    if (!file_) return true;
    if (std::fflush(file_) != 0) {
        error = "Không ghi xong tệp tạm";
        return false;
    }
    if (std::fseek(file_, 0, SEEK_SET) != 0) {
        error = "Không đọc lại được tệp tạm";
        return false;
    }
    Bytes block(kDiskReadBlock);
    while (true) {
        size_t got = std::fread(block.data(), 1, block.size(), file_);
        if (got == 0) break;
        if (!sink_(block.data(), got, error)) return false;
    }
    buffered_ = 0;
    return true;
}

bool ChunkBuffer::flush(std::string& error) {
    if (discarded_) {
        error = "Vùng đệm đã bị huỷ";
        return false;
    }
    switch (mode_) {
        case BufferMode::Stream: return true;
        case BufferMode::Memory: return flushMemory(error);
        case BufferMode::Disk: return flushDisk(error);
    }
    return true;
}

void ChunkBuffer::discard() {
    if (discarded_) return;
    discarded_ = true;
    memory_.clear();
    memory_.shrink_to_fit();
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
        removeFileIfExists(spoolPath_);
    }
}

}  // namespace storage
}  // namespace ttd
