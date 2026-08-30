#include "crypto/random.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "crypto/hash.h"

namespace ttd {
namespace crypto {

namespace {

// Bộ sinh dự phòng theo kiểu ChaCha-lite dựa trên SHA-256, chỉ dùng khi
// nguồn của hệ điều hành không khả dụng (rất hiếm). Vẫn được gieo mầm từ
// nhiều nguồn entropy độc lập.
class FallbackRng {
public:
    void reseed() {
        Sha256 h;
        auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        h.update(reinterpret_cast<const uint8_t*>(&now), sizeof(now));
        std::random_device rd;
        for (int i = 0; i < 16; ++i) {
            unsigned int v = rd();
            h.update(reinterpret_cast<const uint8_t*>(&v), sizeof(v));
        }
        void* addr = this;
        h.update(reinterpret_cast<const uint8_t*>(&addr), sizeof(addr));
        h.update(state_, sizeof(state_));
        h.finish(state_);
        counter_ = 0;
    }

    void generate(uint8_t* out, size_t len) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!seeded_) {
            std::memset(state_, 0, sizeof(state_));
            reseed();
            seeded_ = true;
        }
        while (len > 0) {
            Sha256 h;
            h.update(state_, sizeof(state_));
            h.update(reinterpret_cast<const uint8_t*>(&counter_), sizeof(counter_));
            uint8_t block[32];
            h.finish(block);
            ++counter_;
            size_t take = len < 32 ? len : 32;
            std::memcpy(out, block, take);
            out += take;
            len -= take;
            if ((counter_ & 0xffff) == 0) reseed();
        }
    }

private:
    std::mutex mu_;
    bool seeded_ = false;
    uint8_t state_[32] = {0};
    uint64_t counter_ = 0;
};

FallbackRng& fallbackRng() {
    static FallbackRng rng;
    return rng;
}

#if !defined(_WIN32)
int urandomFd() {
    static int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    return fd;
}
#endif

}  // namespace

void fillRandom(uint8_t* out, size_t len) {
    if (len == 0) return;
#if defined(_WIN32)
    NTSTATUS st = BCryptGenRandom(nullptr, out, static_cast<ULONG>(len),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st == 0) return;
#else
    int fd = urandomFd();
    if (fd >= 0) {
        size_t got = 0;
        while (got < len) {
            ssize_t n = ::read(fd, out + got, len - got);
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
        if (got == len) return;
    }
#endif
    fallbackRng().generate(out, len);
}

Bytes randomBytes(size_t len) {
    Bytes b(len);
    if (len) fillRandom(b.data(), len);
    return b;
}

uint32_t randomUInt32() {
    uint32_t v;
    fillRandom(reinterpret_cast<uint8_t*>(&v), sizeof(v));
    return v;
}

uint64_t randomUInt64() {
    uint64_t v;
    fillRandom(reinterpret_cast<uint8_t*>(&v), sizeof(v));
    return v;
}

int64_t randomInt64() { return static_cast<int64_t>(randomUInt64()); }

uint64_t randomBelow(uint64_t bound) {
    if (bound == 0) return 0;
    // Loại bỏ phần dư để phân phối đều tuyệt đối.
    uint64_t limit = UINT64_MAX - (UINT64_MAX % bound);
    uint64_t v;
    do {
        v = randomUInt64();
    } while (v >= limit);
    return v % bound;
}

std::string randomHex(size_t len) { return toHex(randomBytes(len)); }

std::string randomToken(size_t bytes) { return base64UrlEncode(randomBytes(bytes)); }

}  // namespace crypto
}  // namespace ttd
