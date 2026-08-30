// Bộ nhớ đệm khối 1 MB cho dữ liệu tải về từ Telegram.
// Giúp tua qua lại khi phát trực tuyến mà không phải tải lại cùng một khối.
#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/strutil.h"

namespace ttd {
namespace storage {

class BlockCache {
public:
    explicit BlockCache(uint64_t capacityBytes);

    // Khoá gồm mã tài liệu và vị trí khối.
    bool get(int64_t documentId, uint64_t blockOffset, Bytes& out);
    void put(int64_t documentId, uint64_t blockOffset, const Bytes& data);
    void invalidate(int64_t documentId);
    void clear();

    void setCapacity(uint64_t bytes);
    uint64_t capacity() const;
    uint64_t used() const;
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }

private:
    struct Entry {
        std::string key;
        Bytes data;
    };

    std::string makeKey(int64_t documentId, uint64_t blockOffset) const;
    void evictLocked();

    mutable std::mutex mu_;
    uint64_t capacity_;
    uint64_t used_ = 0;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
    std::list<Entry> order_;  // mới nhất ở đầu
    std::unordered_map<std::string, std::list<Entry>::iterator> index_;
};

}  // namespace storage
}  // namespace ttd
