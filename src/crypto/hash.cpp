#include "crypto/hash.h"

namespace ttd {
namespace crypto {

namespace {

inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

inline uint32_t load32be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint32_t load32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t load64be(const uint8_t* p) {
    return (uint64_t(load32be(p)) << 32) | load32be(p + 4);
}
inline void store32be(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
inline void store32le(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}
inline void store64be(uint8_t* p, uint64_t v) {
    store32be(p, uint32_t(v >> 32));
    store32be(p + 4, uint32_t(v));
}

}  // namespace

// ===========================================================================
//  MD5
// ===========================================================================
void Md5::reset() {
    state_[0] = 0x67452301;
    state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe;
    state_[3] = 0x10325476;
    bitCount_ = 0;
    bufferLen_ = 0;
}

void Md5::processBlock(const uint8_t* p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t M[16];
    for (int i = 0; i < 16; ++i) M[i] = load32le(p + i * 4);
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + K[i] + M[g], S[i]);
        a = tmp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::update(const uint8_t* data, size_t len) {
    bitCount_ += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
        size_t take = 64 - bufferLen_;
        if (take > len) take = len;
        std::memcpy(buffer_ + bufferLen_, data, take);
        bufferLen_ += take;
        data += take;
        len -= take;
        if (bufferLen_ == 64) {
            processBlock(buffer_);
            bufferLen_ = 0;
        }
    }
}

void Md5::finish(uint8_t out[16]) {
    uint64_t bits = bitCount_;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (bufferLen_ != 56) update(&zero, 1);
    uint8_t lenBuf[8];
    for (int i = 0; i < 8; ++i) lenBuf[i] = uint8_t(bits >> (8 * i));
    std::memcpy(buffer_ + 56, lenBuf, 8);
    processBlock(buffer_);
    bufferLen_ = 0;
    for (int i = 0; i < 4; ++i) store32le(out + i * 4, state_[i]);
}

Bytes Md5::hash(const uint8_t* data, size_t len) {
    Md5 h;
    h.update(data, len);
    Bytes out(16);
    h.finish(out.data());
    return out;
}

// ===========================================================================
//  SHA-1
// ===========================================================================
void Sha1::reset() {
    state_[0] = 0x67452301;
    state_[1] = 0xEFCDAB89;
    state_[2] = 0x98BADCFE;
    state_[3] = 0x10325476;
    state_[4] = 0xC3D2E1F0;
    bitCount_ = 0;
    bufferLen_ = 0;
}

void Sha1::processBlock(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = load32be(p + i * 4);
    for (int i = 16; i < 80; ++i) w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t tmp = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = tmp;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

void Sha1::update(const uint8_t* data, size_t len) {
    bitCount_ += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
        size_t take = 64 - bufferLen_;
        if (take > len) take = len;
        std::memcpy(buffer_ + bufferLen_, data, take);
        bufferLen_ += take;
        data += take;
        len -= take;
        if (bufferLen_ == 64) {
            processBlock(buffer_);
            bufferLen_ = 0;
        }
    }
}

void Sha1::finish(uint8_t out[20]) {
    uint64_t bits = bitCount_;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (bufferLen_ != 56) update(&zero, 1);
    store64be(buffer_ + 56, bits);
    processBlock(buffer_);
    bufferLen_ = 0;
    for (int i = 0; i < 5; ++i) store32be(out + i * 4, state_[i]);
}

Bytes Sha1::hash(const uint8_t* data, size_t len) {
    Sha1 h;
    h.update(data, len);
    Bytes out(20);
    h.finish(out.data());
    return out;
}

// ===========================================================================
//  SHA-256
// ===========================================================================
void Sha256::reset() {
    static const uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(state_, kInit, sizeof(kInit));
    bitCount_ = 0;
    bufferLen_ = 0;
}

void Sha256::processBlock(const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = load32be(p + i * 4);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::update(const uint8_t* data, size_t len) {
    bitCount_ += static_cast<uint64_t>(len) * 8;
    while (len > 0) {
        size_t take = 64 - bufferLen_;
        if (take > len) take = len;
        std::memcpy(buffer_ + bufferLen_, data, take);
        bufferLen_ += take;
        data += take;
        len -= take;
        if (bufferLen_ == 64) {
            processBlock(buffer_);
            bufferLen_ = 0;
        }
    }
}

void Sha256::finish(uint8_t out[32]) {
    uint64_t bits = bitCount_;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (bufferLen_ != 56) update(&zero, 1);
    store64be(buffer_ + 56, bits);
    processBlock(buffer_);
    bufferLen_ = 0;
    for (int i = 0; i < 8; ++i) store32be(out + i * 4, state_[i]);
}

Bytes Sha256::hash(const uint8_t* data, size_t len) {
    Sha256 h;
    h.update(data, len);
    Bytes out(32);
    h.finish(out.data());
    return out;
}

// ===========================================================================
//  SHA-512
// ===========================================================================
void Sha512::reset() {
    static const uint64_t kInit[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
                                      0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
                                      0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                      0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    std::memcpy(state_, kInit, sizeof(kInit));
    bitCountLow_ = 0;
    bitCountHigh_ = 0;
    bufferLen_ = 0;
}

void Sha512::processBlock(const uint8_t* p) {
    static const uint64_t K[80] = {
        0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
        0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
        0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
        0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = load64be(p + i * 8);
    for (int i = 16; i < 80; ++i) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint64_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 80; ++i) {
        uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K[i] + w[i];
        uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha512::update(const uint8_t* data, size_t len) {
    uint64_t addBits = static_cast<uint64_t>(len) * 8;
    uint64_t before = bitCountLow_;
    bitCountLow_ += addBits;
    if (bitCountLow_ < before) ++bitCountHigh_;
    while (len > 0) {
        size_t take = 128 - bufferLen_;
        if (take > len) take = len;
        std::memcpy(buffer_ + bufferLen_, data, take);
        bufferLen_ += take;
        data += take;
        len -= take;
        if (bufferLen_ == 128) {
            processBlock(buffer_);
            bufferLen_ = 0;
        }
    }
}

void Sha512::finish(uint8_t out[64]) {
    uint64_t lo = bitCountLow_, hi = bitCountHigh_;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (bufferLen_ != 112) update(&zero, 1);
    store64be(buffer_ + 112, hi);
    store64be(buffer_ + 120, lo);
    processBlock(buffer_);
    bufferLen_ = 0;
    for (int i = 0; i < 8; ++i) {
        store32be(out + i * 8, uint32_t(state_[i] >> 32));
        store32be(out + i * 8 + 4, uint32_t(state_[i]));
    }
}

Bytes Sha512::hash(const uint8_t* data, size_t len) {
    Sha512 h;
    h.update(data, len);
    Bytes out(64);
    h.finish(out.data());
    return out;
}

// ===========================================================================
//  HMAC
// ===========================================================================
namespace {
template <typename H, size_t BlockSize, size_t DigestSize>
Bytes hmacGeneric(const Bytes& key, const Bytes& msg) {
    uint8_t k[BlockSize];
    std::memset(k, 0, BlockSize);
    if (key.size() > BlockSize) {
        H h;
        h.update(key.data(), key.size());
        uint8_t d[DigestSize];
        h.finish(d);
        std::memcpy(k, d, DigestSize);
    } else if (!key.empty()) {
        std::memcpy(k, key.data(), key.size());
    }
    uint8_t ipad[BlockSize], opad[BlockSize];
    for (size_t i = 0; i < BlockSize; ++i) {
        ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
    }
    uint8_t inner[DigestSize];
    {
        H h;
        h.update(ipad, BlockSize);
        h.update(msg.data(), msg.size());
        h.finish(inner);
    }
    Bytes out(DigestSize);
    {
        H h;
        h.update(opad, BlockSize);
        h.update(inner, DigestSize);
        h.finish(out.data());
    }
    return out;
}
}  // namespace

Bytes hmacSha1(const Bytes& key, const Bytes& msg) {
    return hmacGeneric<Sha1, 64, 20>(key, msg);
}
Bytes hmacSha256(const Bytes& key, const Bytes& msg) {
    return hmacGeneric<Sha256, 64, 32>(key, msg);
}
Bytes hmacSha512(const Bytes& key, const Bytes& msg) {
    return hmacGeneric<Sha512, 128, 64>(key, msg);
}

namespace {
template <size_t DigestSize>
Bytes pbkdf2Generic(Bytes (*prf)(const Bytes&, const Bytes&), const Bytes& password,
                    const Bytes& salt, int iterations, size_t outLen) {
    if (iterations < 1) iterations = 1;
    Bytes out;
    out.reserve(outLen);
    uint32_t blockIndex = 1;
    while (out.size() < outLen) {
        Bytes input = salt;
        input.push_back(static_cast<uint8_t>(blockIndex >> 24));
        input.push_back(static_cast<uint8_t>(blockIndex >> 16));
        input.push_back(static_cast<uint8_t>(blockIndex >> 8));
        input.push_back(static_cast<uint8_t>(blockIndex));
        Bytes u = prf(password, input);
        Bytes acc = u;
        for (int i = 1; i < iterations; ++i) {
            u = prf(password, u);
            for (size_t j = 0; j < acc.size(); ++j) acc[j] ^= u[j];
        }
        size_t take = outLen - out.size();
        if (take > acc.size()) take = acc.size();
        out.insert(out.end(), acc.begin(), acc.begin() + static_cast<long>(take));
        ++blockIndex;
    }
    return out;
}
}  // namespace

Bytes pbkdf2HmacSha512(const Bytes& password, const Bytes& salt, int iterations, size_t outLen) {
    return pbkdf2Generic<64>(&hmacSha512, password, salt, iterations, outLen);
}

Bytes pbkdf2HmacSha256(const Bytes& password, const Bytes& salt, int iterations, size_t outLen) {
    return pbkdf2Generic<32>(&hmacSha256, password, salt, iterations, outLen);
}

// ===========================================================================
//  CRC32
// ===========================================================================
namespace {
struct Crc32Table {
    uint32_t t[256];
    Crc32Table() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};
const Crc32Table& crcTable() {
    static Crc32Table tbl;
    return tbl;
}
}  // namespace

uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed) {
    const Crc32Table& tbl = crcTable();
    uint32_t c = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = tbl.t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t crc32(const std::string& s, uint32_t seed) {
    return crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size(), seed);
}

bool constantTimeEquals(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

bool constantTimeEquals(const Bytes& a, const Bytes& b) {
    if (a.size() != b.size()) return false;
    return constantTimeEquals(a.data(), b.data(), a.size());
}

}  // namespace crypto
}  // namespace ttd
