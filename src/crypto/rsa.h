// RSA khoá công khai: nạp khoá dạng PEM/DER, mã hoá thô, PKCS#1 v1.5 và OAEP.
// Dùng cho: bắt tay MTProto (mã hoá thô 2048-bit) và xác thực MySQL caching_sha2 (OAEP).
#pragma once

#include <string>
#include <vector>

#include "common/strutil.h"
#include "crypto/bigint.h"

namespace ttd {
namespace crypto {

class RsaPublicKey {
public:
    bool valid() const { return !n_.isZero() && !e_.isZero(); }

    // Nạp từ chuỗi PEM "-----BEGIN RSA PUBLIC KEY-----" (PKCS#1)
    // hoặc "-----BEGIN PUBLIC KEY-----" (SubjectPublicKeyInfo).
    static bool fromPem(const std::string& pem, RsaPublicKey& out);
    static bool fromModulusExponent(const Bytes& modulus, const Bytes& exponent,
                                    RsaPublicKey& out);

    const BigInt& modulus() const { return n_; }
    const BigInt& exponent() const { return e_; }
    size_t keySizeBytes() const { return n_.byteLength(); }

    // Dấu vân tay khoá kiểu Telegram: 8 byte cuối của SHA1(TL(n) + TL(e)),
    // đọc theo thứ tự little-endian thành số nguyên 64-bit có dấu.
    int64_t telegramFingerprint() const;

    // c = m^e mod n. Dữ liệu vào phải nhỏ hơn modulus.
    bool rawEncrypt(const Bytes& input, Bytes& out) const;

    // Đệm PKCS#1 v1.5 kiểu 2 (mã hoá).
    bool encryptPkcs1v15(const Bytes& message, Bytes& out) const;

    // Đệm OAEP với MGF1-SHA1 (MySQL caching_sha2_password dùng cách này).
    bool encryptOaepSha1(const Bytes& message, Bytes& out) const;

private:
    BigInt n_;
    BigInt e_;
};

// Bộ phân tích DER tối giản, đủ để đọc khoá công khai RSA.
namespace der {
bool parseRsaPublicKey(const Bytes& der, Bytes& modulus, Bytes& exponent);
}

}  // namespace crypto
}  // namespace ttd
