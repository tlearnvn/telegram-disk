// Lớp truyền tải MTProto qua TCP.
// Hỗ trợ giao thức "intermediate" (0xeeeeeeee) ở cả dạng thường và dạng nguỵ trang
// (obfuscated2) — dạng nguỵ trang giúp vượt qua các mạng chặn theo đặc trưng gói tin.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/net.h"
#include "common/strutil.h"
#include "crypto/aes.h"

namespace ttd {
namespace tg {

class MtprotoTransport {
public:
    MtprotoTransport() = default;
    ~MtprotoTransport();

    // Kết nối tới địa chỉ IP của DC.
    bool connect(const std::string& ip, uint16_t port, int dcId, bool obfuscated, int timeoutMs,
                 std::string& error);
    void close();
    bool connected() const { return socket_.valid(); }

    // Gửi một gói tin MTProto (đã mã hoá hoặc dạng rõ).
    bool sendPacket(const Bytes& payload, int timeoutMs, std::string& error);
    // Nhận một gói tin. Trả về false nếu hết giờ hoặc lỗi (error được điền).
    bool recvPacket(Bytes& out, int timeoutMs, std::string& error);
    // Kiểm tra có dữ liệu chờ đọc không (không chặn lâu).
    bool hasDataReady(int timeoutMs) const { return socket_.waitReadable(timeoutMs); }

    std::string remoteAddress() const { return remote_; }

private:
    bool sendRaw(const uint8_t* data, size_t len, int timeoutMs);
    bool recvRaw(uint8_t* data, size_t len, int timeoutMs);

    net::TcpSocket socket_;
    std::string remote_;
    bool obfuscated_ = false;
    crypto::AesCtr encryptor_;
    crypto::AesCtr decryptor_;
};

}  // namespace tg
}  // namespace ttd
