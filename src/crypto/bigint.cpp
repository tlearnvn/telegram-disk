#include "crypto/bigint.h"

#include <algorithm>
#include <cstring>

#include "crypto/random.h"

namespace ttd {
namespace crypto {

BigInt::BigInt(uint64_t v) {
    if (v == 0) return;
    words_.push_back(static_cast<uint32_t>(v & 0xffffffffu));
    if (v >> 32) words_.push_back(static_cast<uint32_t>(v >> 32));
}

void BigInt::trim() {
    while (!words_.empty() && words_.back() == 0) words_.pop_back();
}

BigInt BigInt::fromBytes(const uint8_t* data, size_t len) {
    BigInt r;
    // Bỏ số 0 ở đầu.
    size_t start = 0;
    while (start < len && data[start] == 0) ++start;
    size_t n = len - start;
    if (n == 0) return r;
    r.words_.resize((n + 3) / 4, 0);
    // Duyệt từ byte thấp nhất (cuối mảng) lên.
    for (size_t i = 0; i < n; ++i) {
        uint8_t byte = data[len - 1 - i];
        r.words_[i / 4] |= static_cast<uint32_t>(byte) << ((i % 4) * 8);
    }
    r.trim();
    return r;
}

BigInt BigInt::fromHex(const std::string& hex) {
    Bytes b = ttd::fromHex(hex);
    return fromBytes(b);
}

BigInt BigInt::fromDecimal(const std::string& dec) {
    BigInt r;
    BigInt ten(10);
    for (char c : dec) {
        if (c < '0' || c > '9') continue;
        r = mul(r, ten);
        r = add(r, BigInt(static_cast<uint64_t>(c - '0')));
    }
    return r;
}

Bytes BigInt::toBytes(size_t minLen) const {
    size_t n = byteLength();
    size_t total = std::max(n, minLen);
    Bytes out(total, 0);
    for (size_t i = 0; i < n; ++i) {
        uint32_t w = words_[i / 4];
        out[total - 1 - i] = static_cast<uint8_t>((w >> ((i % 4) * 8)) & 0xff);
    }
    return out;
}

std::string BigInt::toHex() const {
    Bytes b = toBytes();
    if (b.empty()) return "00";
    return ttd::toHex(b);
}

std::string BigInt::toDecimal() const {
    if (isZero()) return "0";
    BigInt cur = *this;
    BigInt billion(1000000000ull);
    std::vector<uint32_t> chunks;
    while (!cur.isZero()) {
        BigInt q, r;
        divmod(cur, billion, q, r);
        chunks.push_back(r.words_.empty() ? 0u : r.words_[0]);
        cur = q;
    }
    std::string out = std::to_string(chunks.back());
    for (int i = static_cast<int>(chunks.size()) - 2; i >= 0; --i) {
        std::string part = std::to_string(chunks[static_cast<size_t>(i)]);
        out += std::string(9 - part.size(), '0') + part;
    }
    return out;
}

size_t BigInt::bitLength() const {
    if (words_.empty()) return 0;
    uint32_t top = words_.back();
    size_t bits = 0;
    while (top) {
        ++bits;
        top >>= 1;
    }
    return (words_.size() - 1) * 32 + bits;
}

bool BigInt::testBit(size_t i) const {
    size_t w = i / 32;
    if (w >= words_.size()) return false;
    return (words_[w] >> (i % 32)) & 1;
}

int BigInt::compare(const BigInt& a, const BigInt& b) {
    if (a.words_.size() != b.words_.size())
        return a.words_.size() < b.words_.size() ? -1 : 1;
    for (size_t i = a.words_.size(); i-- > 0;) {
        if (a.words_[i] != b.words_[i]) return a.words_[i] < b.words_[i] ? -1 : 1;
    }
    return 0;
}

BigInt BigInt::add(const BigInt& a, const BigInt& b) {
    BigInt r;
    size_t n = std::max(a.words_.size(), b.words_.size());
    r.words_.resize(n + 1, 0);
    uint64_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
        uint64_t s = carry;
        if (i < a.words_.size()) s += a.words_[i];
        if (i < b.words_.size()) s += b.words_[i];
        r.words_[i] = static_cast<uint32_t>(s & 0xffffffffu);
        carry = s >> 32;
    }
    r.words_[n] = static_cast<uint32_t>(carry);
    r.trim();
    return r;
}

BigInt BigInt::sub(const BigInt& a, const BigInt& b) {
    BigInt r;
    r.words_.resize(a.words_.size(), 0);
    int64_t borrow = 0;
    for (size_t i = 0; i < a.words_.size(); ++i) {
        int64_t d = static_cast<int64_t>(a.words_[i]) - borrow;
        if (i < b.words_.size()) d -= static_cast<int64_t>(b.words_[i]);
        if (d < 0) {
            d += 0x100000000LL;
            borrow = 1;
        } else {
            borrow = 0;
        }
        r.words_[i] = static_cast<uint32_t>(d);
    }
    r.trim();
    return r;
}

BigInt BigInt::mul(const BigInt& a, const BigInt& b) {
    BigInt r;
    if (a.isZero() || b.isZero()) return r;
    r.words_.assign(a.words_.size() + b.words_.size(), 0);
    for (size_t i = 0; i < a.words_.size(); ++i) {
        uint64_t carry = 0;
        uint64_t av = a.words_[i];
        if (av == 0) continue;
        for (size_t j = 0; j < b.words_.size(); ++j) {
            uint64_t cur = r.words_[i + j] + av * b.words_[j] + carry;
            r.words_[i + j] = static_cast<uint32_t>(cur & 0xffffffffu);
            carry = cur >> 32;
        }
        size_t k = i + b.words_.size();
        while (carry) {
            uint64_t cur = r.words_[k] + carry;
            r.words_[k] = static_cast<uint32_t>(cur & 0xffffffffu);
            carry = cur >> 32;
            ++k;
        }
    }
    r.trim();
    return r;
}

BigInt BigInt::shiftLeft(size_t bits) const {
    if (isZero() || bits == 0) return *this;
    size_t wordShift = bits / 32, bitShift = bits % 32;
    BigInt r;
    r.words_.assign(words_.size() + wordShift + 1, 0);
    for (size_t i = 0; i < words_.size(); ++i) {
        uint64_t v = static_cast<uint64_t>(words_[i]) << bitShift;
        r.words_[i + wordShift] |= static_cast<uint32_t>(v & 0xffffffffu);
        r.words_[i + wordShift + 1] |= static_cast<uint32_t>(v >> 32);
    }
    r.trim();
    return r;
}

BigInt BigInt::shiftRight(size_t bits) const {
    if (isZero()) return *this;
    size_t wordShift = bits / 32, bitShift = bits % 32;
    if (wordShift >= words_.size()) return BigInt();
    BigInt r;
    r.words_.assign(words_.size() - wordShift, 0);
    for (size_t i = 0; i < r.words_.size(); ++i) {
        uint64_t v = words_[i + wordShift] >> bitShift;
        if (bitShift && i + wordShift + 1 < words_.size())
            v |= static_cast<uint64_t>(words_[i + wordShift + 1]) << (32 - bitShift);
        r.words_[i] = static_cast<uint32_t>(v & 0xffffffffu);
    }
    r.trim();
    return r;
}

void BigInt::divmod(const BigInt& a, const BigInt& b, BigInt& q, BigInt& r) {
    q = BigInt();
    r = BigInt();
    if (b.isZero()) return;  // phép chia cho 0 — trả về 0/0
    if (compare(a, b) < 0) {
        r = a;
        return;
    }
    if (b.words_.size() == 1) {
        // Đường nhanh: chia cho một từ 32-bit.
        uint64_t d = b.words_[0];
        q.words_.assign(a.words_.size(), 0);
        uint64_t rem = 0;
        for (size_t i = a.words_.size(); i-- > 0;) {
            uint64_t cur = (rem << 32) | a.words_[i];
            q.words_[i] = static_cast<uint32_t>(cur / d);
            rem = cur % d;
        }
        q.trim();
        if (rem) r.words_.push_back(static_cast<uint32_t>(rem));
        return;
    }

    // Thuật toán chia dài Knuth D, chuẩn hoá để chữ số cao nhất >= 2^31.
    int shift = 0;
    uint32_t top = b.words_.back();
    while (!(top & 0x80000000u)) {
        top <<= 1;
        ++shift;
    }
    BigInt u = a.shiftLeft(static_cast<size_t>(shift));
    BigInt v = b.shiftLeft(static_cast<size_t>(shift));
    size_t n = v.words_.size();
    size_t m = u.words_.size() - n;
    u.words_.push_back(0);

    q.words_.assign(m + 1, 0);
    uint64_t vHigh = v.words_[n - 1];
    uint64_t vNext = v.words_[n - 2];

    for (size_t j = m + 1; j-- > 0;) {
        uint64_t num = (static_cast<uint64_t>(u.words_[j + n]) << 32) | u.words_[j + n - 1];
        uint64_t qhat = num / vHigh;
        uint64_t rhat = num % vHigh;
        if (qhat > 0xffffffffull) {
            qhat = 0xffffffffull;
            rhat = num - qhat * vHigh;
        }
        while (rhat <= 0xffffffffull && qhat * vNext > ((rhat << 32) | u.words_[j + n - 2])) {
            --qhat;
            rhat += vHigh;
        }
        // Nhân và trừ.
        int64_t borrow = 0;
        uint64_t carry = 0;
        for (size_t i = 0; i < n; ++i) {
            uint64_t p = qhat * v.words_[i] + carry;
            carry = p >> 32;
            int64_t t = static_cast<int64_t>(u.words_[i + j]) -
                        static_cast<int64_t>(p & 0xffffffffu) - borrow;
            if (t < 0) {
                t += 0x100000000LL;
                borrow = 1;
            } else {
                borrow = 0;
            }
            u.words_[i + j] = static_cast<uint32_t>(t);
        }
        int64_t t = static_cast<int64_t>(u.words_[j + n]) - static_cast<int64_t>(carry) - borrow;
        if (t < 0) {
            t += 0x100000000LL;
            borrow = 1;
        } else {
            borrow = 0;
        }
        u.words_[j + n] = static_cast<uint32_t>(t);

        if (borrow) {
            // qhat lớn hơn 1 đơn vị — cộng bù lại.
            --qhat;
            uint64_t c = 0;
            for (size_t i = 0; i < n; ++i) {
                uint64_t s = static_cast<uint64_t>(u.words_[i + j]) + v.words_[i] + c;
                u.words_[i + j] = static_cast<uint32_t>(s & 0xffffffffu);
                c = s >> 32;
            }
            u.words_[j + n] = static_cast<uint32_t>(u.words_[j + n] + c);
        }
        q.words_[j] = static_cast<uint32_t>(qhat);
    }
    q.trim();

    u.words_.resize(n);
    u.trim();
    r = u.shiftRight(static_cast<size_t>(shift));
}

BigInt BigInt::mod(const BigInt& a, const BigInt& m) {
    BigInt q, r;
    divmod(a, m, q, r);
    return r;
}

BigInt BigInt::mulMod(const BigInt& a, const BigInt& b, const BigInt& m) {
    return mod(mul(a, b), m);
}

BigInt BigInt::addMod(const BigInt& a, const BigInt& b, const BigInt& m) {
    BigInt s = add(a, b);
    if (compare(s, m) >= 0) s = mod(s, m);
    return s;
}

BigInt BigInt::subMod(const BigInt& a, const BigInt& b, const BigInt& m) {
    BigInt x = mod(a, m);
    BigInt y = mod(b, m);
    if (compare(x, y) >= 0) return sub(x, y);
    return sub(add(x, m), y);
}

namespace {

// Số học Montgomery cho modulus lẻ — nhanh hơn nhiều so với chia liên tục.
struct Montgomery {
    BigInt n;        // modulus (lẻ)
    size_t k;        // số từ 32-bit của n
    uint32_t nPrime; // -n^{-1} mod 2^32
    BigInt rr;       // R^2 mod n, với R = 2^(32k)

