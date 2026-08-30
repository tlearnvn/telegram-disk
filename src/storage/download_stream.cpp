#include "storage/download_stream.h"

#include "common/strutil.h"

namespace ttd {
namespace storage {

bool parseRangeHeader(const std::string& header, uint64_t fileSize, ByteRange& out,
                      bool& unsatisfiable) {
    unsatisfiable = false;
    std::string h = trim(header);
    if (h.empty()) return false;
    if (!startsWith(toLower(h), "bytes=")) return false;
    std::string spec = trim(h.substr(6));

    // Chỉ lấy dải đầu tiên nếu client gửi nhiều dải.
    size_t comma = spec.find(',');
    if (comma != std::string::npos) spec = trim(spec.substr(0, comma));

    size_t dash = spec.find('-');
    if (dash == std::string::npos) return false;
    std::string startStr = trim(spec.substr(0, dash));
    std::string endStr = trim(spec.substr(dash + 1));

    if (fileSize == 0) {
        unsatisfiable = true;
        return false;
    }

    if (startStr.empty()) {
        // Dạng "-N": N byte cuối cùng.
        uint64_t suffix = 0;
        if (!parseUInt64(endStr, suffix) || suffix == 0) {
            unsatisfiable = true;
            return false;
        }
        if (suffix > fileSize) suffix = fileSize;
        out.start = fileSize - suffix;
        out.end = fileSize - 1;
        return true;
    }

    uint64_t start = 0;
    if (!parseUInt64(startStr, start)) return false;
    if (start >= fileSize) {
        unsatisfiable = true;
        return false;
    }
    uint64_t end = fileSize - 1;
    if (!endStr.empty()) {
        if (!parseUInt64(endStr, end)) return false;
        if (end >= fileSize) end = fileSize - 1;
    }
    if (end < start) {
        unsatisfiable = true;
        return false;
    }
    out.start = start;
    out.end = end;
    return true;
}

std::string makeContentRange(const ByteRange& range, uint64_t fileSize) {
    return "bytes " + std::to_string(range.start) + "-" + std::to_string(range.end) + "/" +
           std::to_string(fileSize);
}

std::string makeUnsatisfiedContentRange(uint64_t fileSize) {
    return "bytes */" + std::to_string(fileSize);
}

}  // namespace storage
}  // namespace ttd
