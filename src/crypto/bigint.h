// Số nguyên lớn không dấu, đủ dùng cho RSA-2048, Diffie-Hellman 2048-bit và SRP.
// Cài đặt riêng để không phụ thuộc thư viện ngoài.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/strutil.h"

namespace ttd {
namespace crypto {

class BigInt {
public:
    BigInt() = default;
    explicit BigInt(uint64_t v);

    static BigInt fromBytes(const uint8_t* data, size_t len);  // big-endian
    static BigInt fromBytes(const Bytes& b) { return fromBytes(b.data(), b.size()); }
    static BigInt fromHex(const std::string& hex);
    static BigInt fromDecimal(const std::string& dec);

    // Xuất big-endian; nếu minLen > 0 thì đệm số 0 ở đầu cho đủ độ dài.
    Bytes toBytes(size_t minLen = 0) const;
    std::string toHex() const;
    std::string toDecimal() const;

    bool isZero() const { return words_.empty(); }
    bool isOne() const { return words_.size() == 1 && words_[0] == 1; }
    bool isEven() const { return words_.empty() || (words_[0] & 1) == 0; }
    size_t bitLength() const;
    size_t byteLength() const { return (bitLength() + 7) / 8; }
    bool testBit(size_t i) const;

    static int compare(const BigInt& a, const BigInt& b);
    bool operator==(const BigInt& o) const { return compare(*this, o) == 0; }
    bool operator!=(const BigInt& o) const { return compare(*this, o) != 0; }
    bool operator<(const BigInt& o) const { return compare(*this, o) < 0; }
    bool operator<=(const BigInt& o) const { return compare(*this, o) <= 0; }
    bool operator>(const BigInt& o) const { return compare(*this, o) > 0; }
    bool operator>=(const BigInt& o) const { return compare(*this, o) >= 0; }

    static BigInt add(const BigInt& a, const BigInt& b);
    // Yêu cầu a >= b (không dấu).
    static BigInt sub(const BigInt& a, const BigInt& b);
    static BigInt mul(const BigInt& a, const BigInt& b);
    static void divmod(const BigInt& a, const BigInt& b, BigInt& q, BigInt& r);
    static BigInt mod(const BigInt& a, const BigInt& m);
    static BigInt mulMod(const BigInt& a, const BigInt& b, const BigInt& m);
    static BigInt addMod(const BigInt& a, const BigInt& b, const BigInt& m);
    static BigInt subMod(const BigInt& a, const BigInt& b, const BigInt& m);
    // Luỹ thừa modulo — dùng phương pháp Montgomery khi modulus lẻ.
    static BigInt powMod(const BigInt& base, const BigInt& exp, const BigInt& m);
    static BigInt gcd(BigInt a, BigInt b);
    // Nghịch đảo modulo; trả về false nếu không tồn tại.
    static bool modInverse(const BigInt& a, const BigInt& m, BigInt& out);

    BigInt shiftLeft(size_t bits) const;
    BigInt shiftRight(size_t bits) const;

    // Kiểm tra số nguyên tố Miller-Rabin (dùng để xác minh dh_prime của Telegram).
    bool isProbablePrime(int rounds = 30) const;

    const std::vector<uint32_t>& words() const { return words_; }

private:
    void trim();
    std::vector<uint32_t> words_;  // little-endian, cơ số 2^32
};

}  // namespace crypto
}  // namespace ttd
