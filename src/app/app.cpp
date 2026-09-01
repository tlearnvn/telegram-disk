#include "app/app.h"

#include <chrono>
#include <cstdio>

#include "common/fsutil.h"
#include "common/logging.h"
#include "common/strutil.h"
#include "common/timeutil.h"
#include "http/api_routes.h"
#include "http/assets.h"
#include "http/webdav.h"
#include "tg/dc_config.h"
#include "version.h"

namespace ttd {
namespace app {

namespace {
constexpr const char* kTag = "app";

// Thông tin ứng dụng gửi kèm trong initConnection. Dựng ở một chỗ duy nhất để
// lúc khởi động và lúc lưu Cài đặt không bao giờ lệch nhau.
tg::AppInfo buildAppInfo(const Config& cfg) {
    tg::AppInfo info;
    info.apiId = cfg.telegram.apiId;
    info.apiHash = cfg.telegram.apiHash;
    info.deviceModel = cfg.telegram.deviceModel;
    info.systemVersion = cfg.telegram.systemVersion;
    info.appVersion = std::string(version::kVersion);
    info.langCode = cfg.telegram.langCode;
    info.systemLangCode = cfg.telegram.langCode;
    info.layer = cfg.telegram.layer;
    return info;
}
}  // namespace

App::App() = default;

App::~App() { stop(); }

bool App::initLogging(std::string& error) {
    Config& cfg = Config::instance();
    std::string logPath = cfg.logging.file.empty() ? "" : cfg.resolvePath(cfg.logging.file);
    Logger::instance().configure(parseLogLevel(cfg.logging.level), logPath,
                                 cfg.logging.maxFileBytes, cfg.logging.maxFiles,
                                 cfg.logging.console,
                                 static_cast<size_t>(cfg.logging.memoryRecords));
    LOG_INFO(kTag, "%s phiên bản %s (build %d, %s) — %s", version::kAppName, version::kVersion,
             version::kBuildNumber, version::kGitCommit, version::kAppFooter);
    LOG_INFO(kTag, "Múi giờ hệ thống: %s — hiện tại %s", kSystemTimezoneName,
             formatDateTime(nowUnix()).c_str());
    (void)error;
    return true;
}

bool App::initDatabase(std::string& error) {
    Config& cfg = Config::instance();
    db::DatabaseConfig dbc;
    dbc.kind = cfg.database.kind;
    dbc.sqlitePath = cfg.resolvePath(cfg.database.sqlitePath);
    dbc.mysqlHost = cfg.database.mysqlHost;
    dbc.mysqlPort = cfg.database.mysqlPort;
    dbc.mysqlUser = cfg.database.mysqlUser;
    dbc.mysqlPassword = cfg.database.mysqlPassword;
    dbc.mysqlDatabase = cfg.database.mysqlDatabase;

    db_ = db::createDatabase(dbc, error);
    if (!db_) return false;
    if (!db_->open(error)) {
        error = "Không mở được cơ sở dữ liệu (" + dbc.kind + "): " + error;
        return false;
    }
    if (!db_->migrate(error)) return false;
    LOG_INFO(kTag, "Cơ sở dữ liệu: %s", db_->description().c_str());
    return true;
}

bool App::initSchema(std::string& error) {
    Config& cfg = Config::instance();
    std::vector<std::string> warnings;

    std::string mtproto;
    if (!assets::find("schema/mtproto.tl", mtproto) || mtproto.empty()) {
        error = "Thiếu schema mtproto.tl trong tệp thực thi";
        return false;
    }
    schema_.load(mtproto, &warnings);

    // Ưu tiên tệp schema bên ngoài nếu có (cho phép nâng layer mà không cần biên dịch lại).
    std::string apiSchema;
    bool external = false;
    if (!cfg.telegram.schemaFile.empty()) {
        std::string path = cfg.resolvePath(cfg.telegram.schemaFile);
        if (readWholeFile(path, apiSchema) && !apiSchema.empty()) {
            external = true;
            LOG_INFO(kTag, "Dùng schema TL bên ngoài: %s", path.c_str());
        } else {
            LOG_WARN(kTag, "Không đọc được schema TL bên ngoài: %s — dùng bản đi kèm",
                     path.c_str());
        }
    }
    if (!external) {
        std::string candidate = cfg.resolvePath("schema/api.tl");
        if (readWholeFile(candidate, apiSchema) && !apiSchema.empty()) {
            external = true;
            LOG_INFO(kTag, "Dùng schema TL từ %s", candidate.c_str());
        }
    }
    if (!external && !assets::find("schema/api.tl", apiSchema)) {
        error = "Thiếu schema api.tl";
        return false;
    }
    schema_.load(apiSchema, &warnings);

    for (const auto& w : warnings) LOG_WARN(kTag, "Schema TL: %s", w.c_str());
    LOG_INFO(kTag, "Đã nạp %zu hàm dựng TL (layer %d)", schema_.size(), schema_.layer());

    // Kiểm tra các hàm dựng bắt buộc.
    static const char* kRequired[] = {"req_pq_multi",
                                      "req_DH_params",
                                      "set_client_DH_params",
                                      "invokeWithLayer",
                                      "initConnection",
                                      "help.getConfig",
                                      "upload.saveBigFilePart",
                                      "upload.getFile",
                                      "messages.sendMedia",
                                      "inputMediaUploadedDocument",
                                      "inputFileBig",
                                      "inputPeerChannel",
                                      "inputDocumentFileLocation",
                                      "channels.getMessages",
                                      "channels.deleteMessages",
                                      "auth.sendCode",
                                      "auth.signIn",
                                      "auth.checkPassword",
                                      "account.getPassword",
                                      nullptr};
    std::vector<std::string> missing;
    for (int i = 0; kRequired[i]; ++i)
        if (!schema_.byName(kRequired[i])) missing.push_back(kRequired[i]);
    if (!missing.empty()) {
        error = "Schema TL thiếu các hàm dựng bắt buộc: " + join(missing, ", ") +
                ". Hãy đặt tệp api.tl đầy đủ vào thư mục schema/.";
        return false;
    }

    if (schema_.layer() > 0) cfg.telegram.layer = schema_.layer();
    return true;
}

bool App::initBackend(std::string& error) {
    Config& cfg = Config::instance();
    tg::DcConfig::instance().loadDefaults();
    for (const auto& pem : cfg.telegram.extraRsaKeys) {
        if (!tg::DcConfig::instance().addPublicKeyPem(pem))
            LOG_WARN(kTag, "Bỏ qua một khoá RSA bổ sung không đọc được");
    }

    if (cfg.telegram.backend == "local") {
        localBackend_.reset(new tg::LocalBackend(cfg.resolvePath(cfg.telegram.localDirectory)));
        backend_ = localBackend_.get();
        LOG_WARN(kTag,
                 "Đang chạy ở CHẾ ĐỘ THỬ NGHIỆM — dữ liệu lưu trên đĩa máy này, "
                 "không đẩy lên Telegram.");
    } else {
        tg::AppInfo info = buildAppInfo(cfg);
        if (info.apiId == 0 || info.apiHash.empty()) {
            LOG_WARN(kTag,
                     "Chưa có api_id/api_hash — hãy vào Cài đặt điền trước khi thêm tài "
                     "khoản Telegram (lấy tại my.telegram.org).");
        }

        pool_.reset(new tg::AccountPool(schema_, info));
        pool_->setSessionPersist([this](int accountId, const std::map<int, tg::AuthKey>& keys) {
            persistAccountSessions(accountId, keys);
        });
        backend_ = pool_.get();

        if (cfg.telegram.channelId != 0) {
            tg::SupergroupRef group;
            group.channelId = cfg.telegram.channelId;
            group.accessHash = cfg.telegram.channelAccessHash;
            group.title = cfg.telegram.channelTitle;
            pool_->setSupergroup(group);
        }

        std::string reloadError;
        if (!reloadAccounts(reloadError))
            LOG_WARN(kTag, "Không nạp được tài khoản Telegram: %s", reloadError.c_str());
    }

    engine_.reset(new storage::StorageEngine(*db_, *backend_, cfg));
    uploads_.reset(new storage::UploadManager(*engine_, *db_, cfg));
    vfs_.reset(new Vfs(*db_, *engine_, cfg));
    (void)error;
    return true;
}

bool App::reloadAccounts(std::string& error) {
    if (!pool_) return true;
    Config& cfg = Config::instance();

    std::vector<db::AccountEntry> accounts;
    if (!db_->listAccounts(accounts, error)) return false;

    for (const auto& a : accounts) {
        tg::TgAccountConfig ac;
        ac.id = a.id;
        ac.label = a.label.empty() ? ("Tài khoản " + std::to_string(a.id)) : a.label;
        ac.phone = a.phone;
        ac.homeDc = a.homeDc > 0 ? a.homeDc : 2;
        ac.testMode = cfg.telegram.testMode;
        ac.obfuscated = cfg.telegram.obfuscated;
        ac.connectionsPerDc = cfg.telegram.connectionsPerAccount;
        ac.requestTimeoutMs = cfg.telegram.requestTimeoutSeconds * 1000;

        tg::TgAccount* account = pool_->addAccount(ac);
        if (!account) continue;
        pool_->setAccountEnabled(a.id, a.enabled);

        std::vector<db::SessionKeyEntry> keys;
        std::string keyError;
        if (db_->listSessionKeys(a.id, keys, keyError)) {
            for (const auto& k : keys) {
                Bytes authKey = fromHex(k.authKeyHex);
                if (authKey.size() == 256) account->loadSession(k.dcId, authKey, k.serverSalt);
            }
        }
    }
    LOG_INFO(kTag, "Đã nạp %zu tài khoản Telegram", accounts.size());
    return true;
}

void App::persistAccountSessions(int accountId, const std::map<int, tg::AuthKey>& keys) {
    if (!db_) return;
    for (const auto& kv : keys) {
        if (!kv.second.valid()) continue;
        db::SessionKeyEntry entry;
        entry.accountId = accountId;
        entry.dcId = kv.first;
        entry.authKeyHex = toHex(kv.second.key);
        entry.serverSalt = kv.second.serverSalt;
        entry.updatedAt = nowUnix();
        std::string error;
        if (!db_->saveSessionKey(entry, error))
            LOG_WARN(kTag, "Không lưu được phiên tài khoản #%d DC%d: %s", accountId, kv.first,
                     error.c_str());
    }
}

bool App::initServer(std::string& error) {
    Config& cfg = Config::instance();
    http::ServerOptions opts;
    opts.bindAddress = cfg.server.bindAddress;
    opts.port = cfg.server.port;
    opts.workerThreads = cfg.server.workerThreads;
    opts.idleTimeoutSeconds = cfg.server.idleTimeoutSeconds;
    opts.maxInlineBodyBytes = static_cast<uint64_t>(cfg.server.maxRequestBodyMb) * 1024 * 1024;
    opts.logRequests = cfg.logging.logRequests;

    server_.reset(new http::HttpServer(opts));
    http::registerApiRoutes(*server_, *this);
    if (cfg.server.enableWebdav) http::registerWebdavRoutes(*server_, *this);
    http::registerStaticRoutes(*server_, *this);

    if (!server_->start(error)) return false;

    std::vector<std::string> ips = net::localIpAddresses();
    std::string hint = "http://127.0.0.1:" + std::to_string(cfg.server.port);
    for (const auto& ip : ips) hint += "  |  http://" + ip + ":" + std::to_string(cfg.server.port);
    LOG_INFO(kTag, "Mở trình duyệt tại: %s", hint.c_str());
    if (cfg.server.enableWebdav)
        LOG_INFO(kTag, "WebDAV sẵn sàng tại %s%s", hint.substr(0, hint.find("  |")).c_str(),
                 cfg.server.webdavPrefix.c_str());
    return true;
}

bool App::start(const std::string& configPath, std::string& error) {
    startedAt_ = nowUnix();
    Config& cfg = Config::instance();

    // Thư mục dữ liệu mặc định nằm cạnh tệp thực thi.
    std::string exeDir = executableDirectory();
    cfg.setDataRoot(exeDir.empty() ? currentWorkingDirectory() : exeDir);

    std::string resolvedConfig = configPath.empty() ? cfg.resolvePath("config.json")
                                                    : configPath;
    if (pathExists(resolvedConfig)) {
        if (!cfg.loadFromFile(resolvedConfig, error)) return false;
    } else {
        cfg.setPath(resolvedConfig);
        std::string saveError;
        if (!cfg.saveToFile(resolvedConfig, saveError))
            std::fprintf(stderr, "Cảnh báo: không tạo được tệp cấu hình mặc định: %s\n",
                         saveError.c_str());
    }

    if (!initLogging(error)) return false;
    if (!pathExists(resolvedConfig))
        LOG_INFO(kTag, "Đã tạo tệp cấu hình mặc định tại %s", resolvedConfig.c_str());
    else
        LOG_INFO(kTag, "Đã nạp cấu hình từ %s", resolvedConfig.c_str());

    ensureDirectoryExists(cfg.resolvePath("data"));
    ensureDirectoryExists(cfg.resolvePath(cfg.storage.spoolDirectory));
    ensureDirectoryExists(cfg.resolvePath(cfg.storage.downloadCacheDirectory));

    // Cho phép phát triển giao diện mà không cần biên dịch lại.
    std::string webDir = cfg.resolvePath("web");
    if (isDirectory(webDir)) assets::setOverrideDirectory(webDir);

    if (!initDatabase(error)) return false;
    if (!initSchema(error)) return false;

    users_.reset(new UserManager(*db_, cfg));
    if (!users_->ensureAdminExists(adminBootstrapPassword_, error)) return false;
    if (!adminBootstrapPassword_.empty()) {
        LOG_WARN(kTag, "==============================================================");
        LOG_WARN(kTag, " Tài khoản quản trị đầu tiên đã được tạo:");
        LOG_WARN(kTag, "   Tên đăng nhập: admin");
        LOG_WARN(kTag, "   Mật khẩu     : %s", adminBootstrapPassword_.c_str());
        LOG_WARN(kTag, " Hãy đăng nhập và đổi mật khẩu ngay.");
        LOG_WARN(kTag, "==============================================================");
    }

    if (!initBackend(error)) return false;
    if (!initServer(error)) return false;

    running_.store(true);
    backgroundThread_ = std::thread([this]() { backgroundLoop(); });

    if (pool_) {
        // Kết nối tài khoản ở luồng nền để không làm chậm lúc khởi động.
        std::thread([this]() {
            pool_->connectAll();
            std::string error;
            tg::TgAccount* first = nullptr;
            for (const auto& s : pool_->statuses()) {
                first = pool_->findAccount(s.id);
                if (first && first->authorized()) break;
                first = nullptr;
            }
            if (first) first->refreshDcConfig(error);
        }).detach();
    }
    return true;
}

void App::backgroundLoop() {
    int64_t lastReap = 0;
    int64_t lastTrash = 0;
    int64_t lastPersist = 0;
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!running_.load()) break;
        int64_t now = nowUnix();