    bool init(const BigInt& modulus) {
        if (modulus.isZero() || modulus.isEven()) return false;
        n = modulus;
        k = n.words().size();
        uint32_t n0 = n.words()[0];
        // Newton-Raphson trên vành 2^32 để tìm nghịch đảo.
        uint32_t inv = 1;
        for (int i = 0; i < 5; ++i) inv *= 2u - n0 * inv;
        nPrime = static_cast<uint32_t>(0u - inv);
        // R^2 mod n
        BigInt r2 = BigInt(1).shiftLeft(2 * k * 32);
        rr = BigInt::mod(r2, n);
        return true;
    }

    // Rút gọn Montgomery: trả về t * R^{-1} mod n.
    BigInt reduce(const BigInt& t) const {
        std::vector<uint32_t> a(t.words());
        if (a.size() < 2 * k + 1) a.resize(2 * k + 1, 0);
        for (size_t i = 0; i < k; ++i) {
            uint64_t m = (static_cast<uint64_t>(a[i]) * nPrime) & 0xffffffffull;
            uint64_t carry = 0;
            for (size_t j = 0; j < k; ++j) {
                uint64_t cur = a[i + j] + m * n.words()[j] + carry;
                a[i + j] = static_cast<uint32_t>(cur & 0xffffffffu);
                carry = cur >> 32;
            }
            size_t idx = i + k;
            while (carry && idx < a.size()) {
                uint64_t cur = a[idx] + carry;
                a[idx] = static_cast<uint32_t>(cur & 0xffffffffu);
                carry = cur >> 32;
                ++idx;
            }
        }
        // Kết quả nằm ở các từ [k, 2k].
        std::vector<uint32_t> hi(a.begin() + static_cast<long>(k), a.end());
        BigInt out = wordsToBig(hi);
        if (BigInt::compare(out, n) >= 0) out = BigInt::sub(out, n);
        return out;
    }

