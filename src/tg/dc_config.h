// Danh sách trung tâm dữ liệu (DC) của Telegram và khoá công khai RSA của máy chủ.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "crypto/rsa.h"

namespace ttd {
namespace tg {

struct DcEndpoint {
    int dcId = 0;
    std::string ip;
    uint16_t port = 443;
    bool ipv6 = false;
    bool mediaOnly = false;
    bool cdn = false;
    bool testMode = false;
};

class DcConfig {
public:
    static DcConfig& instance();

    // Nạp danh sách mặc định (địa chỉ công bố chính thức của Telegram).
    void loadDefaults();
    // Cập nhật từ kết quả help.getConfig.
    void updateFromConfig(const std::vector<DcEndpoint>& endpoints);

    // Lấy điểm kết nối phù hợp nhất cho một DC.
    bool endpointFor(int dcId, bool testMode, DcEndpoint& out) const;
    std::vector<DcEndpoint> allEndpoints() const;

    // Khoá công khai của máy chủ Telegram, tra theo dấu vân tay.
    const crypto::RsaPublicKey* keyByFingerprint(int64_t fingerprint) const;
    // Chọn khoá đầu tiên khớp với danh sách vân tay máy chủ gửi tới.
    const crypto::RsaPublicKey* selectKey(const std::vector<int64_t>& fingerprints,
                                          int64_t& chosenFingerprint) const;
    // Thêm khoá từ chuỗi PEM (cho phép người dùng bổ sung khi Telegram đổi khoá).
    bool addPublicKeyPem(const std::string& pem);
    size_t keyCount() const;

private:
    DcConfig() = default;
    void ensureKeysLoaded();

    mutable std::mutex mu_;
    std::vector<DcEndpoint> endpoints_;
    std::map<int64_t, crypto::RsaPublicKey> keys_;
    bool keysLoaded_ = false;
    bool defaultsLoaded_ = false;
};

}  // namespace tg
}  // namespace ttd
