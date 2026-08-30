#include "crypto/aes.h"

#include <cstring>

namespace ttd {
namespace crypto {

namespace {

// Bảng S-box và nghịch đảo được sinh một lần khi khởi động.
struct AesTables {
    uint8_t sbox[256];
    uint8_t rsbox[256];
    uint32_t te0[256], te1[256], te2[256], te3[256];
    uint32_t td0[256], td1[256], td2[256], td3[256];
    uint8_t td4[256];

    static uint8_t xtime(uint8_t x) {
        return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
    }
    static uint8_t mul(uint8_t a, uint8_t b) {
        uint8_t r = 0;
        while (b) {
            if (b & 1) r ^= a;
            a = xtime(a);
            b >>= 1;
        }
        return r;
    }

    AesTables() {
        // Sinh S-box từ nghịch đảo trong GF(2^8) + phép biến đổi affine.
        uint8_t p = 1, q = 1;
        sbox[0] = 0x63;
        do {
            p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
            q ^= static_cast<uint8_t>(q << 1);
            q ^= static_cast<uint8_t>(q << 2);
            q ^= static_cast<uint8_t>(q << 4);
            if (q & 0x80) q ^= 0x09;
            uint8_t x = static_cast<uint8_t>(
                q ^ static_cast<uint8_t>((q << 1) | (q >> 7)) ^
                static_cast<uint8_t>((q << 2) | (q >> 6)) ^
                static_cast<uint8_t>((q << 3) | (q >> 5)) ^
                static_cast<uint8_t>((q << 4) | (q >> 4)));
            sbox[p] = static_cast<uint8_t>(x ^ 0x63);
        } while (p != 1);
        for (int i = 0; i < 256; ++i) rsbox[sbox[i]] = static_cast<uint8_t>(i);

        for (int i = 0; i < 256; ++i) {
            uint8_t s = sbox[i];
            uint8_t s2 = mul(s, 2), s3 = mul(s, 3);
            te0[i] = (uint32_t(s2) << 24) | (uint32_t(s) << 16) | (uint32_t(s) << 8) | s3;
            te1[i] = (te0[i] >> 8) | (te0[i] << 24);
            te2[i] = (te1[i] >> 8) | (te1[i] << 24);
            te3[i] = (te2[i] >> 8) | (te2[i] << 24);

            uint8_t r = rsbox[i];
            td4[i] = r;
            td0[i] = (uint32_t(mul(r, 0x0e)) << 24) | (uint32_t(mul(r, 0x09)) << 16) |
                     (uint32_t(mul(r, 0x0d)) << 8) | mul(r, 0x0b);
            td1[i] = (td0[i] >> 8) | (td0[i] << 24);
            td2[i] = (td1[i] >> 8) | (td1[i] << 24);
            td3[i] = (td2[i] >> 8) | (td2[i] << 24);
        }
    }
};

const AesTables& tables() {
    static AesTables t;
    return t;
}

inline uint32_t load32be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline void store32be(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

const uint32_t kRcon[11] = {0x00000000, 0x01000000, 0x02000000, 0x04000000, 0x08000000, 0x10000000,
                            0x20000000, 0x40000000, 0x80000000, 0x1B000000, 0x36000000};

}  // namespace

bool Aes::setEncryptKey(const uint8_t* key, int keyBits) {
    const AesTables& T = tables();
    int nk;
    switch (keyBits) {
        case 128: nk = 4; rounds_ = 10; break;
        case 192: nk = 6; rounds_ = 12; break;
        case 256: nk = 8; rounds_ = 14; break;
        default: rounds_ = 0; return false;
    }
    int total = 4 * (rounds_ + 1);
    for (int i = 0; i < nk; ++i) rk_[i] = load32be(key + i * 4);
    for (int i = nk; i < total; ++i) {
        uint32_t temp = rk_[i - 1];
        if (i % nk == 0) {
            temp = (temp << 8) | (temp >> 24);
            temp = (uint32_t(T.sbox[(temp >> 24) & 0xff]) << 24) |
                   (uint32_t(T.sbox[(temp >> 16) & 0xff]) << 16) |
                   (uint32_t(T.sbox[(temp >> 8) & 0xff]) << 8) | uint32_t(T.sbox[temp & 0xff]);
            temp ^= kRcon[i / nk];
        } else if (nk > 6 && i % nk == 4) {
            temp = (uint32_t(T.sbox[(temp >> 24) & 0xff]) << 24) |
                   (uint32_t(T.sbox[(temp >> 16) & 0xff]) << 16) |
                   (uint32_t(T.sbox[(temp >> 8) & 0xff]) << 8) | uint32_t(T.sbox[temp & 0xff]);
        }
        rk_[i] = rk_[i - nk] ^ temp;
    }
    return true;
}

bool Aes::setDecryptKey(const uint8_t* key, int keyBits) {
    if (!setEncryptKey(key, keyBits)) return false;
    const AesTables& T = tables();
    // Đảo thứ tự khoá vòng.
    for (int i = 0, j = 4 * rounds_; i < j; i += 4, j -= 4) {
        for (int k = 0; k < 4; ++k) std::swap(rk_[i + k], rk_[j + k]);
    }
    // Áp dụng InvMixColumns cho các khoá vòng ở giữa.
    for (int i = 1; i < rounds_; ++i) {
        uint32_t* p = rk_ + i * 4;
        for (int k = 0; k < 4; ++k) {
            uint32_t v = p[k];
            p[k] = T.td0[T.sbox[(v >> 24) & 0xff]] ^ T.td1[T.sbox[(v >> 16) & 0xff]] ^
                   T.td2[T.sbox[(v >> 8) & 0xff]] ^ T.td3[T.sbox[v & 0xff]];
        }
    }
    return true;
}

void Aes::encryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    const AesTables& T = tables();
    uint32_t s0 = load32be(in) ^ rk_[0];
    uint32_t s1 = load32be(in + 4) ^ rk_[1];
    uint32_t s2 = load32be(in + 8) ^ rk_[2];
    uint32_t s3 = load32be(in + 12) ^ rk_[3];
    uint32_t t0, t1, t2, t3;
    const uint32_t* rk = rk_ + 4;
    for (int r = 1; r < rounds_; ++r) {
        t0 = T.te0[(s0 >> 24) & 0xff] ^ T.te1[(s1 >> 16) & 0xff] ^ T.te2[(s2 >> 8) & 0xff] ^
             T.te3[s3 & 0xff] ^ rk[0];
        t1 = T.te0[(s1 >> 24) & 0xff] ^ T.te1[(s2 >> 16) & 0xff] ^ T.te2[(s3 >> 8) & 0xff] ^
             T.te3[s0 & 0xff] ^ rk[1];
        t2 = T.te0[(s2 >> 24) & 0xff] ^ T.te1[(s3 >> 16) & 0xff] ^ T.te2[(s0 >> 8) & 0xff] ^
             T.te3[s1 & 0xff] ^ rk[2];
        t3 = T.te0[(s3 >> 24) & 0xff] ^ T.te1[(s0 >> 16) & 0xff] ^ T.te2[(s1 >> 8) & 0xff] ^
             T.te3[s2 & 0xff] ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
        rk += 4;
    }
    // Vòng cuối không có MixColumns.
    t0 = (uint32_t(T.sbox[(s0 >> 24) & 0xff]) << 24) | (uint32_t(T.sbox[(s1 >> 16) & 0xff]) << 16) |
         (uint32_t(T.sbox[(s2 >> 8) & 0xff]) << 8) | uint32_t(T.sbox[s3 & 0xff]);
    t1 = (uint32_t(T.sbox[(s1 >> 24) & 0xff]) << 24) | (uint32_t(T.sbox[(s2 >> 16) & 0xff]) << 16) |
         (uint32_t(T.sbox[(s3 >> 8) & 0xff]) << 8) | uint32_t(T.sbox[s0 & 0xff]);
    t2 = (uint32_t(T.sbox[(s2 >> 24) & 0xff]) << 24) | (uint32_t(T.sbox[(s3 >> 16) & 0xff]) << 16) |
         (uint32_t(T.sbox[(s0 >> 8) & 0xff]) << 8) | uint32_t(T.sbox[s1 & 0xff]);
    t3 = (uint32_t(T.sbox[(s3 >> 24) & 0xff]) << 24) | (uint32_t(T.sbox[(s0 >> 16) & 0xff]) << 16) |
         (uint32_t(T.sbox[(s1 >> 8) & 0xff]) << 8) | uint32_t(T.sbox[s2 & 0xff]);
    store32be(out, t0 ^ rk[0]);
    store32be(out + 4, t1 ^ rk[1]);
    store32be(out + 8, t2 ^ rk[2]);
    store32be(out + 12, t3 ^ rk[3]);
}

void Aes::decryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    const AesTables& T = tables();
    uint32_t s0 = load32be(in) ^ rk_[0];
    uint32_t s1 = load32be(in + 4) ^ rk_[1];
    uint32_t s2 = load32be(in + 8) ^ rk_[2];
    uint32_t s3 = load32be(in + 12) ^ rk_[3];
    uint32_t t0, t1, t2, t3;
    const uint32_t* rk = rk_ + 4;
    for (int r = 1; r < rounds_; ++r) {
        t0 = T.td0[(s0 >> 24) & 0xff] ^ T.td1[(s3 >> 16) & 0xff] ^ T.td2[(s2 >> 8) & 0xff] ^
             T.td3[s1 & 0xff] ^ rk[0];
        t1 = T.td0[(s1 >> 24) & 0xff] ^ T.td1[(s0 >> 16) & 0xff] ^ T.td2[(s3 >> 8) & 0xff] ^
             T.td3[s2 & 0xff] ^ rk[1];
        t2 = T.td0[(s2 >> 24) & 0xff] ^ T.td1[(s1 >> 16) & 0xff] ^ T.td2[(s0 >> 8) & 0xff] ^
             T.td3[s3 & 0xff] ^ rk[2];
        t3 = T.td0[(s3 >> 24) & 0xff] ^ T.td1[(s2 >> 16) & 0xff] ^ T.td2[(s1 >> 8) & 0xff] ^
             T.td3[s0 & 0xff] ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
        rk += 4;
    }
    t0 = (uint32_t(T.td4[(s0 >> 24) & 0xff]) << 24) | (uint32_t(T.td4[(s3 >> 16) & 0xff]) << 16) |
         (uint32_t(T.td4[(s2 >> 8) & 0xff]) << 8) | uint32_t(T.td4[s1 & 0xff]);
    t1 = (uint32_t(T.td4[(s1 >> 24) & 0xff]) << 24) | (uint32_t(T.td4[(s0 >> 16) & 0xff]) << 16) |
         (uint32_t(T.td4[(s3 >> 8) & 0xff]) << 8) | uint32_t(T.td4[s2 & 0xff]);
    t2 = (uint32_t(T.td4[(s2 >> 24) & 0xff]) << 24) | (uint32_t(T.td4[(s1 >> 16) & 0xff]) << 16) |
         (uint32_t(T.td4[(s0 >> 8) & 0xff]) << 8) | uint32_t(T.td4[s3 & 0xff]);
    t3 = (uint32_t(T.td4[(s3 >> 24) & 0xff]) << 24) | (uint32_t(T.td4[(s2 >> 16) & 0xff]) << 16) |
         (uint32_t(T.td4[(s1 >> 8) & 0xff]) << 8) | uint32_t(T.td4[s0 & 0xff]);
    store32be(out, t0 ^ rk[0]);
    store32be(out + 4, t1 ^ rk[1]);
    store32be(out + 8, t2 ^ rk[2]);
    store32be(out + 12, t3 ^ rk[3]);
}

// ---------------------------------------------------------------------------
//  IGE
// ---------------------------------------------------------------------------
bool aesIgeEncrypt(const Bytes& data, const Bytes& key, const Bytes& iv, Bytes& out) {
    if (data.size() % 16 != 0 || iv.size() != 32) return false;
    if (key.size() != 16 && key.size() != 24 && key.size() != 32) return false;
    Aes aes;
    if (!aes.setEncryptKey(key.data(), static_cast<int>(key.size() * 8))) return false;

    out.resize(data.size());
    uint8_t prevCipher[16], prevPlain[16], block[16], tmp[16];
    std::memcpy(prevCipher, iv.data(), 16);
    std::memcpy(prevPlain, iv.data() + 16, 16);

    for (size_t i = 0; i < data.size(); i += 16) {
        std::memcpy(block, data.data() + i, 16);
        for (int k = 0; k < 16; ++k) tmp[k] = static_cast<uint8_t>(block[k] ^ prevCipher[k]);
        aes.encryptBlock(tmp, tmp);
        for (int k = 0; k < 16; ++k) tmp[k] = static_cast<uint8_t>(tmp[k] ^ prevPlain[k]);
        std::memcpy(out.data() + i, tmp, 16);
        std::memcpy(prevCipher, tmp, 16);
        std::memcpy(prevPlain, block, 16);
    }
    return true;
}

bool aesIgeDecrypt(const Bytes& data, const Bytes& key, const Bytes& iv, Bytes& out) {
    if (data.size() % 16 != 0 || iv.size() != 32) return false;
    if (key.size() != 16 && key.size() != 24 && key.size() != 32) return false;
    Aes aes;
    if (!aes.setDecryptKey(key.data(), static_cast<int>(key.size() * 8))) return false;

    out.resize(data.size());
    uint8_t prevCipher[16], prevPlain[16], block[16], tmp[16];
    std::memcpy(prevCipher, iv.data(), 16);
    std::memcpy(prevPlain, iv.data() + 16, 16);

    for (size_t i = 0; i < data.size(); i += 16) {
        std::memcpy(block, data.data() + i, 16);
        for (int k = 0; k < 16; ++k) tmp[k] = static_cast<uint8_t>(block[k] ^ prevPlain[k]);
        aes.decryptBlock(tmp, tmp);
        for (int k = 0; k < 16; ++k) tmp[k] = static_cast<uint8_t>(tmp[k] ^ prevCipher[k]);
        std::memcpy(out.data() + i, tmp, 16);
        std::memcpy(prevCipher, block, 16);
        std::memcpy(prevPlain, tmp, 16);
    }
    return true;
}

// ---------------------------------------------------------------------------
//  CTR
// ---------------------------------------------------------------------------
bool AesCtr::init(const Bytes& key, const Bytes& iv) {
    if (iv.size() != 16) return false;
    if (key.size() != 16 && key.size() != 24 && key.size() != 32) return false;
    if (!aes_.setEncryptKey(key.data(), static_cast<int>(key.size() * 8))) return false;
    std::memcpy(counter_, iv.data(), 16);
    offset_ = 16;
    return true;
}

void AesCtr::process(uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (offset_ == 16) {
            aes_.encryptBlock(counter_, keystream_);
            offset_ = 0;
            // Tăng bộ đếm 128-bit theo thứ tự big-endian.
            for (int k = 15; k >= 0; --k) {
                if (++counter_[k] != 0) break;
            }
        }
        data[i] = static_cast<uint8_t>(data[i] ^ keystream_[offset_++]);
    }
}