    static BigInt wordsToBig(const std::vector<uint32_t>& w) {
        // Dựng BigInt từ mảng từ little-endian thông qua chuỗi byte big-endian.
        std::vector<uint8_t> be;
        be.reserve(w.size() * 4);
        for (size_t i = w.size(); i-- > 0;) {
            be.push_back(static_cast<uint8_t>(w[i] >> 24));
            be.push_back(static_cast<uint8_t>(w[i] >> 16));
            be.push_back(static_cast<uint8_t>(w[i] >> 8));
            be.push_back(static_cast<uint8_t>(w[i]));
        }
        return BigInt::fromBytes(be.data(), be.size());
    }

    BigInt toMont(const BigInt& x) const { return reduce(BigInt::mul(BigInt::mod(x, n), rr)); }
    BigInt fromMont(const BigInt& x) const { return reduce(x); }
    BigInt mulMont(const BigInt& a, const BigInt& b) const { return reduce(BigInt::mul(a, b)); }
};

}  // namespace

BigInt BigInt::powMod(const BigInt& base, const BigInt& exp, const BigInt& m) {
    if (m.isZero()) return BigInt();
    if (m.isOne()) return BigInt();
    if (exp.isZero()) return BigInt(1);

    if (!m.isEven()) {
        Montgomery mont;
        if (mont.init(m)) {
            BigInt one = BigInt(1);
            BigInt resM = mont.toMont(one);
            BigInt baseM = mont.toMont(base);
            size_t bits = exp.bitLength();
            // Cửa sổ trượt 4 bit để giảm số phép nhân.
            const size_t kWindow = 4;
            std::vector<BigInt> table(1u << kWindow);
            table[0] = resM;
            for (size_t i = 1; i < table.size(); ++i)
                table[i] = mont.mulMont(table[i - 1], baseM);

            size_t i = bits;
            while (i > 0) {
                size_t take = i >= kWindow ? kWindow : i;
                for (size_t s = 0; s < take; ++s) resM = mont.mulMont(resM, resM);
                uint32_t idx = 0;
                for (size_t s = 0; s < take; ++s) {
                    idx = (idx << 1) | (exp.testBit(i - 1 - s) ? 1u : 0u);
                }
                if (idx) resM = mont.mulMont(resM, table[idx]);
                i -= take;
            }
            return mont.fromMont(resM);
        }
    }

    // Đường chậm cho modulus chẵn.
    BigInt result(1);
    BigInt b = mod(base, m);
    size_t bits = exp.bitLength();
    for (size_t i = bits; i-- > 0;) {
        result = mulMod(result, result, m);
        if (exp.testBit(i)) result = mulMod(result, b, m);
    }
    return result;
}

BigInt BigInt::gcd(BigInt a, BigInt b) {
    while (!b.isZero()) {
        BigInt r = mod(a, b);
        a = b;
        b = r;
    }
    return a;
}

bool BigInt::modInverse(const BigInt& a, const BigInt& m, BigInt& out) {
    // Thuật toán Euclid mở rộng, giữ dấu bằng cách theo dõi thủ công.
    BigInt r0 = mod(a, m), r1 = m;
    BigInt s0(1), s1(0);
    bool s0neg = false, s1neg = false;
    if (r0.isZero()) return false;

    // Bất biến: r0 = ±s0 * a (mod m)
    while (!r1.isZero()) {
        BigInt q, r;
        divmod(r0, r1, q, r);
        // (r0, r1) <- (r1, r)
        r0 = r1;
        r1 = r;
        // s <- s0 - q*s1  (theo dõi dấu)
        BigInt qs = mul(q, s1);
        BigInt ns;
        bool nsNeg;
        if (s0neg == s1neg) {
            // cùng dấu: trừ giá trị tuyệt đối
            if (compare(s0, qs) >= 0) {
                ns = sub(s0, qs);
                nsNeg = s0neg;
            } else {
                ns = sub(qs, s0);
                nsNeg = !s0neg;
            }
        } else {
            ns = add(s0, qs);
            nsNeg = s0neg;
        }
        s0 = s1;
        s0neg = s1neg;
        s1 = ns;
        s1neg = nsNeg;
    }
    if (!r0.isOne()) return false;
    BigInt res = mod(s0, m);
    if (s0neg && !res.isZero()) res = sub(m, res);
    out = res;
    return true;
}

bool BigInt::isProbablePrime(int rounds) const {
    if (isZero()) return false;
    if (isOne()) return false;
    if (words_.size() == 1) {
        uint32_t v = words_[0];
        if (v == 2 || v == 3) return true;
        if (v % 2 == 0) return false;
    } else if (isEven()) {
        return false;
    }
    static const uint32_t kSmallPrimes[] = {3,  5,  7,  11, 13, 17, 19, 23, 29, 31, 37,
                                            41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83,
                                            89, 97, 101, 103, 107, 109, 113};
    for (uint32_t p : kSmallPrimes) {
        BigInt bp(p);
        if (compare(*this, bp) == 0) return true;
        BigInt r = mod(*this, bp);
        if (r.isZero()) return false;
    }

    BigInt nMinus1 = sub(*this, BigInt(1));
    BigInt d = nMinus1;
    size_t s = 0;
    while (d.isEven()) {
        d = d.shiftRight(1);
        ++s;
    }

    for (int i = 0; i < rounds; ++i) {
        // Chọn cơ số ngẫu nhiên trong [2, n-2].
        BigInt a;
        size_t bytes = byteLength();
        for (int attempt = 0; attempt < 64; ++attempt) {
            Bytes rnd = randomBytes(bytes);
            a = fromBytes(rnd);
            a = mod(a, sub(nMinus1, BigInt(1)));
            a = add(a, BigInt(2));
            if (compare(a, nMinus1) < 0) break;
        }
        BigInt x = powMod(a, d, *this);
        if (x.isOne() || compare(x, nMinus1) == 0) continue;
        bool composite = true;
        for (size_t r = 1; r < s; ++r) {
            x = mulMod(x, x, *this);
            if (compare(x, nMinus1) == 0) {
                composite = false;
                break;
            }
        }
        if (composite) return false;
    }
    return true;
}

}  // namespace crypto
}  // namespace ttd
