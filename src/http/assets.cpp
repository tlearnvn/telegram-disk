#include "http/assets.h"

#include <cstring>
#include <mutex>

#include "common/fsutil.h"
#include "common/logging.h"
#include "common/strutil.h"

namespace ttd {
namespace assets {

// Được sinh bởi cmake/EmbedAssets.cmake
struct EmbeddedAsset {
    const char* path;
    const unsigned char* data;
    size_t size;
};
const EmbeddedAsset* embeddedAssetTable();
size_t embeddedAssetCount();

namespace {
std::string& overrideDirRef() {
    static std::string dir;
    return dir;
}
std::mutex& mu() {
    static std::mutex m;
    return m;
}

// Chặn thoát khỏi thư mục gốc bằng "..".
bool isSafeRelativePath(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] == '/' || path[0] == '\\') return false;
    if (path.find("..") != std::string::npos) return false;
#if defined(_WIN32)
    if (path.size() >= 2 && path[1] == ':') return false;
#endif
    return true;
}
}  // namespace

void setOverrideDirectory(const std::string& dir) {
    std::lock_guard<std::mutex> lk(mu());
    overrideDirRef() = dir;
    if (!dir.empty()) LOG_INFO("assets", "Đọc tài nguyên web từ thư mục: %s", dir.c_str());
}

const std::string& overrideDirectory() {
    static std::string empty;
    std::lock_guard<std::mutex> lk(mu());
    return overrideDirRef().empty() ? empty : overrideDirRef();
}

bool find(const std::string& path, std::string& out) {
    if (!isSafeRelativePath(path)) return false;

    std::string dir;
    {
        std::lock_guard<std::mutex> lk(mu());
        dir = overrideDirRef();
    }
    if (!dir.empty()) {
        std::string full = joinPath(dir, path);
        if (pathExists(full) && !isDirectory(full)) {
            if (readWholeFile(full, out)) return true;
        }
    }

    const EmbeddedAsset* table = embeddedAssetTable();
    size_t n = embeddedAssetCount();
    for (size_t i = 0; i < n; ++i) {
        if (path == table[i].path) {
            out.assign(reinterpret_cast<const char*>(table[i].data), table[i].size);
            return true;
        }
    }
    return false;
}

const char* findTextAsset(const std::string& path) {
    const EmbeddedAsset* table = embeddedAssetTable();
    size_t n = embeddedAssetCount();
    for (size_t i = 0; i < n; ++i) {
        if (path == table[i].path) return reinterpret_cast<const char*>(table[i].data);
    }
    return nullptr;
}

std::vector<std::string> listPaths() {
    std::vector<std::string> out;
    const EmbeddedAsset* table = embeddedAssetTable();
    size_t n = embeddedAssetCount();
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(table[i].path);
    return out;
}

}  // namespace assets
}  // namespace ttd
