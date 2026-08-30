#include "app/users.h"

#include "common/logging.h"
#include "common/strutil.h"
#include "common/timeutil.h"
#include "crypto/hash.h"
#include "crypto/random.h"

namespace ttd {
namespace app {

namespace {
constexpr const char* kTag = "users";
constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;
}  // namespace

std::string hashPassword(const std::string& password, int iterations) {
    if (iterations < 10000) iterations = 10000;
    Bytes salt = crypto::randomBytes(kSaltBytes);
    Bytes hash = crypto::pbkdf2HmacSha256(toBytes(password), salt, iterations, kHashBytes);
    return "pbkdf2-sha256$" + std::to_string(iterations) + "$" + toHex(salt) + "$" +
           toHex(hash);
}

bool verifyPassword(const std::string& password, const std::string& stored) {
    std::vector<std::string> parts = split(stored, '$');
    if (parts.size() != 4) return false;
    if (parts[0] != "pbkdf2-sha256") return false;
    int64_t iterations = 0;
    if (!parseInt64(parts[1], iterations) || iterations < 1) return false;
    Bytes salt = fromHex(parts[2]);
    Bytes expected = fromHex(parts[3]);
    if (salt.empty() || expected.empty()) return false;
    Bytes actual = crypto::pbkdf2HmacSha256(toBytes(password), salt,
                                            static_cast<int>(iterations), expected.size());
    return crypto::constantTimeEquals(actual, expected);
}

bool checkPasswordStrength(const std::string& password, std::string& message) {
    if (utf8Length(password) < 8) {
        message = "Mật khẩu phải dài ít nhất 8 ký tự.";
        return false;
    }
    if (password.size() > 256) {
        message = "Mật khẩu quá dài (tối đa 256 ký tự).";
        return false;
    }
    bool hasLetter = false, hasDigit = false;
    for (unsigned char c : password) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80) hasLetter = true;
        if (c >= '0' && c <= '9') hasDigit = true;
    }
    if (!hasLetter || !hasDigit) {
        message = "Mật khẩu nên có cả chữ và số.";
        return false;
    }
    return true;
}

UserManager::UserManager(db::Database& database, const Config& config)
    : db_(database), config_(config) {}

bool UserManager::ensureAdminExists(std::string& generatedPassword, std::string& error) {
    generatedPassword.clear();
    uint64_t count = 0;
    if (!db_.countUsers(count, error)) return false;
    if (count > 0) return true;

    // Sinh mật khẩu ngẫu nhiên dễ đọc.
    static const char* kAlphabet = "abcdefghijkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    size_t alphabetLen = std::strlen(kAlphabet);
    std::string password;
    for (int i = 0; i < 16; ++i) {
        password.push_back(kAlphabet[crypto::randomBelow(alphabetLen)]);
        if (i == 3 || i == 7 || i == 11) password.push_back('-');
    }

    db::UserEntry admin;
    admin.username = "admin";
    admin.displayName = "Quản trị viên";
    admin.passwordHash = hashPassword(password, config_.security.passwordIterations);
    admin.isAdmin = true;
    admin.enabled = true;
    admin.createdAt = nowUnix();
    if (!db_.createUser(admin, error)) return false;

    generatedPassword = password;
    LOG_INFO(kTag, "Đã tạo tài khoản quản trị đầu tiên: admin");
    return true;
}

AuthResult UserManager::login(const std::string& username, const std::string& password,
                              const std::string& userAgent, const std::string& ip) {
    AuthResult result;
    db::UserEntry user;
    std::string error;
    if (!db_.getUserByName(username, user, error)) {
        // Vẫn tính băm để thời gian phản hồi không tiết lộ tài khoản có tồn tại hay không.
        hashPassword(password, config_.security.passwordIterations);
        result.error = "Tên đăng nhập hoặc mật khẩu không đúng.";
        return result;
    }
    if (!user.enabled) {
        result.error = "Tài khoản đã bị khoá.";
        return result;
    }
    if (!verifyPassword(password, user.passwordHash)) {
        result.error = "Tên đăng nhập hoặc mật khẩu không đúng.";
        LOG_WARN(kTag, "Đăng nhập thất bại cho '%s' từ %s", username.c_str(), ip.c_str());
        return result;
    }

    db::WebSession session;
    session.token = crypto::randomToken(32);
    session.userId = user.id;
    session.createdAt = nowUnix();
    session.expiresAt = session.createdAt +
                        static_cast<int64_t>(config_.security.sessionDays) * 86400;
    session.userAgent = utf8TruncateBytes(userAgent, 500);
    session.ip = ip;
    if (!db_.createSession(session, error)) {
        result.error = "Không tạo được phiên đăng nhập: " + error;
        return result;
    }

    user.lastLoginAt = nowUnix();
    db_.updateUser(user, error);

    result.ok = true;
    result.user = user;
    result.token = session.token;
    result.expiresAt = session.expiresAt;
    LOG_INFO(kTag, "Người dùng '%s' đăng nhập từ %s", username.c_str(), ip.c_str());
    return result;
}

