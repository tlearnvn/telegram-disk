// Vùng đệm tạm cho một mảnh dữ liệu trước khi đẩy lên Telegram.
// Ba chế độ (người quản trị chọn trong Cài đặt):
//   stream : không đệm, đẩy thẳng lên Telegram từng phần 512 KB (ít RAM nhất)
//   memory : giữ trọn mảnh trong RAM rồi đẩy lên
//   disk   : ghi ra tệp tạm rồi đẩy lên (dùng khi máy ít RAM)
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

#include "common/strutil.h"

namespace ttd {
namespace storage {

enum class BufferMode { Stream, Memory, Disk };

BufferMode parseBufferMode(const std::string& s);
const char* bufferModeName(BufferMode mode);

// Hàm nhận dữ liệu khi vùng đệm được xả.
using SinkFn = std::function<bool(const uint8_t* data, size_t len, std::string& error)>;

class ChunkBuffer {
public:
    ChunkBuffer(BufferMode mode, uint64_t capacity, std::string spoolPath, SinkFn sink);
    ~ChunkBuffer();

    ChunkBuffer(const ChunkBuffer&) = delete;
    ChunkBuffer& operator=(const ChunkBuffer&) = delete;

    // Ghi dữ liệu vào vùng đệm. Ở chế độ stream, dữ liệu đi thẳng tới sink.
    bool append(const uint8_t* data, size_t len, std::string& error);
    // Đẩy toàn bộ dữ liệu còn lại tới sink.
    bool flush(std::string& error);
    // Huỷ và dọn tệp tạm.
    void discard();

    uint64_t bytesBuffered() const { return buffered_; }
    uint64_t bytesTotal() const { return total_; }
    BufferMode mode() const { return mode_; }

private:
    bool flushMemory(std::string& error);
    bool flushDisk(std::string& error);

    BufferMode mode_;
    uint64_t capacity_;
    std::string spoolPath_;
    SinkFn sink_;

    Bytes memory_;
    FILE* file_ = nullptr;
    uint64_t buffered_ = 0;
    uint64_t total_ = 0;
    bool discarded_ = false;
};

}  // namespace storage
}  // namespace ttd
