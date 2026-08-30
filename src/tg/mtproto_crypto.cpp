#include "tg/mtproto_crypto.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#include "common/logging.h"
#include "common/timeutil.h"
#include "crypto/aes.h"
#include "crypto/bigint.h"
#include "crypto/hash.h"
#include "crypto/random.h"

namespace ttd {
namespace tg {

using crypto::Sha1;
using crypto::Sha256;

namespace {

int64_t readInt64Le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return static_cast<int64_t>(v);
}

void writeInt64Le(Bytes& out, int64_t value) {
    uint64_t v = static_cast<uint64_t>(value);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

void writeInt32Le(Bytes& out, int32_t value) {
    uint32_t v = static_cast<uint32_t>(value);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}

Bytes slice(const Bytes& b, size_t offset, size_t len) {
    if (offset >= b.size()) return Bytes();
    size_t n = std::min(len, b.size() - offset);
    return Bytes(b.begin() + static_cast<long>(offset),
                 b.begin() + static_cast<long>(offset + n));
}

Bytes concat(std::initializer_list<Bytes> parts) {
    size_t total = 0;
    for (const auto& p : parts) total += p.size();
    Bytes out;
    out.reserve(total);
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

}  // namespace

void AuthKey::computeKeyId() {
    if (key.size() != 256) {
        keyId = 0;
        return;
    }
    Bytes digest = Sha1::hash(key);
    keyId = readInt64Le(digest.data() + 12);
}

Bytes AuthKey::auxHash() const {
    if (key.size() != 256) return Bytes();
    Bytes digest = Sha1::hash(key);
    return Bytes(digest.begin(), digest.begin() + 8);
}

void deriveAesKeyIv(const Bytes& authKey, const Bytes& msgKey, bool fromServer, Bytes& aesKey,
                    Bytes& aesIv) {
    size_t x = fromServer ? 8 : 0;
    Bytes a = Sha256::hash(concat({msgKey, slice(authKey, x, 36)}));
    Bytes b = Sha256::hash(concat({slice(authKey, 40 + x, 36), msgKey}));

    aesKey = concat({slice(a, 0, 8), slice(b, 8, 16), slice(a, 24, 8)});
    aesIv = concat({slice(b, 0, 8), slice(a, 8, 16), slice(b, 24, 8)});
}

Bytes encryptMessage(const AuthKey& authKey, int64_t sessionId, int64_t msgId, int32_t seqNo,
                     const Bytes& payload) {
    Bytes plain;
    plain.reserve(32 + payload.size() + 32);
    writeInt64Le(plain, authKey.serverSalt);
    writeInt64Le(plain, sessionId);
    writeInt64Le(plain, msgId);
    writeInt32Le(plain, seqNo);
    writeInt32Le(plain, static_cast<int32_t>(payload.size()));
    plain.insert(plain.end(), payload.begin(), payload.end());

    // Đệm 12..1024 byte ngẫu nhiên sao cho tổng chia hết cho 16.
    size_t padLen = 16 - (plain.size() % 16);
    if (padLen < 12) padLen += 16;
    padLen += 16 * (crypto::randomUInt32() % 8);  // thêm nhiễu cho khó phân tích lưu lượng
    Bytes pad = crypto::randomBytes(padLen);
    plain.insert(plain.end(), pad.begin(), pad.end());

    Bytes msgKeyLarge = Sha256::hash(concat({slice(authKey.key, 88, 32), plain}));
    Bytes msgKey = slice(msgKeyLarge, 8, 16);

    Bytes aesKey, aesIv;
    deriveAesKeyIv(authKey.key, msgKey, false, aesKey, aesIv);

    Bytes encrypted;
    if (!crypto::aesIgeEncrypt(plain, aesKey, aesIv, encrypted)) return Bytes();

    Bytes out;
    out.reserve(24 + encrypted.size());
    writeInt64Le(out, authKey.keyId);
    out.insert(out.end(), msgKey.begin(), msgKey.end());
    out.insert(out.end(), encrypted.begin(), encrypted.end());
    return out;
}

DecryptedMessage decryptMessage(const AuthKey& authKey, const Bytes& packet) {
    DecryptedMessage res;
    if (packet.size() < 24 + 16) {
        res.error = "Gói tin quá ngắn";
        return res;
    }
    int64_t keyId = readInt64Le(packet.data());
    if (keyId != authKey.keyId) {
        res.error = "Sai auth_key_id (máy chủ dùng khoá khác)";
        return res;
    }
    Bytes msgKey(packet.begin() + 8, packet.begin() + 24);
    Bytes encrypted(packet.begin() + 24, packet.end());
    if (encrypted.size() % 16 != 0) {
        res.error = "Độ dài dữ liệu mã hoá không chia hết cho 16";
        return res;
    }

    Bytes aesKey, aesIv;
    deriveAesKeyIv(authKey.key, msgKey, true, aesKey, aesIv);

    Bytes plain;
    if (!crypto::aesIgeDecrypt(encrypted, aesKey, aesIv, plain)) {
        res.error = "Giải mã AES-IGE thất bại";
        return res;
    }
    if (plain.size() < 32) {
        res.error = "Nội dung giải mã quá ngắn";
        return res;
    }

    // Kiểm tra toàn vẹn: msg_key phải khớp với SHA256 của phần rõ.
    Bytes expectLarge = Sha256::hash(concat({slice(authKey.key, 96, 32), plain}));
    Bytes expect = slice(expectLarge, 8, 16);
    if (!crypto::constantTimeEquals(expect, msgKey)) {
        res.error = "msg_key không khớp — gói tin bị sửa đổi hoặc sai khoá";
        return res;
    }

    res.salt = readInt64Le(plain.data());
    res.sessionId = readInt64Le(plain.data() + 8);
    res.msgId = readInt64Le(plain.data() + 16);
    uint32_t seq = 0;
    for (int i = 0; i < 4; ++i) seq |= static_cast<uint32_t>(plain[24 + static_cast<size_t>(i)])
                                      << (8 * i);
    res.seqNo = static_cast<int32_t>(seq);
    uint32_t len = 0;
    for (int i = 0; i < 4; ++i) len |= static_cast<uint32_t>(plain[28 + static_cast<size_t>(i)])
                                      << (8 * i);
    if (len > plain.size() - 32) {
        res.error = "Trường độ dài vượt quá dữ liệu";
        return res;
    }
    // Phần đệm phải nằm trong khoảng 12..1024 byte theo đặc tả.
    size_t padding = plain.size() - 32 - len;
    if (padding < 12 || padding > 1024) {
        res.error = "Độ dài phần đệm không hợp lệ";
        return res;
    }
    res.body.assign(plain.begin() + 32, plain.begin() + 32 + static_cast<long>(len));
    res.ok = true;
    return res;
}

bool rsaPadEncrypt(const Bytes& data, const crypto::RsaPublicKey& key, Bytes& out) {
    if (data.size() > 144) return false;
    if (!key.valid() || key.keySizeBytes() != 256) return false;

    const Bytes zeroIv(32, 0);
    for (int attempt = 0; attempt < 64; ++attempt) {
        // data_with_padding: dữ liệu + đệm ngẫu nhiên cho đủ 192 byte
        Bytes dataWithPadding = data;
        if (dataWithPadding.size() < 192) {
            Bytes pad = crypto::randomBytes(192 - dataWithPadding.size());
            dataWithPadding.insert(dataWithPadding.end(), pad.begin(), pad.end());
        }
        // data_pad_reversed
        Bytes reversed(dataWithPadding.rbegin(), dataWithPadding.rend());

        Bytes tempKey = crypto::randomBytes(32);
        Bytes dataWithHash = reversed;
        Bytes h = Sha256::hash(concat({tempKey, dataWithPadding}));
        dataWithHash.insert(dataWithHash.end(), h.begin(), h.end());  // 192 + 32 = 224

        Bytes aesEncrypted;
        if (!crypto::aesIgeEncrypt(dataWithHash, tempKey, zeroIv, aesEncrypted)) return false;

        Bytes hashAes = Sha256::hash(aesEncrypted);
        Bytes tempKeyXor(32);
        for (size_t i = 0; i < 32; ++i) tempKeyXor[i] = static_cast<uint8_t>(tempKey[i] ^ hashAes[i]);

        Bytes keyAesEncrypted = concat({tempKeyXor, aesEncrypted});  // 32 + 224 = 256
        crypto::BigInt v = crypto::BigInt::fromBytes(keyAesEncrypted);
        if (crypto::BigInt::compare(v, key.modulus()) >= 0) continue;  // thử lại

        return key.rawEncrypt(keyAesEncrypted, out);
    }
    return false;
}

void tmpAesKeyIv(const Bytes& newNonce, const Bytes& serverNonce, Bytes& aesKey, Bytes& aesIv) {
    Bytes ns = Sha1::hash(concat({newNonce, serverNonce}));
    Bytes sn = Sha1::hash(concat({serverNonce, newNonce}));
    Bytes nn = Sha1::hash(concat({newNonce, newNonce}));

    aesKey = concat({ns, slice(sn, 0, 12)});
    aesIv = concat({slice(sn, 12, 8), nn, slice(newNonce, 0, 4)});
}

Bytes newNonceHash(const Bytes& newNonce, uint8_t which, const Bytes& authKeyAuxHash) {
    Bytes buf = newNonce;
    buf.push_back(which);
    buf.insert(buf.end(), authKeyAuxHash.begin(), authKeyAuxHash.end());
    Bytes digest = Sha1::hash(buf);
    return Bytes(digest.begin() + 4, digest.end());  // 16 byte cuối
}

namespace {
uint64_t mulMod64(uint64_t a, uint64_t b, uint64_t m) {
#if defined(__SIZEOF_INT128__)
    return static_cast<uint64_t>((static_cast<__uint128_t>(a) * b) % m);
#else
    // Nhân modulo an toàn khi không có kiểu 128-bit.
    uint64_t result = 0;
    a %= m;
    while (b > 0) {
        if (b & 1) {
            result += a;
            if (result >= m) result -= m;
        }
        a <<= 1;
        if (a >= m) a -= m;
        b >>= 1;
    }
    return result;
#endif
}

uint64_t gcd64(uint64_t a, uint64_t b) {
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}
}  // namespace

bool factorizePq(uint64_t pq, uint64_t& p, uint64_t& q) {
    if (pq == 0) return false;
    if ((pq & 1) == 0) {
        p = 2;
        q = pq / 2;
        if (p > q) std::swap(p, q);
        return true;
    }
    // Thuật toán Pollard-Brent — pq của Telegram chỉ khoảng 63 bit nên rất nhanh.
    for (int attempt = 0; attempt < 40; ++attempt) {
        uint64_t c = 1 + crypto::randomBelow(pq - 1);
        uint64_t x = crypto::randomBelow(pq);
        uint64_t y = x, d = 1, ys = y;
        uint64_t r = 1, m = 128;
        uint64_t qAcc = 1;
        while (d == 1) {
            x = y;
            for (uint64_t i = 0; i < r; ++i) y = (mulMod64(y, y, pq) + c) % pq;
            uint64_t k = 0;
            while (k < r && d == 1) {
                ys = y;
                uint64_t lim = std::min(m, r - k);
                for (uint64_t i = 0; i < lim; ++i) {
                    y = (mulMod64(y, y, pq) + c) % pq;
                    uint64_t diff = x > y ? x - y : y - x;
                    if (diff == 0) diff = 1;
                    qAcc = mulMod64(qAcc, diff, pq);
                }
                d = gcd64(qAcc, pq);
                k += lim;
            }
            r <<= 1;
            if (r > (1ULL << 40)) break;
        }
        if (d == pq) {
            d = 1;
            do {
                ys = (mulMod64(ys, ys, pq) + c) % pq;
                uint64_t diff = x > ys ? x - ys : ys - x;
                if (diff == 0) diff = 1;
                d = gcd64(diff, pq);
            } while (d == 1);
        }
        if (d != 1 && d != pq) {
            p = d;
            q = pq / d;
            if (p > q) std::swap(p, q);
            return true;
        }
    }
    return false;
}

int64_t MsgIdGenerator::next(int64_t timeOffsetSeconds) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);

    int64_t nowMs = nowUnixMillis() + timeOffsetSeconds * 1000;
    int64_t seconds = nowMs / 1000;
    int64_t millis = nowMs % 1000;
    // 32 bit cao là thời gian UNIX, 32 bit thấp là phần lẻ (chia hết cho 4).
    int64_t id = (seconds << 32) | ((millis * 4194304LL / 1000LL) << 2);
    id &= ~3LL;
    if (id <= last_) id = last_ + 4;
    last_ = id;
    return id;
}

}  // namespace tg
}  // namespace ttd
