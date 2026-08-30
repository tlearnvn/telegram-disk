#include "tg/mtproto_transport.h"

#include <algorithm>
#include <cstring>

#include "common/logging.h"
#include "crypto/random.h"

namespace ttd {
namespace tg {

namespace {
constexpr const char* kTag = "tg.net";
constexpr uint32_t kProtocolIntermediate = 0xeeeeeeeeu;
// Kích thước gói tối đa chấp nhận được — chặn máy chủ giả gửi dữ liệu khổng lồ.
constexpr uint32_t kMaxPacketSize = 32u * 1024 * 1024;

uint32_t readUInt32Le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void writeUInt32Le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

}  // namespace

MtprotoTransport::~MtprotoTransport() { close(); }

void MtprotoTransport::close() {
    if (socket_.valid()) {
        socket_.shutdownBoth();
        socket_.close();
    }
    obfuscated_ = false;
}

bool MtprotoTransport::connect(const std::string& ip, uint16_t port, int dcId, bool obfuscated,
                               int timeoutMs, std::string& error) {
    close();
    if (!socket_.connectIp(ip, port, timeoutMs, error)) return false;
    remote_ = ip + ":" + std::to_string(port);
    obfuscated_ = obfuscated;

    if (!obfuscated) {
        uint8_t header[4];
        writeUInt32Le(header, kProtocolIntermediate);
        if (!socket_.sendAll(header, 4, timeoutMs)) {
            error = "Không gửi được tiêu đề giao thức";
            close();
            return false;
        }
        LOG_DEBUG(kTag, "Đã kết nối %s (giao thức intermediate)", remote_.c_str());
        return true;
    }

    // Gói khởi tạo 64 byte của obfuscated2.
    Bytes init(64);
    for (int attempt = 0; attempt < 64; ++attempt) {
        crypto::fillRandom(init.data(), 64);
        if (init[0] == 0xef) continue;
        uint32_t first = readUInt32Le(init.data());
        if (first == 0x44414548u || first == 0x54534f50u || first == 0x20544547u ||
            first == 0x4954504fu || first == 0xeeeeeeeeu || first == 0xddddddddu ||
            first == 0x02010316u)
            continue;
        if (readUInt32Le(init.data() + 4) == 0) continue;
        break;
    }
    writeUInt32Le(init.data() + 56, kProtocolIntermediate);
    // 2 byte định danh DC (tuỳ chọn, giúp máy chủ định tuyến).
    int16_t dc = static_cast<int16_t>(dcId);
    init[60] = static_cast<uint8_t>(dc & 0xff);
    init[61] = static_cast<uint8_t>((dc >> 8) & 0xff);

    Bytes encKey(init.begin() + 8, init.begin() + 40);
    Bytes encIv(init.begin() + 40, init.begin() + 56);
    Bytes reversed(init.begin() + 8, init.begin() + 56);
    std::reverse(reversed.begin(), reversed.end());
    Bytes decKey(reversed.begin(), reversed.begin() + 32);
    Bytes decIv(reversed.begin() + 32, reversed.begin() + 48);

    if (!encryptor_.init(encKey, encIv) || !decryptor_.init(decKey, decIv)) {
        error = "Không khởi tạo được lớp nguỵ trang";
        close();
        return false;
    }

    Bytes encrypted = init;
    encryptor_.process(encrypted.data(), encrypted.size());
    // 56 byte đầu gửi nguyên bản, 8 byte cuối gửi bản đã mã hoá.
    Bytes packet(init.begin(), init.begin() + 56);
    packet.insert(packet.end(), encrypted.begin() + 56, encrypted.begin() + 64);

    if (!socket_.sendAll(packet, timeoutMs)) {
        error = "Không gửi được gói khởi tạo nguỵ trang";
        close();
        return false;
    }
    LOG_DEBUG(kTag, "Đã kết nối %s (giao thức intermediate nguỵ trang)", remote_.c_str());
    return true;
}

bool MtprotoTransport::sendRaw(const uint8_t* data, size_t len, int timeoutMs) {
    if (!obfuscated_) return socket_.sendAll(data, len, timeoutMs);
    Bytes tmp(data, data + len);
    encryptor_.process(tmp.data(), tmp.size());
    return socket_.sendAll(tmp, timeoutMs);
}

bool MtprotoTransport::recvRaw(uint8_t* data, size_t len, int timeoutMs) {
    if (!socket_.recvAll(data, len, timeoutMs)) return false;
    if (obfuscated_) decryptor_.process(data, len);
    return true;
}

bool MtprotoTransport::sendPacket(const Bytes& payload, int timeoutMs, std::string& error) {
    if (!socket_.valid()) {
        error = "Chưa kết nối";
        return false;
    }
    if (payload.size() % 4 != 0) {
        error = "Độ dài gói tin phải chia hết cho 4";
        return false;
    }
    Bytes buf;
    buf.resize(4 + payload.size());
    writeUInt32Le(buf.data(), static_cast<uint32_t>(payload.size()));
    std::memcpy(buf.data() + 4, payload.data(), payload.size());
    if (!sendRaw(buf.data(), buf.size(), timeoutMs)) {
        error = "Gửi dữ liệu thất bại";
        return false;
    }
    return true;
}

bool MtprotoTransport::recvPacket(Bytes& out, int timeoutMs, std::string& error) {
    if (!socket_.valid()) {
        error = "Chưa kết nối";
        return false;
    }
    uint8_t lenBuf[4];
    if (!recvRaw(lenBuf, 4, timeoutMs)) {
        error = "Không đọc được độ dài gói tin";
        return false;
    }
    uint32_t len = readUInt32Le(lenBuf);

    // Máy chủ báo lỗi cấp giao thức bằng một số 4 byte âm.
    if (len >= 0xfffffe00u) {
        int32_t code = static_cast<int32_t>(len);
        error = "Máy chủ trả mã lỗi truyền tải " + std::to_string(code);
        if (code == -404)
            error += " (auth_key không tồn tại trên DC này — cần tạo lại phiên)";
        else if (code == -429)
            error += " (bị giới hạn tần suất, hãy thử lại sau)";
        return false;
    }
    if (len == 0 || len > kMaxPacketSize) {
        error = "Độ dài gói tin không hợp lệ: " + std::to_string(len);
        return false;
    }

    out.resize(len);
    if (!recvRaw(out.data(), len, timeoutMs)) {
        error = "Không đọc đủ dữ liệu gói tin";
        return false;
    }
    return true;
}

}  // namespace tg
}  // namespace ttd
