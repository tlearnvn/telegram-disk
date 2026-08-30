// Bộ mã hoá / giải mã TL dựa trên schema.
#pragma once

#include <cstdint>
#include <string>

#include "common/strutil.h"
#include "tg/tl_schema.h"
#include "tg/tl_value.h"

namespace ttd {
namespace tg {

// Bộ ghi nhị phân little-endian theo quy ước TL.
class TlWriter {
public:
    void writeInt(int32_t v);
    void writeUInt(uint32_t v);
    void writeLong(int64_t v);
    void writeDouble(double v);
    void writeRaw(const uint8_t* data, size_t len);
    void writeRaw(const Bytes& b) { writeRaw(b.data(), b.size()); }
    // Chuỗi/bytes theo TL: 1 hoặc 4 byte độ dài + dữ liệu + đệm về bội số 4.
    void writeBytes(const uint8_t* data, size_t len);
    void writeBytes(const Bytes& b) { writeBytes(b.data(), b.size()); }
    void writeString(const std::string& s);

    Bytes& buffer() { return buf_; }
    const Bytes& buffer() const { return buf_; }
    size_t size() const { return buf_.size(); }
    void reserve(size_t n) { buf_.reserve(n); }

private:
    Bytes buf_;
};

// Bộ đọc nhị phân.
class TlReader {
public:
    TlReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}
    explicit TlReader(const Bytes& b) : data_(b.data()), len_(b.size()) {}

    bool readInt(int32_t& out);
    bool readUInt(uint32_t& out);
    bool readLong(int64_t& out);
    bool readDouble(double& out);
    bool readRaw(size_t n, Bytes& out);
    bool readBytes(Bytes& out);
    bool skip(size_t n);

    size_t position() const { return pos_; }
    size_t remaining() const { return len_ > pos_ ? len_ - pos_ : 0; }
    bool atEnd() const { return pos_ >= len_; }
    const uint8_t* current() const { return data_ + pos_; }
    void seek(size_t p) { pos_ = p; }
    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }
    void fail(const std::string& msg) {
        if (ok_) {
            ok_ = false;
            error_ = msg;
        }
    }

private:
    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    bool ok_ = true;
    std::string error_;
};

class TlCodec {
public:
    explicit TlCodec(const TlSchema& schema) : schema_(schema) {}

    // Ghi một đối tượng (có ID hàm dựng ở đầu).
    bool serialize(const TlValue& value, TlWriter& out, std::string& error) const;
    // Đọc một đối tượng boxed (có ID hàm dựng ở đầu).
    bool deserialize(TlReader& in, TlValue& out, std::string& error) const;
    // Đọc một đối tượng đã biết ID (dùng cho kiểu trần).
    bool deserializeByCtor(TlReader& in, const TlConstructor& ctor, TlValue& out,
                           std::string& error) const;

    const TlSchema& schema() const { return schema_; }

private:
    bool writeArg(const TlArg& arg, const TlValue& v, TlWriter& out, std::string& error) const;
    bool writeTyped(const TlType& type, const TlValue& v, TlWriter& out,
                    std::string& error) const;
    bool readTyped(const TlType& type, TlReader& in, TlValue& out, std::string& error) const;

    const TlSchema& schema_;
};

// Tìm đối tượng đầu tiên có tên hàm dựng cho trước trong cây giá trị TL.
// Hữu ích khi cần lấy `document` từ một `Updates` phức tạp (kể cả khi cây
// chỉ được giải mã một phần).
const TlValue* findFirstObject(const TlValue& root, const std::string& ctorName,
                               int maxDepth = 24);
void collectObjects(const TlValue& root, const std::string& ctorName,
                    std::vector<const TlValue*>& out, int maxDepth = 24);

// Định danh hàm dựng dùng thường xuyên (tính sẵn để tra nhanh).
namespace ctor {
constexpr uint32_t kVector = 0x1cb5c415;
constexpr uint32_t kBoolTrue = 0x997275b5;
constexpr uint32_t kBoolFalse = 0xbc799737;
constexpr uint32_t kTrue = 0x3fedd339;
constexpr uint32_t kGzipPacked = 0x3072cfa1;
constexpr uint32_t kRpcResult = 0xf35c6d01;
constexpr uint32_t kRpcError = 0x2144ca19;
constexpr uint32_t kMsgContainer = 0x73f1f8dc;
constexpr uint32_t kMsgsAck = 0x62d6b459;
constexpr uint32_t kBadMsgNotification = 0xa7eff811;
constexpr uint32_t kBadServerSalt = 0xedab447b;
constexpr uint32_t kNewSessionCreated = 0x9ec20908;
constexpr uint32_t kPong = 0x347773c5;
constexpr uint32_t kMsgDetailedInfo = 0x276d3ec6;
constexpr uint32_t kMsgNewDetailedInfo = 0x809db6df;
constexpr uint32_t kMsgsStateReq = 0xda69fb52;
constexpr uint32_t kMsgResendReq = 0x7d861a08;
constexpr uint32_t kMsgsAllInfo = 0x8cc0d131;
constexpr uint32_t kFutureSalts = 0xae500895;
constexpr uint32_t kDestroySessionOk = 0xe22045fc;
constexpr uint32_t kDestroySessionNone = 0x62d350c9;
constexpr uint32_t kRpcAnswerUnknown = 0x5e2ad36e;
constexpr uint32_t kRpcAnswerDroppedRunning = 0xcd78e586;
constexpr uint32_t kRpcAnswerDropped = 0xa43ad8b7;
}  // namespace ctor

}  // namespace tg
}  // namespace ttd