bool UserManager::logout(const std::string& token) {
    std::string error;
    return db_.deleteSession(token, error);
}

bool UserManager::authenticate(const std::string& token, db::UserEntry& out) {
    if (token.empty()) return false;
    db::WebSession session;
    std::string error;
    if (!db_.getSession(token, session, error)) return false;
    if (session.expiresAt < nowUnix()) {
        db_.deleteSession(token, error);
        return false;
    }
    if (!db_.getUser(session.userId, out, error)) return false;
    return out.enabled;
}

bool UserManager::authenticateBasic(const std::string& authorizationHeader,
                                    db::UserEntry& out) {
    if (!startsWith(toLower(authorizationHeader), "basic ")) return false;
    Bytes decoded = base64Decode(trim(authorizationHeader.substr(6)));
    std::string creds = bytesToString(decoded);
    size_t colon = creds.find(':');
    if (colon == std::string::npos) return false;
    std::string username = creds.substr(0, colon);
    std::string password = creds.substr(colon + 1);

    db::UserEntry user;
    std::string error;
    if (!db_.getUserByName(username, user, error)) return false;
    if (!user.enabled) return false;
    if (!verifyPassword(password, user.passwordHash)) return false;
    out = user;
    return true;
}

bool UserManager::createUser(const std::string& username, const std::string& password,
                             const std::string& displayName, bool isAdmin, uint64_t quota,
                             db::UserEntry& out, std::string& error) {
    std::string clean = trim(username);
    if (clean.size() < 3 || clean.size() > 64) {
        error = "Tên đăng nhập phải dài từ 3 đến 64 ký tự.";
        return false;
    }
    for (char c : clean) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) {
            error = "Tên đăng nhập chỉ được chứa chữ, số và các ký tự . _ -";
            return false;
        }
    }
    std::string strengthMessage;
    if (!checkPasswordStrength(password, strengthMessage)) {
        error = strengthMessage;
        return false;
    }
    db::UserEntry existing;
    std::string findError;
    if (db_.getUserByName(clean, existing, findError)) {
        error = "Tên đăng nhập '" + clean + "' đã tồn tại.";
        return false;
    }

    db::UserEntry user;
    user.username = clean;
    user.displayName = displayName.empty() ? clean : displayName;
    user.passwordHash = hashPassword(password, config_.security.passwordIterations);
    user.isAdmin = isAdmin;
    user.enabled = true;
    user.quotaBytes = quota;
    user.createdAt = nowUnix();
    if (!db_.createUser(user, error)) return false;
    out = user;
    LOG_INFO(kTag, "Đã tạo người dùng '%s'%s", clean.c_str(), isAdmin ? " (quản trị)" : "");
    return true;
}

bool UserManager::changePassword(int userId, const std::string& newPassword,
                                 std::string& error) {
    std::string strengthMessage;
    if (!checkPasswordStrength(newPassword, strengthMessage)) {
        error = strengthMessage;
        return false;
    }
    db::UserEntry user;
    if (!db_.getUser(userId, user, error)) return false;
    user.passwordHash = hashPassword(newPassword, config_.security.passwordIterations);
    if (!db_.updateUser(user, error)) return false;
    // Đăng xuất mọi phiên cũ để bảo đảm an toàn.
    std::string sessionError;
    db_.deleteSessionsOfUser(userId, sessionError);
    LOG_INFO(kTag, "Đã đổi mật khẩu cho '%s'", user.username.c_str());
    return true;
}

bool UserManager::setEnabled(int userId, bool enabled, std::string& error) {
    db::UserEntry user;
    if (!db_.getUser(userId, user, error)) return false;
    user.enabled = enabled;
    if (!db_.updateUser(user, error)) return false;
    if (!enabled) {
        std::string sessionError;
        db_.deleteSessionsOfUser(userId, sessionError);
    }
    return true;
}

bool UserManager::deleteUser(int userId, std::string& error) {
    db::UserEntry user;
    if (!db_.getUser(userId, user, error)) return false;
    if (user.isAdmin) {
        // Không cho xoá quản trị viên cuối cùng.
        std::vector<db::UserEntry> all;
        std::string listError;
        db_.listUsers(all, listError);
        int adminCount = 0;
        for (const auto& u : all)
            if (u.isAdmin) ++adminCount;
        if (adminCount <= 1) {
            error = "Không thể xoá quản trị viên cuối cùng.";
            return false;
        }
    }
    std::string sessionError;
    db_.deleteSessionsOfUser(userId, sessionError);
    return db_.deleteUser(userId, error);
}

bool UserManager::listUsers(std::vector<db::UserEntry>& out, std::string& error) {
    return db_.listUsers(out, error);
}

void UserManager::cleanupExpiredSessions() {
    std::string error;
    db_.deleteExpiredSessions(error);
}

}  // namespace app
}  // namespace ttd
