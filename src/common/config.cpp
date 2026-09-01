#include "common/config.h"

#include "common/fsutil.h"
#include "common/logging.h"
#include "common/strutil.h"

namespace ttd {

namespace {
constexpr const char* kTag = "config";

std::string getStr(const Json& j, const std::string& key, const std::string& def) {
    const Json& v = j[key];
    return v.isNull() ? def : v.asString(def);
}
int64_t getInt(const Json& j, const std::string& key, int64_t def) {
    const Json& v = j[key];
    return v.isNull() ? def : v.asInt64(def);
}
uint64_t getSize(const Json& j, const std::string& key, uint64_t def) {
    const Json& v = j[key];
    if (v.isNull()) return def;
    if (v.isString()) return parseSizeString(v.asString(), def);
    return v.asUInt64(def);
}
bool getBool(const Json& j, const std::string& key, bool def) {
    const Json& v = j[key];
    return v.isNull() ? def : v.asBool(def);
}
}  // namespace

Config& Config::instance() {
    static Config inst;
    return inst;
}

std::string Config::resolvePath(const std::string& relative) const {
    if (relative.empty()) return relative;
#if defined(_WIN32)
    if (relative.size() >= 2 && relative[1] == ':') return relative;
#endif
    if (relative[0] == '/' || relative[0] == '\\') return relative;
    return joinPath(dataRoot_, relative);
}

bool Config::applyJson(const Json& root, std::string& error) {
    std::lock_guard<std::mutex> lk(mutex);

    if (root.has("server")) {
        const Json& j = root["server"];
        server.bindAddress = getStr(j, "bind_address", server.bindAddress);
        server.port = static_cast<uint16_t>(getInt(j, "port", server.port));
        server.workerThreads = static_cast<int>(getInt(j, "worker_threads", server.workerThreads));
        server.maxRequestBodyMb =
            static_cast<int>(getInt(j, "max_request_body_mb", server.maxRequestBodyMb));
        server.idleTimeoutSeconds =
            static_cast<int>(getInt(j, "idle_timeout_seconds", server.idleTimeoutSeconds));
        server.publicUrl = getStr(j, "public_url", server.publicUrl);
        server.enableWebdav = getBool(j, "enable_webdav", server.enableWebdav);
        server.webdavPrefix = getStr(j, "webdav_prefix", server.webdavPrefix);
        server.trustProxyHeaders = getBool(j, "trust_proxy_headers", server.trustProxyHeaders);
    }

    if (root.has("storage")) {
        const Json& j = root["storage"];
        storage.chunkSize = getSize(j, "chunk_size", storage.chunkSize);
        storage.bufferMode = toLower(getStr(j, "buffer_mode", storage.bufferMode));
        storage.memoryBudget = getSize(j, "memory_budget", storage.memoryBudget);
        storage.spoolDirectory = getStr(j, "spool_directory", storage.spoolDirectory);
        storage.browserChunkSize = getSize(j, "browser_chunk_size", storage.browserChunkSize);
        storage.parallelChunks = static_cast<int>(getInt(j, "parallel_chunks",
                                                         storage.parallelChunks));
        storage.downloadCacheBytes = getSize(j, "download_cache_bytes",
                                             storage.downloadCacheBytes);
        storage.uploadIdleTimeoutSeconds = static_cast<int>(
            getInt(j, "upload_idle_timeout_seconds", storage.uploadIdleTimeoutSeconds));
        storage.trashRetentionDays =
            static_cast<int>(getInt(j, "trash_retention_days", storage.trashRetentionDays));
        storage.deduplicate = getBool(j, "deduplicate", storage.deduplicate);
    }

    if (root.has("telegram")) {
        const Json& j = root["telegram"];
        telegram.apiId = static_cast<int32_t>(getInt(j, "api_id", telegram.apiId));
        telegram.apiHash = getStr(j, "api_hash", telegram.apiHash);
        telegram.deviceModel = getStr(j, "device_model", telegram.deviceModel);
        telegram.systemVersion = getStr(j, "system_version", telegram.systemVersion);
        telegram.appVersion = getStr(j, "app_version", telegram.appVersion);
        telegram.langCode = getStr(j, "lang_code", telegram.langCode);
        telegram.layer = static_cast<int>(getInt(j, "layer", telegram.layer));
        telegram.testMode = getBool(j, "test_mode", telegram.testMode);
        telegram.obfuscated = getBool(j, "obfuscated", telegram.obfuscated);
        telegram.connectionsPerAccount = static_cast<int>(
            getInt(j, "connections_per_account", telegram.connectionsPerAccount));
        telegram.requestTimeoutSeconds = static_cast<int>(
            getInt(j, "request_timeout_seconds", telegram.requestTimeoutSeconds));
        telegram.channelId = getInt(j, "channel_id", telegram.channelId);
        telegram.channelAccessHash = getInt(j, "channel_access_hash",
                                            telegram.channelAccessHash);
        telegram.channelTitle = getStr(j, "channel_title", telegram.channelTitle);
        telegram.channelUsername = getStr(j, "channel_username", telegram.channelUsername);
        telegram.backend = toLower(getStr(j, "backend", telegram.backend));
        telegram.localDirectory = getStr(j, "local_directory", telegram.localDirectory);
        telegram.schemaFile = getStr(j, "schema_file", telegram.schemaFile);
        if (j.has("extra_rsa_keys") && j["extra_rsa_keys"].isArray()) {
            telegram.extraRsaKeys.clear();
            for (const auto& k : j["extra_rsa_keys"].arr())
                telegram.extraRsaKeys.push_back(k.asString());
        }
    }

    if (root.has("database")) {
        const Json& j = root["database"];
        database.kind = toLower(getStr(j, "kind", database.kind));
        database.sqlitePath = getStr(j, "sqlite_path", database.sqlitePath);
        database.mysqlHost = getStr(j, "mysql_host", database.mysqlHost);
        database.mysqlPort = static_cast<uint16_t>(getInt(j, "mysql_port", database.mysqlPort));
        database.mysqlUser = getStr(j, "mysql_user", database.mysqlUser);
        database.mysqlPassword = getStr(j, "mysql_password", database.mysqlPassword);
        database.mysqlDatabase = getStr(j, "mysql_database", database.mysqlDatabase);
    }

    if (root.has("logging")) {
        const Json& j = root["logging"];
        logging.level = toLower(getStr(j, "level", logging.level));
        logging.console = getBool(j, "console", logging.console);
        logging.file = getStr(j, "file", logging.file);
        logging.maxFileBytes = getSize(j, "max_file_bytes", logging.maxFileBytes);
        logging.maxFiles = static_cast<int>(getInt(j, "max_files", logging.maxFiles));
        logging.memoryRecords = static_cast<int>(getInt(j, "memory_records",
                                                        logging.memoryRecords));
        logging.logRequests = getBool(j, "log_requests", logging.logRequests);
    }

    if (root.has("security")) {
        const Json& j = root["security"];
        security.sessionDays = static_cast<int>(getInt(j, "session_days", security.sessionDays));
        security.passwordIterations =
            static_cast<int>(getInt(j, "password_iterations", security.passwordIterations));
        security.allowRegistration = getBool(j, "allow_registration",
                                             security.allowRegistration);
        security.publicShareLinks = getBool(j, "public_share_links",
                                            security.publicShareLinks);
    }

    // Kiểm tra và siết các giá trị bất hợp lý.
    if (server.port == 0) server.port = 8088;
    if (server.workerThreads < 2) server.workerThreads = 2;
    if (server.workerThreads > 512) server.workerThreads = 512;
    if (storage.chunkSize < 1024ull * 1024) {
        storage.chunkSize = 1024ull * 1024;
        LOG_WARN(kTag, "Kích thước mảnh quá nhỏ, đã nâng lên 1 MB");
    }
    // Telegram giới hạn mỗi tệp 2000 MB với tài khoản thường.
    if (storage.chunkSize > 1900ull * 1024 * 1024) {
        storage.chunkSize = 1900ull * 1024 * 1024;
        LOG_WARN(kTag, "Kích thước mảnh vượt giới hạn Telegram, đã hạ xuống 1900 MB");
    }
    if (storage.browserChunkSize < 256ull * 1024) storage.browserChunkSize = 256ull * 1024;
    if (storage.browserChunkSize > 128ull * 1024 * 1024)
        storage.browserChunkSize = 128ull * 1024 * 1024;
    if (storage.parallelChunks < 1) storage.parallelChunks = 1;
    if (storage.parallelChunks > 16) storage.parallelChunks = 16;
    if (storage.bufferMode != "stream" && storage.bufferMode != "memory" &&
        storage.bufferMode != "disk") {
        LOG_WARN(kTag, "Chế độ đệm '%s' không hợp lệ, dùng 'stream'",
                 storage.bufferMode.c_str());
        storage.bufferMode = "stream";
    }
    if (security.passwordIterations < 10000) security.passwordIterations = 10000;
    if (security.sessionDays < 1) security.sessionDays = 1;
    if (telegram.layer < 100) telegram.layer = 158;

    (void)error;
    return true;
}

bool Config::loadFromFile(const std::string& file, std::string& error) {
    path_ = file;
    std::string content;
    if (!readWholeFile(file, content)) {
        error = "Không đọc được tệp cấu hình: " + file;
        return false;
    }
    std::string parseError;
    Json root = Json::parse(content, &parseError);
    if (!parseError.empty()) {
        error = "Tệp cấu hình sai định dạng JSON: " + parseError;
        return false;
    }
    return applyJson(root, error);
}

Json Config::toJson() const {
    std::lock_guard<std::mutex> lk(mutex);
    Json root = Json::object();

    Json s = Json::object();
    s.set("bind_address", server.bindAddress);
    s.set("port", static_cast<int64_t>(server.port));
    s.set("worker_threads", static_cast<int64_t>(server.workerThreads));
    s.set("max_request_body_mb", static_cast<int64_t>(server.maxRequestBodyMb));
    s.set("idle_timeout_seconds", static_cast<int64_t>(server.idleTimeoutSeconds));
    s.set("public_url", server.publicUrl);
    s.set("enable_webdav", server.enableWebdav);
    s.set("webdav_prefix", server.webdavPrefix);
    s.set("trust_proxy_headers", server.trustProxyHeaders);
    root.set("server", s);

    Json st = Json::object();
    st.set("chunk_size", storage.chunkSize);
    st.set("buffer_mode", storage.bufferMode);
    st.set("memory_budget", storage.memoryBudget);
    st.set("spool_directory", storage.spoolDirectory);
    st.set("browser_chunk_size", storage.browserChunkSize);
    st.set("parallel_chunks", static_cast<int64_t>(storage.parallelChunks));
    st.set("download_cache_bytes", storage.downloadCacheBytes);
    st.set("upload_idle_timeout_seconds",
           static_cast<int64_t>(storage.uploadIdleTimeoutSeconds));
    st.set("trash_retention_days", static_cast<int64_t>(storage.trashRetentionDays));
    st.set("deduplicate", storage.deduplicate);
    root.set("storage", st);

    Json tg = Json::object();
    tg.set("api_id", static_cast<int64_t>(telegram.apiId));
    tg.set("api_hash", telegram.apiHash);
    tg.set("device_model", telegram.deviceModel);
    tg.set("system_version", telegram.systemVersion);
    tg.set("app_version", telegram.appVersion);
    tg.set("lang_code", telegram.langCode);
    tg.set("layer", static_cast<int64_t>(telegram.layer));
    tg.set("test_mode", telegram.testMode);
    tg.set("obfuscated", telegram.obfuscated);
    tg.set("connections_per_account", static_cast<int64_t>(telegram.connectionsPerAccount));
    tg.set("request_timeout_seconds", static_cast<int64_t>(telegram.requestTimeoutSeconds));
    tg.set("channel_id", telegram.channelId);
    tg.set("channel_access_hash", telegram.channelAccessHash);
    tg.set("channel_title", telegram.channelTitle);
    tg.set("channel_username", telegram.channelUsername);
    tg.set("backend", telegram.backend);
    tg.set("local_directory", telegram.localDirectory);
    tg.set("schema_file", telegram.schemaFile);
    Json keys = Json::array();
    for (const auto& k : telegram.extraRsaKeys) keys.push(Json(k));
    tg.set("extra_rsa_keys", keys);
    root.set("telegram", tg);

    Json db = Json::object();
    db.set("kind", database.kind);
    db.set("sqlite_path", database.sqlitePath);
    db.set("mysql_host", database.mysqlHost);
    db.set("mysql_port", static_cast<int64_t>(database.mysqlPort));
    db.set("mysql_user", database.mysqlUser);
    db.set("mysql_password", database.mysqlPassword);
    db.set("mysql_database", database.mysqlDatabase);
    root.set("database", db);

    Json lg = Json::object();
    lg.set("level", logging.level);
    lg.set("console", logging.console);
    lg.set("file", logging.file);
    lg.set("max_file_bytes", logging.maxFileBytes);
    lg.set("max_files", static_cast<int64_t>(logging.maxFiles));
    lg.set("memory_records", static_cast<int64_t>(logging.memoryRecords));
    lg.set("log_requests", logging.logRequests);
    root.set("logging", lg);

    Json sec = Json::object();
    sec.set("session_days", static_cast<int64_t>(security.sessionDays));
    sec.set("password_iterations", static_cast<int64_t>(security.passwordIterations));
    sec.set("allow_registration", security.allowRegistration);
    sec.set("public_share_links", security.publicShareLinks);
    root.set("security", sec);

    return root;
}

bool Config::saveToFile(const std::string& file, std::string& error) const {
    std::string content = toJson().dump(2);
    content += "\n";
    if (!writeWholeFileAtomic(file, content)) {
        error = "Không ghi được tệp cấu hình: " + file;
        return false;
    }
    LOG_INFO(kTag, "Đã lưu cấu hình vào %s", file.c_str());
    return true;
}

}  // namespace ttd
