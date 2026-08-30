#include "crypto/rsa.h"

#include <cstring>

#include "crypto/hash.h"
#include "crypto/random.h"

namespace ttd {
namespace crypto {

namespace der {

namespace {

struct Reader {
    const uint8_t* p;
    const uint8_t* end;

    bool readTag(uint8_t& tag) {
        if (p >= end) return false;
        tag = *p++;
        return true;
    }
    bool readLength(size_t& len) {
        if (p >= end) return false;
        uint8_t b = *p++;
        if (b < 0x80) {
            len = b;
            return true;
        }
        size_t n = b & 0x7f;
        if (n == 0 || n > 4 || p + n > end) return false;
        len = 0;
        for (size_t i = 0; i < n; ++i) len = (len << 8) | *p++;
        return p + len <= end;
    }
    bool readTlv(uint8_t expectTag, const uint8_t*& body, size_t& len) {
        uint8_t tag;
        if (!readTag(tag) || tag != expectTag) return false;
        if (!readLength(len)) return false;
        body = p;
        p += len;
        return true;
    }
};

Bytes trimLeadingZeros(const uint8_t* p, size_t len) {
    size_t start = 0;
    while (start + 1 < len && p[start] == 0) ++start;
    return Bytes(p + start, p + len);
}

}  // namespace

bool parseRsaPublicKey(const Bytes& derBytes, Bytes& modulus, Bytes& exponent) {
    // Thử PKCS#1:  RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }
    {
        Reader r{derBytes.data(), derBytes.data() + derBytes.size()};
        const uint8_t* seq;
        size_t seqLen;
        if (r.readTlv(0x30, seq, seqLen)) {
            Reader inner{seq, seq + seqLen};
            const uint8_t* n;
            size_t nLen;
            const uint8_t* e;
            size_t eLen;
            if (inner.readTlv(0x02, n, nLen) && inner.readTlv(0x02, e, eLen)) {
                modulus = trimLeadingZeros(n, nLen);
                exponent = trimLeadingZeros(e, eLen);
                return !modulus.empty() && !exponent.empty();
            }
        }
    }
    // Thử SubjectPublicKeyInfo:
    //   SEQUENCE { SEQUENCE { OID, NULL }, BIT STRING { RSAPublicKey } }
    {
        Reader r{derBytes.data(), derBytes.data() + derBytes.size()};
        const uint8_t* seq;
        size_t seqLen;
        if (!r.readTlv(0x30, seq, seqLen)) return false;
        Reader outer{seq, seq + seqLen};
        const uint8_t* alg;
        size_t algLen;
        if (!outer.readTlv(0x30, alg, algLen)) return false;
        const uint8_t* bits;
        size_t bitsLen;
        if (!outer.readTlv(0x03, bits, bitsLen)) return false;
        if (bitsLen < 1) return false;
        // Byte đầu của BIT STRING là số bit không dùng (thường bằng 0).
        Bytes innerDer(bits + 1, bits + bitsLen);
        return parseRsaPublicKey(innerDer, modulus, exponent);
    }
}

}  // namespace der

bool RsaPublicKey::fromModulusExponent(const Bytes& modulus, const Bytes& exponent,
                                       RsaPublicKey& out) {
    out.n_ = BigInt::fromBytes(modulus);
    out.e_ = BigInt::fromBytes(exponent);
    return out.valid();
}

bool RsaPublicKey::fromPem(const std::string& pem, RsaPublicKey& out) {
    // Trích phần base64 nằm giữa các dòng BEGIN/END.
    size_t begin = pem.find("-----BEGIN");
    if (begin == std::string::npos) return false;
    size_t beginEnd = pem.find('\n', begin);
    if (beginEnd == std::string::npos) return false;
    size_t end = pem.find("-----END", beginEnd);
    if (end == std::string::npos) return false;
    std::string body = pem.substr(beginEnd + 1, end - beginEnd - 1);
    Bytes derBytes = base64Decode(body);
    if (derBytes.empty()) return false;

    Bytes modulus, exponent;
    if (!der::parseRsaPublicKey(derBytes, modulus, exponent)) return false;
    return fromModulusExponent(modulus, exponent, out);
}

int64_t RsaPublicKey::telegramFingerprint() const {
    // Vân tay = 8 byte cuối SHA1( TL_bytes(n) || TL_bytes(e) ).
    auto tlBytes = [](const Bytes& v, Bytes& out) {
        size_t len = v.size();
        if (len < 254) {
            out.push_back(static_cast<uint8_t>(len));
            out.insert(out.end(), v.begin(), v.end());
        } else {
            out.push_back(254);
            out.push_back(static_cast<uint8_t>(len & 0xff));
            out.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
            out.push_back(static_cast<uint8_t>((len >> 16) & 0xff));
            out.insert(out.end(), v.begin(), v.end());
        }
        while (out.size() % 4 != 0) out.push_back(0);
    };
    Bytes buf;
    tlBytes(n_.toBytes(), buf);
    tlBytes(e_.toBytes(), buf);
    Bytes digest = Sha1::hash(buf);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(digest[digest.size() - 8 + static_cast<size_t>(i)])
             << (8 * i);
    return static_cast<int64_t>(v);
}

bool RsaPublicKey::rawEncrypt(const Bytes& input, Bytes& out) const {
    if (!valid()) return false;
    BigInt m = BigInt::fromBytes(input);
    if (BigInt::compare(m, n_) >= 0) return false;
    BigInt c = BigInt::powMod(m, e_, n_);
    out = c.toBytes(keySizeBytes());
    return true;
}

bool RsaPublicKey::encryptPkcs1v15(const Bytes& message, Bytes& out) const {
    size_t k = keySizeBytes();
    if (message.size() + 11 > k) return false;
    Bytes em(k, 0);
    em[0] = 0x00;
    em[1] = 0x02;
    size_t psLen = k - message.size() - 3;
    for (size_t i = 0; i < psLen; ++i) {
        uint8_t b = 0;
        while (b == 0) fillRandom(&b, 1);  // đệm phải khác 0
        em[2 + i] = b;
    }
    em[2 + psLen] = 0x00;
    std::memcpy(em.data() + 3 + psLen, message.data(), message.size());
    return rawEncrypt(em, out);
}

namespace {
// Hàm sinh mặt nạ MGF1 dựa trên SHA-1.
Bytes mgf1Sha1(const Bytes& seed, size_t maskLen) {
    Bytes mask;
    mask.reserve(maskLen);
    uint32_t counter = 0;
    while (mask.size() < maskLen) {
        Bytes input = seed;
        input.push_back(static_cast<uint8_t>(counter >> 24));
        input.push_back(static_cast<uint8_t>(counter >> 16));
        input.push_back(static_cast<uint8_t>(counter >> 8));
        input.push_back(static_cast<uint8_t>(counter));
        Bytes h = Sha1::hash(input);
        size_t take = maskLen - mask.size();
        if (take > h.size()) take = h.size();
        mask.insert(mask.end(), h.begin(), h.begin() + static_cast<long>(take));
        ++counter;
    }
    return mask;
}
}  // namespace

bool RsaPublicKey::encryptOaepSha1(const Bytes& message, Bytes& out) const {
    const size_t hLen = 20;
    size_t k = keySizeBytes();
    if (message.size() + 2 * hLen + 2 > k) return false;

    Bytes lHash = Sha1::hash(Bytes{});  // nhãn rỗng
    size_t psLen = k - message.size() - 2 * hLen - 2;

    Bytes db;
    db.reserve(k - hLen - 1);
    db.insert(db.end(), lHash.begin(), lHash.end());
    db.insert(db.end(), psLen, 0x00);
    db.push_back(0x01);
    db.insert(db.end(), message.begin(), message.end());

    Bytes seed = randomBytes(hLen);
    Bytes dbMask = mgf1Sha1(seed, db.size());
    for (size_t i = 0; i < db.size(); ++i) db[i] ^= dbMask[i];

    Bytes seedMask = mgf1Sha1(db, hLen);
    Bytes maskedSeed = seed;
    for (size_t i = 0; i < hLen; ++i) maskedSeed[i] ^= seedMask[i];

    Bytes em;
    em.reserve(k);
    em.push_back(0x00);
    em.insert(em.end(), maskedSeed.begin(), maskedSeed.end());
    em.insert(em.end(), db.begin(), db.end());

    return rawEncrypt(em, out);
}

}  // namespace crypto
}  // namespace ttd
