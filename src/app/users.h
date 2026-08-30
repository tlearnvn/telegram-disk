// Quản lý người dùng web: băm mật khẩu, đăng nhập, phiên làm việc.
#pragma once

#include <string>
#include <vector>

#include "common/config.h"
#include "db/database.h"

namespace ttd {
namespace app {

// Băm mật khẩu theo dạng: pbkdf2-sha256$<số vòng>$<muối hex>$<băm hex>
std::string hashPassword(const std::string& password, int iterations);
bool verifyPassword(const std::string& password, const std::string& stored);

// Đánh giá độ mạnh mật khẩu; trả về thông báo tiếng Việt nếu chưa đạt.
bool checkPasswordStrength(const std::string& password, std::string& message);

struct AuthResult {
    bool ok = false;
    db::UserEntry user;
    std::string token;
    int64_t expiresAt = 0;
    std::string error;
};

class UserManager {
public:
    UserManager(db::Database& database, const Config& config);

    // Tạo quản trị viên đầu tiên nếu cơ sở dữ liệu còn trống.
    // Trả về mật khẩu sinh ngẫu nhiên qua `generatedPassword` (rỗng nếu đã có sẵn).
    bool ensureAdminExists(std::string& generatedPassword, std::string& error);

    AuthResult login(const std::string& username, const std::string& password,
                     const std::string& userAgent, const std::string& ip);
    bool logout(const std::string& token);
    // Xác thực theo mã phiên trong cookie.
    bool authenticate(const std::string& token, db::UserEntry& out);
    // Xác thực theo tiêu đề Authorization: Basic (dùng cho WebDAV).
    bool authenticateBasic(const std::string& authorizationHeader, db::UserEntry& out);

    bool createUser(const std::string& username, const std::string& password,
                    const std::string& displayName, bool isAdmin, uint64_t quota,
                    db::UserEntry& out, std::string& error);
    bool changePassword(int userId, const std::string& newPassword, std::string& error);
    bool setEnabled(int userId, bool enabled, std::string& error);
    bool deleteUser(int userId, std::string& error);
    bool listUsers(std::vector<db::UserEntry>& out, std::string& error);

    void cleanupExpiredSessions();

private:
    db::Database& db_;
    const Config& config_;
};

}  // namespace app
}  // namespace ttd
