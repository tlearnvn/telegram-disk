// SRP dùng cho xác thực hai lớp (2FA) của Telegram.
// Thuật toán: passwordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000SHA256ModPow
#pragma once

#include <cstdint>
#include <string>

#include "common/strutil.h"

namespace ttd {
namespace crypto {

struct SrpAlgo {
    Bytes salt1;
    Bytes salt2;
    int g = 0;
    Bytes p;  // 2048-bit modulus
    bool valid() const { return !salt1.empty() && !salt2.empty() && g > 0 && p.size() == 256; }
};

struct SrpResult {
    Bytes A;
    Bytes M1;
    bool ok = false;
    std::string error;
};

// Tính x = PH2(password, salt1, salt2) theo đặc tả Telegram.
Bytes srpPasswordHash(const std::string& password, const Bytes& salt1, const Bytes& salt2);

// Tính (A, M1) để gửi trong inputCheckPasswordSRP.
//   srpB: giá trị B do máy chủ gửi (account.password.srp_B)
SrpResult srpCompute(const SrpAlgo& algo, const Bytes& srpB, const std::string& password);

// Kiểm tra tham số p, g do máy chủ cung cấp có an toàn không (p nguyên tố an toàn,
// g là căn nguyên thuỷ theo yêu cầu của Telegram).
bool srpCheckPrimeAndGood(const Bytes& p, int g);

}  // namespace crypto
}  // namespace ttd
