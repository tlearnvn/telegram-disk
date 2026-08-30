#include "storage/block_cache.h"

#include <cstdio>

namespace ttd {
namespace storage {

BlockCache::BlockCache(uint64_t capacityBytes) : capacity_(capacityBytes) {}

std::string BlockCache::makeKey(int64_t documentId, uint64_t blockOffset) const {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%llx:%llx", static_cast<unsigned long long>(documentId),
                  static_cast<unsigned long long>(blockOffset));
    return buf;
}

bool BlockCache::get(int64_t documentId, uint64_t blockOffset, Bytes& out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = index_.find(makeKey(documentId, blockOffset));
    if (it == index_.end()) {
        ++misses_;
        return false;
    }
    // Đưa lên đầu danh sách (mới dùng gần đây nhất).
    order_.splice(order_.begin(), order_, it->second);
    out = it->second->data;
    ++hits_;
    return true;
}

void BlockCache::put(int64_t documentId, uint64_t blockOffset, const Bytes& data) {
    if (data.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (data.size() > capacity_) return;  // khối lớn hơn cả bộ đệm

    std::string key = makeKey(documentId, blockOffset);
    auto it = index_.find(key);
    if (it != index_.end()) {
        used_ -= it->second->data.size();
        order_.erase(it->second);
        index_.erase(it);
    }
    order_.push_front(Entry{key, data});
    index_[key] = order_.begin();
    used_ += data.size();
    evictLocked();
}

void BlockCache::evictLocked() {
    while (used_ > capacity_ && !order_.empty()) {
        auto& last = order_.back();
        used_ -= last.data.size();
        index_.erase(last.key);
        order_.pop_back();
    }
}

void BlockCache::invalidate(int64_t documentId) {
    std::lock_guard<std::mutex> lk(mu_);
    char prefix[24];
    std::snprintf(prefix, sizeof(prefix), "%llx:", static_cast<unsigned long long>(documentId));
    std::string p = prefix;
    for (auto it = order_.begin(); it != order_.end();) {
        if (it->key.compare(0, p.size(), p) == 0) {
            used_ -= it->data.size();
            index_.erase(it->key);
            it = order_.erase(it);
        } else {
            ++it;
        }
    }
}

void BlockCache::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    order_.clear();
    index_.clear();
    used_ = 0;
}

void BlockCache::setCapacity(uint64_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    capacity_ = bytes;
    evictLocked();
}

uint64_t BlockCache::capacity() const {
    std::lock_guard<std::mutex> lk(mu_);
    return capacity_;
}

uint64_t BlockCache::used() const {
    std::lock_guard<std::mutex> lk(mu_);
    return used_;
}

}  // namespace storage
}  // namespace ttd