// ---------------------------------------------------------------------------
//  CBC
// ---------------------------------------------------------------------------
bool aesCbcEncrypt(const Bytes& data, const Bytes& key, const Bytes& iv, Bytes& out) {
    if (iv.size() != 16) return false;
    if (key.size() != 16 && key.size() != 24 && key.size() != 32) return false;
    Aes aes;
    if (!aes.setEncryptKey(key.data(), static_cast<int>(key.size() * 8))) return false;

    // Đệm PKCS#7.
    size_t padLen = 16 - (data.size() % 16);
    Bytes padded = data;
    padded.insert(padded.end(), padLen, static_cast<uint8_t>(padLen));

    out.resize(padded.size());
    uint8_t prev[16], tmp[16];
    std::memcpy(prev, iv.data(), 16);
    for (size_t i = 0; i < padded.size(); i += 16) {
        for (int k = 0; k < 16; ++k) tmp[k] = static_cast<uint8_t>(padded[i + size_t(k)] ^ prev[k]);
        aes.encryptBlock(tmp, tmp);
        std::memcpy(out.data() + i, tmp, 16);
        std::memcpy(prev, tmp, 16);
    }
    return true;
}

bool aesCbcDecrypt(const Bytes& data, const Bytes& key, const Bytes& iv, Bytes& out) {
    if (iv.size() != 16 || data.empty() || data.size() % 16 != 0) return false;
    if (key.size() != 16 && key.size() != 24 && key.size() != 32) return false;
    Aes aes;
    if (!aes.setDecryptKey(key.data(), static_cast<int>(key.size() * 8))) return false;

    Bytes plain(data.size());
    uint8_t prev[16], tmp[16];
    std::memcpy(prev, iv.data(), 16);
    for (size_t i = 0; i < data.size(); i += 16) {
        aes.decryptBlock(data.data() + i, tmp);
        for (int k = 0; k < 16; ++k) tmp[k] = static_cast<uint8_t>(tmp[k] ^ prev[k]);
        std::memcpy(plain.data() + i, tmp, 16);
        std::memcpy(prev, data.data() + i, 16);
    }
    uint8_t padLen = plain.back();
    if (padLen == 0 || padLen > 16 || padLen > plain.size()) return false;
    for (size_t i = plain.size() - padLen; i < plain.size(); ++i)
        if (plain[i] != padLen) return false;
    plain.resize(plain.size() - padLen);
    out = std::move(plain);
    return true;
}

}  // namespace crypto
}  // namespace ttd
