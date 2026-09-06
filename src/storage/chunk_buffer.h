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
    // sink có thể để trống ở chế độ memory/disk: khi đó nơi nhận được chỉ định
    // lúc gọi flush(sink, error). Đó là điều kiện để đẩy song song — vùng đệm
    // giữ trọn mảnh xong mới có một luồng nền tự mở tài khoản riêng mà đẩy,
    // thay vì bị buộc vào đúng ChunkWriter đã dựng cùng lúc với nó.
    //
    // Chế độ stream thì BẮT BUỘC có sink ngay, vì nó không giữ gì cả — dữ liệu
    // đi thẳng ra ngoài lúc append(). Cũng vì thế stream không đẩy song song
    // được: không có gì để giao cho luồng nền.
    ChunkBuffer(BufferMode mode, uint64_t capacity, std::string spoolPath, SinkFn sink = nullptr);
    ~ChunkBuffer();

    ChunkBuffer(const ChunkBuffer&) = delete;
    ChunkBuffer& operator=(const ChunkBuffer&) = delete;

    // Ghi dữ liệu vào vùng đệm. Ở chế độ stream, dữ liệu đi thẳng tới sink.
    bool append(const uint8_t* data, size_t len, std::string& error);
    // Đẩy toàn bộ dữ liệu còn lại tới sink đã dựng cùng vùng đệm.
    bool flush(std::string& error);
    // Đẩy toàn bộ dữ liệu còn lại tới một nơi nhận chỉ định lúc gọi.
    bool flush(const SinkFn& sink, std::string& error);
    // Huỷ và dọn tệp tạm.
    void discard();

    // Đệm được cả mảnh trong bộ nhớ hoặc trên đĩa (tức là giao cho luồng nền
    // đẩy được) hay không.
    bool giuDuocCaManh() const { return mode_ != BufferMode::Stream; }

    uint64_t bytesBuffered() const { return buffered_; }
    uint64_t bytesTotal() const { return total_; }
    BufferMode mode() const { return mode_; }

private:
    bool flushMemory(const SinkFn& sink, std::string& error);
    bool flushDisk(const SinkFn& sink, std::string& error);

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
