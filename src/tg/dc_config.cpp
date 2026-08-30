#include "tg/dc_config.h"

#include "common/logging.h"

namespace ttd {
namespace tg {

namespace {
constexpr const char* kTag = "tg.dc";

// Khoá công khai RSA của máy chủ Telegram (bản đang dùng cho hệ thống thật).
// Dấu vân tay: 0xd09d1d85de64fd85
const char* kProductionPublicKey =
    "-----BEGIN RSA PUBLIC KEY-----\n"
    "MIIBCgKCAQEA6LszBcC1LGzyr992NzE0ieY+BSaOW622Aa9Bd4ZHLl+TuFQ4lo4g\n"
    "5nKaMBwK/BIb9xUfg0Q29/2mgIR6Zr9krM7HjuIcCzFvDtr+L0GQjae9H0pRB2OO\n"
    "62cECs5HKhT5DZ98K33vmWiLowc621dQuwKWSQKjWf50XYFw42h21P2KXUGyp2y/\n"
    "+aEyZ+uVgLLQbRA1dEjSDZ2iGRy12Mk5gpYc397aYp438fsJoHIgJ2lgMv5h7WY9\n"
    "t6N/byY9Nw9p21Og3AoXSL2q/2IJ1WRUhebgAdGVMlV1fkuOQoEzR7EdpqtQD9Cs\n"
    "5+bfo3Nhmcyvk5ftB0WkJ9z6bNZ7yxrP8wIDAQAB\n"
    "-----END RSA PUBLIC KEY-----\n";

}  // namespace

DcConfig& DcConfig::instance() {
    static DcConfig inst;
    return inst;
}

void DcConfig::ensureKeysLoaded() {
    if (keysLoaded_) return;
    keysLoaded_ = true;
    crypto::RsaPublicKey key;
    if (crypto::RsaPublicKey::fromPem(kProductionPublicKey, key)) {
        int64_t fp = key.telegramFingerprint();
        keys_[fp] = key;
        LOG_DEBUG(kTag, "Đã nạp khoá công khai Telegram, vân tay 0x%016llx",
                  static_cast<unsigned long long>(fp));
    } else {
        LOG_ERROR(kTag, "Không đọc được khoá công khai Telegram dựng sẵn");
    }
}

void DcConfig::loadDefaults() {
    std::lock_guard<std::mutex> lk(mu_);
    ensureKeysLoaded();
    if (defaultsLoaded_) return;
    defaultsLoaded_ = true;

    // Địa chỉ công bố chính thức. Sau khi kết nối, ứng dụng sẽ gọi help.getConfig
    // để cập nhật danh sách mới nhất.
    const struct {
        int dc;
        const char* ip;
        bool test;
    } kDefaults[] = {
        {1, "149.154.175.53", false},
        {2, "149.154.167.51", false},
        {3, "149.154.175.100", false},
        {4, "149.154.167.91", false},
        {5, "91.108.56.130", false},
        {1, "149.154.175.10", true},
        {2, "149.154.167.40", true},
        {3, "149.154.175.117", true},
    };
    for (const auto& d : kDefaults) {
        DcEndpoint e;
        e.dcId = d.dc;
        e.ip = d.ip;
        e.port = 443;
        e.testMode = d.test;
        endpoints_.push_back(e);
    }
    LOG_DEBUG(kTag, "Đã nạp %zu điểm kết nối mặc định", endpoints_.size());
}

void DcConfig::updateFromConfig(const std::vector<DcEndpoint>& endpoints) {
    if (endpoints.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    // Giữ lại các mục thử nghiệm, thay toàn bộ mục thật bằng danh sách mới.
    std::vector<DcEndpoint> merged;
    for (const auto& e : endpoints_)
        if (e.testMode) merged.push_back(e);
    for (const auto& e : endpoints) merged.push_back(e);
    endpoints_ = std::move(merged);
    LOG_INFO(kTag, "Đã cập nhật %zu điểm kết nối từ máy chủ", endpoints.size());
}

bool DcConfig::endpointFor(int dcId, bool testMode, DcEndpoint& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    // Ưu tiên: IPv4, không phải media-only, không phải CDN.
    const DcEndpoint* best = nullptr;
    for (const auto& e : endpoints_) {
        if (e.dcId != dcId || e.testMode != testMode) continue;
        if (e.cdn) continue;
        if (!best) {
            best = &e;
            continue;
        }
        int scoreNew = (e.ipv6 ? 2 : 0) + (e.mediaOnly ? 1 : 0);
        int scoreOld = (best->ipv6 ? 2 : 0) + (best->mediaOnly ? 1 : 0);
        if (scoreNew < scoreOld) best = &e;
    }
    if (!best) return false;
    out = *best;
    return true;
}

std::vector<DcEndpoint> DcConfig::allEndpoints() const {
    std::lock_guard<std::mutex> lk(mu_);
    return endpoints_;
}

const crypto::RsaPublicKey* DcConfig::keyByFingerprint(int64_t fingerprint) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = keys_.find(fingerprint);
    return it == keys_.end() ? nullptr : &it->second;
}

const crypto::RsaPublicKey* DcConfig::selectKey(const std::vector<int64_t>& fingerprints,
                                                int64_t& chosenFingerprint) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (int64_t fp : fingerprints) {
        auto it = keys_.find(fp);
        if (it != keys_.end()) {
            chosenFingerprint = fp;
            return &it->second;
        }
    }
    return nullptr;
}

bool DcConfig::addPublicKeyPem(const std::string& pem) {
    crypto::RsaPublicKey key;
    if (!crypto::RsaPublicKey::fromPem(pem, key)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    ensureKeysLoaded();
    int64_t fp = key.telegramFingerprint();
    keys_[fp] = key;
    LOG_INFO(kTag, "Đã thêm khoá công khai, vân tay 0x%016llx",
             static_cast<unsigned long long>(fp));
    return true;
}

size_t DcConfig::keyCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return keys_.size();
}

}  // namespace tg
}  // namespace ttd