        if (now - lastReap >= 30) {
            lastReap = now;
            if (uploads_) uploads_->reapStale();
            if (users_) users_->cleanupExpiredSessions();
        }
        if (now - lastTrash >= 3600) {
            lastTrash = now;
            if (vfs_) vfs_->emptyExpiredTrash();
        }
        if (now - lastPersist >= 300) {
            lastPersist = now;
            if (pool_) pool_->persistAll();
        }
    }
}

void App::stop() {
    if (!running_.exchange(false)) return;
    LOG_INFO(kTag, "Đang dừng ứng dụng…");
    if (backgroundThread_.joinable()) backgroundThread_.join();
    if (server_) server_->stop();
    if (pool_) pool_->disconnectAll();
    uploads_.reset();
    engine_.reset();
    vfs_.reset();
    users_.reset();
    if (db_) db_->close();
    LOG_INFO(kTag, "Đã dừng. Tạm biệt!");
}

void App::waitForShutdown() {
    while (running_.load() && !shutdownRequested_.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop();
}

// ---------------------------------------------------------------------------
//  Tài khoản Telegram
// ---------------------------------------------------------------------------
bool App::addAccountAndSendCode(const std::string& label, const std::string& phone,
                                int& accountIdOut, tg::LoginResult& result,
                                std::string& error) {
    if (!pool_) {
        error = "Đang chạy ở chế độ thử nghiệm — không dùng tài khoản Telegram.";
        return false;
    }
    Config& cfg = Config::instance();
    if (cfg.telegram.apiId == 0 || cfg.telegram.apiHash.empty()) {
        error =
            "Chưa cấu hình api_id và api_hash. Hãy tạo ứng dụng tại my.telegram.org rồi điền "
            "vào phần Cài đặt → Telegram.";
        return false;
    }
    // Đồng bộ lại lần nữa cho chắc: tài khoản mới phải mang đúng api_id đang
    // có trong cấu hình, kể cả khi cấu hình vừa đổi qua một đường khác.
    {
        tg::AppInfo info = buildAppInfo(cfg);
        if (info.apiId != pool_->appInfo().apiId ||
            info.apiHash != pool_->appInfo().apiHash) {
            pool_->updateAppInfo(info);
        }
    }

    db::AccountEntry entry;
    entry.label = label.empty() ? phone : label;
    entry.phone = phone;
    entry.enabled = true;
    entry.homeDc = 2;
    entry.createdAt = nowUnix();
    if (!db_->createAccount(entry, error)) return false;
    accountIdOut = entry.id;

    tg::TgAccountConfig ac;
    ac.id = entry.id;
    ac.label = entry.label;
    ac.phone = phone;
    ac.homeDc = 2;
    ac.testMode = cfg.telegram.testMode;
    ac.obfuscated = cfg.telegram.obfuscated;
    ac.requestTimeoutMs = cfg.telegram.requestTimeoutSeconds * 1000;
    tg::TgAccount* account = pool_->addAccount(ac);
    // In ra api_id đang thực sự dùng để dễ đối chiếu với my.telegram.org khi
    // Telegram từ chối. api_hash là bí mật nên chỉ ghi độ dài.
    if (account) {
        LOG_INFO(kTag, "[%s] Đăng nhập với api_id %d (api_hash %zu ký tự), layer %d",
                 ac.label.c_str(), account->appApiId(), account->appApiHash().size(),
                 cfg.telegram.layer);
    }
    if (!account) {
        error = "Không tạo được đối tượng tài khoản";
        return false;
    }

    result = tg::loginSendCode(*account, phone);
    if (!result.ok) {
        error = result.message;
        // Dọn tài khoản vừa tạo để danh sách không bị rác.
        std::string deleteError;
        db_->deleteAccount(entry.id, deleteError);
        pool_->removeAccount(entry.id);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        PendingLogin p;
        p.accountId = entry.id;
        p.state = result.state;
        p.createdAt = nowUnix();
        pendingLogins_[entry.id] = p;
    }
    persistAccountSessions(entry.id, account->exportSessions());
    return true;
}

bool App::submitAccountCode(int accountId, const std::string& code, tg::LoginResult& result,
                            std::string& error) {
    if (!pool_) {
        error = "Không có tài khoản Telegram trong chế độ thử nghiệm.";
        return false;
    }
    tg::TgAccount* account = pool_->findAccount(accountId);
    if (!account) {
        error = "Không tìm thấy tài khoản #" + std::to_string(accountId);
        return false;
    }
    tg::LoginState state;
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        auto it = pendingLogins_.find(accountId);
        if (it == pendingLogins_.end()) {
            error = "Phiên đăng nhập đã hết hạn. Hãy gửi lại mã xác thực.";
            return false;
        }
        state = it->second.state;
    }

    result = tg::loginSubmitCode(*account, state, code);
    persistAccountSessions(accountId, account->exportSessions());

    if (result.needsPassword) {
        std::lock_guard<std::mutex> lk(pendingMu_);
        auto it = pendingLogins_.find(accountId);
        if (it != pendingLogins_.end()) it->second.state = result.state;
        return true;
    }
    if (!result.ok) {
        error = result.message;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        pendingLogins_.erase(accountId);
    }
    std::vector<db::AccountEntry> accounts;
    std::string listError;
    if (db_->listAccounts(accounts, listError)) {
        for (auto& a : accounts) {
            if (a.id != accountId) continue;
            a.displayName = result.displayName;
            a.homeDc = account->config().homeDc;
            a.lastUsedAt = nowUnix();
            std::string updateError;
            db_->updateAccount(a, updateError);
        }
    }
    return true;
}

