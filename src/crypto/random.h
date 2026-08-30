// Nguồn số ngẫu nhiên an toàn mật mã, lấy từ hệ điều hành.
#pragma once

#include <cstdint>
#include <string>

#include "common/strutil.h"

namespace ttd {
namespace crypto {

// Sinh `len` byte ngẫu nhiên an toàn. Ném std::runtime_error nếu hệ điều hành từ chối.
Bytes randomBytes(size_t len);
void fillRandom(uint8_t* out, size_t len);

uint32_t randomUInt32();
uint64_t randomUInt64();
int64_t randomInt64();
// Số ngẫu nhiên trong [0, bound), bound > 0.
uint64_t randomBelow(uint64_t bound);

// Chuỗi ngẫu nhiên dạng hex (len byte -> 2*len ký tự).
std::string randomHex(size_t len);
// Định danh dạng URL-safe base64 (dùng cho mã phiên, khoá chia sẻ).
std::string randomToken(size_t bytes = 24);

}  // namespace crypto
}  // namespace ttd
