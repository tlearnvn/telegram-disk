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
            // KHÔNG lặng lẽ tụt về stream. Ở đường đẩy song song, vùng đệm
            // không có sink (nơi nhận được chỉ định muộn, lúc flush), nên tụt về
            // stream nghĩa là append() báo một lỗi vô nghĩa còn flush() thì trả
            // về true — mảnh rỗng được ghi nhận như mảnh đầy. Để nguyên chế độ
            // disk và không có tệp; append() sẽ báo đúng nguyên nhân.
            LOG_ERROR(kTag, "Không tạo được tệp tạm %s — kiểm tra quyền ghi và"
                            " dung lượng trống của thư mục tệp tạm", spoolPath_.c_str());
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
            if (!sink_) {
                error = "Chế độ stream cần nơi nhận dữ liệu ngay từ đầu";
                return false;
            }
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

bool ChunkBuffer::flushMemory(const SinkFn& sink, std::string& error) {
    if (memory_.empty()) return true;
    // Đẩy theo từng khối để không giữ hai bản sao lớn cùng lúc.
    size_t offset = 0;
    const size_t step = 1024 * 1024;
    while (offset < memory_.size()) {
        size_t take = std::min(step, memory_.size() - offset);
        if (!sink(memory_.data() + offset, take, error)) return false;
        offset += take;
    }
    memory_.clear();
    memory_.shrink_to_fit();
    buffered_ = 0;
    return true;
}

bool ChunkBuffer::flushDisk(const SinkFn& sink, std::string& error) {
    if (!file_) {
        // Không mở được tệp tạm mà lại có byte đã đếm: báo hỏng, đừng trả về
        // true — trả true là ghi nhận một mảnh rỗng như mảnh đầy đủ.
        if (buffered_ > 0 || total_ > 0) {
            error = "Tệp tạm không khả dụng — không đẩy được mảnh";
            return false;
        }
        return true;
    }
    if (std::fflush(file_) != 0) {
        error = "Không ghi xong tệp tạm";
        return false;
    }
    if (std::fseek(file_, 0, SEEK_SET) != 0) {
        error = "Không đọc lại được tệp tạm";
        return false;
    }
    // Đọc lại ĐÚNG số byte đã ghi, không đọc tới hết tệp. Nếu một lượt ghi
    // trước đó chỉ vào được một nửa rồi máy khách gửi lại, tệp tạm có thể dài
    // hơn buffered_ — đọc tới EOF là ghép thừa byte vào mảnh, mà mảnh vẫn được
    // ghi nhận đúng cỡ và đúng băm, nên không chỗ nào báo sai.
    Bytes block(kDiskReadBlock);
    uint64_t conLai = buffered_;
    while (conLai > 0) {
        size_t muon = static_cast<size_t>(std::min<uint64_t>(block.size(), conLai));
        size_t got = std::fread(block.data(), 1, muon, file_);
        if (got == 0) {
            // Hết tệp sớm hơn số byte đã đếm, hoặc lỗi đọc — cả hai đều là hỏng
            // thật. Trả về true ở đây là lặng lẽ giao một mảnh thiếu byte.
            error = std::ferror(file_) ? "Lỗi khi đọc lại tệp tạm"
                                       : "Tệp tạm ngắn hơn số byte đã ghi";
            return false;
        }
        if (!sink(block.data(), got, error)) return false;
        conLai -= got;
    }
    if (std::ferror(file_)) {
        error = "Lỗi khi đọc lại tệp tạm";
        return false;
    }
    buffered_ = 0;
    return true;
}

bool ChunkBuffer::flush(std::string& error) { return flush(sink_, error); }

bool ChunkBuffer::flush(const SinkFn& sink, std::string& error) {
    if (discarded_) {
        error = "Vùng đệm đã bị huỷ";
        return false;
    }
    switch (mode_) {
        case BufferMode::Stream:
            // Không giữ gì cả — dữ liệu đã đi thẳng ra ngoài từ lúc append().
            return true;
        case BufferMode::Memory:
        case BufferMode::Disk:
            if (!sink) {
                error = "Vùng đệm chưa có nơi nhận dữ liệu";
                return false;
            }
            return mode_ == BufferMode::Memory ? flushMemory(sink, error)
                                               : flushDisk(sink, error);
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