bool App::submitAccountPassword(int accountId, const std::string& password,
                                tg::LoginResult& result, std::string& error) {
    if (!pool_) {
        error = "Không có tài khoản Telegram trong chế độ thử nghiệm.";
        return false;
    }
    tg::TgAccount* account = pool_->findAccount(accountId);
    if (!account) {
        error = "Không tìm thấy tài khoản #" + std::to_string(accountId);
        return false;
    }
    result = tg::loginSubmitPassword(*account, password);
    persistAccountSessions(accountId, account->exportSessions());
    if (!result.ok) {
        error = result.message;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        pendingLogins_.erase(accountId);
    }
    std::vector<db::AccountEntry> accounts;
    std::string listError;
    if (db_->listAccounts(accounts, listError)) {
        for (auto& a : accounts) {
            if (a.id != accountId) continue;
            a.displayName = result.displayName;
            a.homeDc = account->config().homeDc;
            a.lastUsedAt = nowUnix();
            std::string updateError;
            db_->updateAccount(a, updateError);
        }
    }
    return true;
}

bool App::removeAccount(int accountId, std::string& error) {
    if (pool_) {
        tg::TgAccount* account = pool_->findAccount(accountId);
        if (account) {
            std::string logoutError;
            tg::logout(*account, logoutError);
        }
        pool_->removeAccount(accountId);
    }
    std::string keyError;
    db_->deleteSessionKeys(accountId, keyError);
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        pendingLogins_.erase(accountId);
    }
    return db_->deleteAccount(accountId, error);
}

