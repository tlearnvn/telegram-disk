#include "crypto/srp.h"

#include <mutex>
#include <set>

#include "crypto/bigint.h"
#include "crypto/hash.h"
#include "crypto/random.h"

namespace ttd {
namespace crypto {

namespace {

Bytes H(const Bytes& data) { return Sha256::hash(data); }

Bytes concat(std::initializer_list<const Bytes*> parts) {
    size_t total = 0;
    for (const Bytes* p : parts) total += p->size();
    Bytes out;
    out.reserve(total);
    for (const Bytes* p : parts) out.insert(out.end(), p->begin(), p->end());
    return out;
}

// SH(data, salt) = H(salt | data | salt)
Bytes SH(const Bytes& data, const Bytes& salt) {
    Bytes buf = concat({&salt, &data, &salt});
    return H(buf);
}

Bytes pad256(const BigInt& v) { return v.toBytes(256); }

}  // namespace

Bytes srpPasswordHash(const std::string& password, const Bytes& salt1, const Bytes& salt2) {
    Bytes pw(password.begin(), password.end());
    // PH1 = SH(SH(password, salt1), salt2)
    Bytes ph1 = SH(SH(pw, salt1), salt2);
    // PH2 = SH(PBKDF2-HMAC-SHA512(PH1, salt1, 100000, 64), salt2)
    Bytes dk = pbkdf2HmacSha512(ph1, salt1, 100000, 64);
    return SH(dk, salt2);
}

bool srpCheckPrimeAndGood(const Bytes& p, int g) {
    if (p.size() != 256) return false;
    if ((p[0] & 0x80) == 0) return false;  // phải đủ 2048 bit

    // Kiểm tra Miller-Rabin khá tốn thời gian nên chỉ làm một lần cho mỗi p.
    static std::mutex mu;
    static std::set<std::string> knownGood;
    std::string key = toHex(p);
    {
        std::lock_guard<std::mutex> lk(mu);
        if (knownGood.count(key)) {
            // đã kiểm tra p rồi, chỉ cần kiểm tra g bên dưới
        } else {
            BigInt P = BigInt::fromBytes(p);
            if (!P.isProbablePrime(24)) return false;
            BigInt half = BigInt::sub(P, BigInt(1)).shiftRight(1);
            if (!half.isProbablePrime(24)) return false;
            knownGood.insert(key);
        }
    }

    BigInt P = BigInt::fromBytes(p);
    auto modSmall = [&](uint32_t m) -> uint32_t {
        BigInt r = BigInt::mod(P, BigInt(m));
        return r.words().empty() ? 0u : r.words()[0];
    };
    switch (g) {
        case 2: return modSmall(8) == 7;
        case 3: return modSmall(3) == 2;
        case 4: return true;
        case 5: {
            uint32_t r = modSmall(5);
            return r == 1 || r == 4;
        }
        case 6: {
            uint32_t r = modSmall(24);
            return r == 19 || r == 23;
        }
        case 7: {
            uint32_t r = modSmall(7);
            return r == 3 || r == 5 || r == 6;
        }
        default: return false;
    }
}

SrpResult srpCompute(const SrpAlgo& algo, const Bytes& srpB, const std::string& password) {
    SrpResult res;
    if (!algo.valid()) {
        res.error = "Tham số SRP từ máy chủ không hợp lệ";
        return res;
    }
    if (srpB.empty() || srpB.size() > 256) {
        res.error = "Giá trị B từ máy chủ không hợp lệ";
        return res;
    }
    if (!srpCheckPrimeAndGood(algo.p, algo.g)) {
        res.error = "Máy chủ trả về tham số p/g không an toàn — từ chối đăng nhập";
        return res;
    }

    BigInt p = BigInt::fromBytes(algo.p);
    BigInt g(static_cast<uint64_t>(algo.g));
    BigInt gB = BigInt::fromBytes(srpB);

    BigInt one(1);
    BigInt pMinus1 = BigInt::sub(p, one);
    if (BigInt::compare(gB, one) <= 0 || BigInt::compare(gB, pMinus1) >= 0) {
        res.error = "Giá trị B nằm ngoài khoảng cho phép";
        return res;
    }

    Bytes gPadded = pad256(g);
    Bytes pBytes = algo.p;

    // x = PH2(password, salt1, salt2)
    Bytes xBytes = srpPasswordHash(password, algo.salt1, algo.salt2);
    BigInt x = BigInt::fromBytes(xBytes);

    // Sinh a ngẫu nhiên 2048 bit; lặp lại nếu g_a rơi ra ngoài khoảng hợp lệ.
    BigInt a, gA;
    for (int attempt = 0; attempt < 32; ++attempt) {
        a = BigInt::fromBytes(randomBytes(256));
        gA = BigInt::powMod(g, a, p);
        if (BigInt::compare(gA, one) > 0 && BigInt::compare(gA, pMinus1) < 0) break;
        gA = BigInt();
    }
    if (gA.isZero()) {
        res.error = "Không sinh được giá trị A hợp lệ";
        return res;
    }

    Bytes gABytes = pad256(gA);
    Bytes gBBytes = pad256(gB);

    // k = H(p | g)
    BigInt k = BigInt::fromBytes(H(concat({&pBytes, &gPadded})));
    // v = g^x mod p
    BigInt v = BigInt::powMod(g, x, p);
    // k_v = k*v mod p
    BigInt kv = BigInt::mulMod(k, v, p);
    // t = (g_b - k_v) mod p, đảm bảo dương
    BigInt t = BigInt::subMod(gB, kv, p);
    // u = H(g_a | g_b)
    BigInt u = BigInt::fromBytes(H(concat({&gABytes, &gBBytes})));
    if (u.isZero()) {
        res.error = "Giá trị u bằng 0 — dừng để tránh rủi ro";
        return res;
    }
    // s_a = t^(a + u*x) mod p
    BigInt exp = BigInt::add(a, BigInt::mul(u, x));
    BigInt sA = BigInt::powMod(t, exp, p);
    Bytes kA = H(pad256(sA));

    // M1 = H( H(p) xor H(g) | H(salt1) | H(salt2) | g_a | g_b | k_a )
    Bytes hp = H(pBytes);
    Bytes hg = H(gPadded);
    Bytes hx(hp.size());
    for (size_t i = 0; i < hp.size(); ++i) hx[i] = static_cast<uint8_t>(hp[i] ^ hg[i]);
    Bytes hs1 = H(algo.salt1);
    Bytes hs2 = H(algo.salt2);
    Bytes m1Input = concat({&hx, &hs1, &hs2, &gABytes, &gBBytes, &kA});

    res.A = gABytes;
    res.M1 = H(m1Input);
    res.ok = true;
    return res;
}

}  // namespace crypto
}  // namespace ttd
