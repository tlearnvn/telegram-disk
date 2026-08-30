// Đối tượng ứng dụng: gom cấu hình, cơ sở dữ liệu, nơi lưu trữ, máy chủ web
// và các tác vụ nền vào một chỗ.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/users.h"
#include "app/vfs.h"
#include "common/config.h"
#include "db/database.h"
#include "http/http_server.h"
#include "storage/storage_engine.h"
#include "storage/upload_manager.h"
#include "tg/account_pool.h"
#include "tg/backend_local.h"
#include "tg/tg_login.h"
#include "tg/tl_schema.h"

namespace ttd {
namespace app {

// Trạng thái đăng nhập Telegram đang dở dang (chờ nhập mã / mật khẩu).
struct PendingLogin {
    int accountId = 0;
    tg::LoginState state;
    int64_t createdAt = 0;
};

class App {
public:
    App();
    ~App();

    // Khởi động toàn bộ: cấu hình -> nhật ký -> cơ sở dữ liệu -> nơi lưu -> máy chủ web.
    bool start(const std::string& configPath, std::string& error);
    void stop();
    void waitForShutdown();
    void requestShutdown() { shutdownRequested_.store(true); }

    Config& config() { return Config::instance(); }
    db::Database* database() { return db_.get(); }
    storage::StorageEngine* engine() { return engine_.get(); }
    storage::UploadManager* uploads() { return uploads_.get(); }
    UserManager* users() { return users_.get(); }
    Vfs* vfs() { return vfs_.get(); }
    tg::AccountPool* pool() { return pool_.get(); }
    http::HttpServer* server() { return server_.get(); }
    const tg::TlSchema& schema() const { return schema_; }
    int64_t startedAt() const { return startedAt_; }

    // Quản lý tài khoản Telegram.
    bool reloadAccounts(std::string& error);
    bool addAccountAndSendCode(const std::string& label, const std::string& phone,
                               int& accountIdOut, tg::LoginResult& result, std::string& error);
    bool submitAccountCode(int accountId, const std::string& code, tg::LoginResult& result,
                           std::string& error);
    bool submitAccountPassword(int accountId, const std::string& password,
                               tg::LoginResult& result, std::string& error);
    bool removeAccount(int accountId, std::string& error);
    void persistAccountSessions(int accountId, const std::map<int, tg::AuthKey>& keys);

    // Áp dụng lại cấu hình lưu trữ (khi quản trị viên đổi trong giao diện).
    bool applySettings(const Json& settings, std::string& error);
    // Thiết lập siêu nhóm lưu trữ.
    bool setSupergroup(const std::string& usernameOrId, std::string& error,
                       std::string& resolvedTitle);

    std::string adminBootstrapPassword() const { return adminBootstrapPassword_; }

private:
    bool initLogging(std::string& error);
    bool initDatabase(std::string& error);
    bool initSchema(std::string& error);
    bool initBackend(std::string& error);
    bool initServer(std::string& error);
    void backgroundLoop();

    tg::TlSchema schema_;
    std::unique_ptr<db::Database> db_;
    std::unique_ptr<tg::AccountPool> pool_;
    std::unique_ptr<tg::LocalBackend> localBackend_;
    tg::StorageBackend* backend_ = nullptr;
    std::unique_ptr<storage::StorageEngine> engine_;
    std::unique_ptr<storage::UploadManager> uploads_;
    std::unique_ptr<UserManager> users_;
    std::unique_ptr<Vfs> vfs_;
    std::unique_ptr<http::HttpServer> server_;

    std::thread backgroundThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdownRequested_{false};
    int64_t startedAt_ = 0;
    std::string adminBootstrapPassword_;

    std::mutex pendingMu_;
    std::map<int, PendingLogin> pendingLogins_;
};

}  // namespace app
}  // namespace ttd