bool App::setSupergroup(const std::string& usernameOrId, std::string& error,
                        std::string& resolvedTitle) {
    if (!pool_) {
        error = "Chế độ thử nghiệm không dùng siêu nhóm Telegram.";
        return false;
    }
    tg::TgAccount* account = nullptr;
    for (const auto& s : pool_->statuses()) {
        if (!s.enabled || !s.authorized) continue;
        account = pool_->findAccount(s.id);
        if (account) break;
    }
    if (!account) {
        error = "Chưa có tài khoản Telegram nào đăng nhập.";
        return false;
    }

    tg::SupergroupRef group;
    std::string input = trim(usernameOrId);
    bool numeric = !input.empty();
    for (char c : input)
        if (c != '-' && (c < '0' || c > '9')) numeric = false;

    bool ok = false;
    if (numeric) {
        int64_t id = 0;
        parseInt64(input, id);
        if (id < 0) id = -id;
        // Telegram hiển thị id kênh dạng -100xxxxxxxxxx.
        std::string s = std::to_string(id);
        if (startsWith(s, "100") && s.size() > 3) parseInt64(s.substr(3), id);
        ok = account->resolveSupergroupById(id, 0, group, error);
    } else {
        ok = account->resolveSupergroupByUsername(input, group, error);
    }
    if (!ok) return false;

    pool_->setSupergroup(group);
    Config& cfg = Config::instance();
    {
        std::lock_guard<std::mutex> lk(cfg.mutex);
        cfg.telegram.channelId = group.channelId;
        cfg.telegram.channelAccessHash = group.accessHash;
        cfg.telegram.channelTitle = group.title;
        cfg.telegram.channelUsername = numeric ? "" : input;
    }
    std::string saveError;
    cfg.saveToFile(cfg.path(), saveError);
    resolvedTitle = group.title;
    LOG_INFO(kTag, "Siêu nhóm lưu trữ: %s (id %lld)", group.title.c_str(),
             static_cast<long long>(group.channelId));
    return true;
}

