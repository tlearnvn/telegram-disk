// Các hàm băm tự cài đặt: MD5, SHA-1, SHA-256, SHA-512, HMAC, PBKDF2, CRC32.
// Cài đặt riêng để tệp thực thi không phụ thuộc OpenSSL — chạy độc lập trên mọi máy.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "common/strutil.h"

namespace ttd {
namespace crypto {

// ---------------------------------------------------------------------------
class Md5 {
public:
    Md5() { reset(); }
    void reset();
    void update(const uint8_t* data, size_t len);
    void update(const std::string& s) {
        update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    void finish(uint8_t out[16]);
    static Bytes hash(const uint8_t* data, size_t len);
    static Bytes hash(const Bytes& b) { return hash(b.data(), b.size()); }

private:
    void processBlock(const uint8_t* p);
    uint32_t state_[4];
    uint64_t bitCount_;
    uint8_t buffer_[64];
    size_t bufferLen_;
};

// ---------------------------------------------------------------------------
class Sha1 {
public:
    static constexpr size_t kDigestSize = 20;
    Sha1() { reset(); }
    void reset();
    void update(const uint8_t* data, size_t len);
    void update(const std::string& s) {
        update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    void update(const Bytes& b) { update(b.data(), b.size()); }
    void finish(uint8_t out[20]);
    static Bytes hash(const uint8_t* data, size_t len);
    static Bytes hash(const Bytes& b) { return hash(b.data(), b.size()); }
    static Bytes hash(const std::string& s) {
        return hash(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

private:
    void processBlock(const uint8_t* p);
    uint32_t state_[5];
    uint64_t bitCount_;
    uint8_t buffer_[64];
    size_t bufferLen_;
};

// ---------------------------------------------------------------------------
class Sha256 {
public:
    static constexpr size_t kDigestSize = 32;
    Sha256() { reset(); }
    void reset();
    void update(const uint8_t* data, size_t len);
    void update(const std::string& s) {
        update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    void update(const Bytes& b) { update(b.data(), b.size()); }
    void finish(uint8_t out[32]);
    static Bytes hash(const uint8_t* data, size_t len);
    static Bytes hash(const Bytes& b) { return hash(b.data(), b.size()); }
    static Bytes hash(const std::string& s) {
        return hash(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

private:
    void processBlock(const uint8_t* p);
    uint32_t state_[8];
    uint64_t bitCount_;
    uint8_t buffer_[64];
    size_t bufferLen_;
};

// ---------------------------------------------------------------------------
class Sha512 {
public:
    static constexpr size_t kDigestSize = 64;
    Sha512() { reset(); }
    void reset();
    void update(const uint8_t* data, size_t len);
    void update(const Bytes& b) { update(b.data(), b.size()); }
    void finish(uint8_t out[64]);
    static Bytes hash(const uint8_t* data, size_t len);
    static Bytes hash(const Bytes& b) { return hash(b.data(), b.size()); }

private:
    void processBlock(const uint8_t* p);
    uint64_t state_[8];
    uint64_t bitCountLow_;
    uint64_t bitCountHigh_;
    uint8_t buffer_[128];
    size_t bufferLen_;
};

// ---------------------------------------------------------------------------
Bytes hmacSha1(const Bytes& key, const Bytes& msg);
Bytes hmacSha256(const Bytes& key, const Bytes& msg);
Bytes hmacSha512(const Bytes& key, const Bytes& msg);

// PBKDF2-HMAC-SHA512 (dùng cho mật khẩu 2FA của Telegram).
Bytes pbkdf2HmacSha512(const Bytes& password, const Bytes& salt, int iterations, size_t outLen);
// PBKDF2-HMAC-SHA256 (dùng cho mật khẩu tài khoản web).
Bytes pbkdf2HmacSha256(const Bytes& password, const Bytes& salt, int iterations, size_t outLen);

// CRC32 (đa thức IEEE 802.3, dùng cho định danh hàm dựng TL và kiểm tra dữ liệu).
uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0);
uint32_t crc32(const std::string& s, uint32_t seed = 0);

// So sánh theo thời gian hằng số, chống tấn công đo thời gian.
bool constantTimeEquals(const Bytes& a, const Bytes& b);
bool constantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len);

}  // namespace crypto
}  // namespace ttd
