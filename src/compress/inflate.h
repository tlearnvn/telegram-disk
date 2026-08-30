// Bộ giải nén DEFLATE (RFC 1951), zlib (RFC 1950) và gzip (RFC 1952).
// Cần cho gzip_packed của MTProto và cho Content-Encoding của WebDAV client.
// Kèm bộ nén DEFLATE ở chế độ "stored" để tạo gzip hợp lệ khi cần.
#pragma once

#include <cstdint>
#include <string>

#include "common/strutil.h"

namespace ttd {
namespace compress {

// Giải nén luồng DEFLATE thô. maxOutput = 0 nghĩa là không giới hạn.
bool inflateRaw(const uint8_t* data, size_t len, Bytes& out, size_t maxOutput = 0);
// Giải nén luồng zlib (có 2 byte tiêu đề + Adler-32).
bool inflateZlib(const uint8_t* data, size_t len, Bytes& out, size_t maxOutput = 0);
// Giải nén luồng gzip (tiêu đề 0x1f 0x8b).
bool inflateGzip(const uint8_t* data, size_t len, Bytes& out, size_t maxOutput = 0);
// Tự nhận dạng zlib / gzip / deflate thô.
bool inflateAuto(const Bytes& data, Bytes& out, size_t maxOutput = 0);

// Nén thành gzip dùng khối "stored" (không nén thật) — luôn hợp lệ, dùng khi
// cần trả dữ liệu đã có sẵn dạng gzip mà không muốn kéo thêm thư viện nén.
Bytes gzipStored(const uint8_t* data, size_t len);

uint32_t adler32(const uint8_t* data, size_t len, uint32_t seed = 1);

}  // namespace compress
}  // namespace ttd