bool App::applySettings(const Json& settings, std::string& error) {
    Config& cfg = Config::instance();
    if (!cfg.applyJson(settings, error)) return false;
    if (!cfg.saveToFile(cfg.path(), error)) return false;

    // Áp dụng ngay những thứ có thể đổi nóng.
    Logger::instance().setLevel(parseLogLevel(cfg.logging.level));
    if (engine_) engine_->cache().setCapacity(cfg.storage.downloadCacheBytes);
    ensureDirectoryExists(cfg.resolvePath(cfg.storage.spoolDirectory));
    ensureDirectoryExists(cfg.resolvePath(cfg.storage.downloadCacheDirectory));

    // api_id / api_hash thường được điền sau khi ứng dụng đã chạy. Nếu không
    // đẩy xuống pool ở đây thì mọi tài khoản vẫn gửi api_id cũ (thường là 0)
    // trong initConnection và Telegram trả về CONNECTION_API_ID_INVALID.
    if (pool_) {
        tg::AppInfo info = buildAppInfo(cfg);
        tg::AppInfo current = pool_->appInfo();
        if (info.apiId != current.apiId || info.apiHash != current.apiHash ||
            info.deviceModel != current.deviceModel ||
            info.systemVersion != current.systemVersion || info.langCode != current.langCode ||
            info.systemLangCode != current.systemLangCode || info.layer != current.layer) {
            pool_->updateAppInfo(info);
        }
    }
    LOG_INFO(kTag, "Đã áp dụng cấu hình mới (mảnh %s, đệm %s, nhật ký %s)",
             formatBytes(cfg.storage.chunkSize).c_str(), cfg.storage.bufferMode.c_str(),
             cfg.logging.level.c_str());
    return true;
}

}  // namespace app
}  // namespace ttd
