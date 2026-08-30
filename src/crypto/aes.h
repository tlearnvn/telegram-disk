// AES tự cài đặt kèm các chế độ ECB / CBC / CTR / IGE.
// MTProto dùng AES-256-IGE cho gói tin và AES-256-CTR cho lớp truyền tải nguỵ trang.
#pragma once

#include <cstdint>

#include "common/strutil.h"

namespace ttd {
namespace crypto {

class Aes {
public:
    // keyBits: 128, 192 hoặc 256.
    Aes() = default;
    bool setEncryptKey(const uint8_t* key, int keyBits);
    bool setDecryptKey(const uint8_t* key, int keyBits);

    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const;
    void decryptBlock(const uint8_t in[16], uint8_t out[16]) const;

private:
    uint32_t rk_[60];
    int rounds_ = 0;
};

// Chế độ IGE (Infinite Garble Extension) — dữ liệu phải là bội số của 16 byte.
// iv gồm 32 byte: 16 byte đầu là iv cho khối mã, 16 byte sau là iv cho khối rõ.
bool aesIgeEncrypt(const Bytes& data, const Bytes& key, const Bytes& iv, Bytes& out);
bool aesIgeDecrypt(const Bytes& data, const Bytes& key, const Bytes& iv, Bytes& out);

// Chế độ CTR — dùng cho lớp truyền tải nguỵ trang (obfuscated transport).
class AesCtr {
public:
    bool init(const Bytes& key, const Bytes& iv);
    void process(uint8_t* data, size_t len);

private:
    Aes aes_;
    uint8_t counter_[16] = {0};
    uint8_t keystream_[16] = {0};
    size_t offset_ = 16;
};

// Chế độ CBC — dùng để mã hoá phiên đăng nhập lưu trên đĩa.
bool aesCbcEncrypt(const Bytes& data, const Bytes& key, const Bytes& iv, Bytes& out);
bool aesCbcDecrypt(const Bytes& data, const Bytes& key, const Bytes& iv, Bytes& out);

}  // namespace crypto
}  // namespace ttd
