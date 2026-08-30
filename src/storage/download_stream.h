// Hỗ trợ phát trực tuyến theo dải byte (HTTP Range / WebDAV).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ttd {
namespace storage {

struct ByteRange {
    uint64_t start = 0;
    uint64_t end = 0;  // bao gồm cả byte này
    uint64_t length() const { return end >= start ? end - start + 1 : 0; }
};

// Phân tích tiêu đề Range của HTTP. Trả về false nếu không hợp lệ.
// Chỉ hỗ trợ một dải (đủ cho phát video và mọi trình phát phổ biến).
bool parseRangeHeader(const std::string& header, uint64_t fileSize, ByteRange& out,
                      bool& unsatisfiable);

// Tạo giá trị cho tiêu đề Content-Range.
std::string makeContentRange(const ByteRange& range, uint64_t fileSize);
std::string makeUnsatisfiedContentRange(uint64_t fileSize);

}  // namespace storage
}  // namespace ttd
