#include "db/mysql_database.h"

#include "common/logging.h"
#include "common/strutil.h"
#include "common/timeutil.h"

namespace ttd {
namespace db {

namespace {
constexpr const char* kTag = "db.mysql";

std::string Q(const std::string& s) { return MysqlConnection::quote(s); }
std::string N(int64_t v) { return std::to_string(v); }
std::string N(uint64_t v) { return std::to_string(v); }
std::string B(bool v) { return v ? "1" : "0"; }

int64_t toInt(const std::string& s) {
    int64_t v = 0;
    parseInt64(s, v);
    return v;
}
uint64_t toUInt(const std::string& s) {
    uint64_t v = 0;
    parseUInt64(s, v);
    return v;
}

const char* kEntryColumns =
    "id, parent_id, name, path, is_folder, size, mime_type, sha256, quick_hash, chunk_size, "
    "chunk_count, created_at, modified_at, owner_id, trashed, trashed_at, share_token, "
    "share_expires_at, starred, note";

void readEntry(const MysqlRow& r, FileEntry& e) {
    e.id = toInt(r.at(0));
    e.parentId = toInt(r.at(1));
    e.name = r.at(2);
    e.path = r.at(3);
    e.isFolder = toInt(r.at(4)) != 0;
    e.size = toUInt(r.at(5));
    e.mimeType = r.at(6);
    e.sha256 = r.at(7);
    e.quickHash = r.at(8);
    e.chunkSize = toUInt(r.at(9));
    e.chunkCount = static_cast<int>(toInt(r.at(10)));
    e.createdAt = toInt(r.at(11));
    e.modifiedAt = toInt(r.at(12));
    e.ownerId = static_cast<int>(toInt(r.at(13)));
    e.trashed = toInt(r.at(14)) != 0;
    e.trashedAt = toInt(r.at(15));
    e.shareToken = r.at(16);
    e.shareExpiresAt = toInt(r.at(17));
    e.starred = toInt(r.at(18)) != 0;
    e.note = r.at(19);
}

std::string orderByClause(const std::string& sortBy, bool desc) {
    std::string col;
    if (sortBy == "size") col = "size";
    else if (sortBy == "modified") col = "modified_at";
    else if (sortBy == "created") col = "created_at";
    else if (sortBy == "type") col = "mime_type";
    else col = "name";
    return " ORDER BY is_folder DESC, " + col + (desc ? " DESC" : " ASC") + ", name ASC";
}

const char* kUserColumns =
    "id, username, display_name, password_hash, is_admin, enabled, quota_bytes, created_at, "
    "last_login_at";

void readUser(const MysqlRow& r, UserEntry& u) {
    u.id = static_cast<int>(toInt(r.at(0)));
    u.username = r.at(1);
    u.displayName = r.at(2);
    u.passwordHash = r.at(3);
    u.isAdmin = toInt(r.at(4)) != 0;
    u.enabled = toInt(r.at(5)) != 0;
    u.quotaBytes = toUInt(r.at(6));
    u.createdAt = toInt(r.at(7));
    u.lastLoginAt = toInt(r.at(8));
}

const char* kUploadColumns =
    "id, owner_id, name, target_path, total_size, received_bytes, stored_bytes, chunk_count, "
    "state, message, quick_hash, created_at, updated_at";

void readUpload(const MysqlRow& r, UploadRecord& u) {
    u.id = r.at(0);
    u.ownerId = static_cast<int>(toInt(r.at(1)));
    u.name = r.at(2);
    u.targetPath = r.at(3);
    u.totalSize = toUInt(r.at(4));
    u.receivedBytes = toUInt(r.at(5));
    u.storedBytes = toUInt(r.at(6));
    u.chunkCount = static_cast<int>(toInt(r.at(7)));
    u.state = r.at(8);
    u.message = r.at(9);
    u.quickHash = r.at(10);
    u.createdAt = toInt(r.at(11));
    u.updatedAt = toInt(r.at(12));
}

}  // namespace

MysqlDatabase::MysqlDatabase(DatabaseConfig config) : config_(std::move(config)) {}

MysqlDatabase::~MysqlDatabase() { close(); }

bool MysqlDatabase::ensureConnection(std::string& error) {
    if (connected_ && conn_.connected()) return true;
    MysqlConnectionParams p;
    p.host = config_.mysqlHost;
    p.port = config_.mysqlPort;
    p.user = config_.mysqlUser;
    p.password = config_.mysqlPassword;
    p.database = config_.mysqlDatabase;
    p.charset = config_.mysqlCharset;
    p.timeoutMs = config_.mysqlTimeoutMs;
    if (!conn_.connect(p, error)) {
        connected_ = false;
        return false;
    }
    connected_ = true;
    std::string err;
    conn_.query("SET NAMES utf8mb4 COLLATE utf8mb4_unicode_ci");
    conn_.query("SET SESSION sql_mode='STRICT_TRANS_TABLES,NO_ENGINE_SUBSTITUTION'");
    conn_.query("SET SESSION time_zone='+00:00'");
    return true;
}

bool MysqlDatabase::open(std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensureConnection(error)) return false;
    LOG_INFO(kTag, "Đã mở cơ sở dữ liệu MySQL %s tại %s", config_.mysqlDatabase.c_str(),
             config_.mysqlHost.c_str());
    return true;
}

void MysqlDatabase::close() {
    std::lock_guard<std::mutex> lk(mu_);
    conn_.close();
    connected_ = false;
}

bool MysqlDatabase::healthy() const {
    std::lock_guard<std::mutex> lk(mu_);
    return connected_ && conn_.connected();
}

std::string MysqlDatabase::description() const {
    return "MySQL " + conn_.serverVersion() + " — " + config_.mysqlUser + "@" +
           config_.mysqlHost + ":" + std::to_string(config_.mysqlPort) + "/" +
           config_.mysqlDatabase;
}

MysqlResult MysqlDatabase::run(const std::string& sql, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensureConnection(error)) {
        MysqlResult r;
        r.error = error;
        return r;
    }
    MysqlResult res = conn_.query(sql);
    if (!res.ok && res.errorCode == 0 && !res.error.empty()) {
        // Nhiều khả năng mất kết nối — thử kết nối lại một lần.
        LOG_WARN(kTag, "Mất kết nối MySQL, đang kết nối lại…");
        conn_.close();
        connected_ = false;
        if (ensureConnection(error)) res = conn_.query(sql);
    }
    if (!res.ok) {
        error = res.error.empty() ? "Truy vấn MySQL thất bại" : res.error;
        if (res.errorCode) error = "MySQL lỗi " + std::to_string(res.errorCode) + ": " + error;
    }
    return res;
}

bool MysqlDatabase::migrate(std::string& error) {
    for (const auto& sql : schemaStatements("mysql")) {
        std::string err;
        MysqlResult r = run(sql, err);
        if (!r.ok) {
            // 1061 = chỉ mục đã tồn tại; 1050 = bảng đã tồn tại — bỏ qua.
            if (r.errorCode == 1061 || r.errorCode == 1050) continue;
            error = "Lỗi khi tạo bảng: " + err + " | Câu lệnh: " + sql.substr(0, 160);
            return false;
        }
    }
    LOG_INFO(kTag, "Cấu trúc bảng đã sẵn sàng");
    return true;
}

// ---------------------------------------------------------------------------
//  Tệp & thư mục
// ---------------------------------------------------------------------------
bool MysqlDatabase::createEntry(FileEntry& e, std::string& error) {
    if (e.createdAt == 0) e.createdAt = nowUnix();
    if (e.modifiedAt == 0) e.modifiedAt = e.createdAt;
    std::string sql =
        "INSERT INTO ttd_entries (parent_id, name, path, is_folder, size, mime_type, sha256, "
        "quick_hash, chunk_size, chunk_count, created_at, modified_at, owner_id, trashed, "
        "trashed_at, share_token, share_expires_at, starred, note) VALUES (" +
        N(e.parentId) + "," + Q(e.name) + "," + Q(e.path) + "," + B(e.isFolder) + "," +
        N(e.size) + "," + Q(e.mimeType) + "," + Q(e.sha256) + "," + Q(e.quickHash) + "," +
        N(e.chunkSize) + "," + N(static_cast<int64_t>(e.chunkCount)) + "," + N(e.createdAt) +
        "," + N(e.modifiedAt) + "," + N(static_cast<int64_t>(e.ownerId)) + "," + B(e.trashed) +
        "," + N(e.trashedAt) + "," + Q(e.shareToken) + "," + N(e.shareExpiresAt) + "," +
        B(e.starred) + "," + Q(e.note) + ")";
    MysqlResult r = run(sql, error);
    if (!r.ok) return false;
    e.id = static_cast<int64_t>(r.lastInsertId);
    return true;
}

bool MysqlDatabase::updateEntry(const FileEntry& e, std::string& error) {
    std::string sql =
        "UPDATE ttd_entries SET parent_id=" + N(e.parentId) + ", name=" + Q(e.name) +
        ", path=" + Q(e.path) + ", is_folder=" + B(e.isFolder) + ", size=" + N(e.size) +
        ", mime_type=" + Q(e.mimeType) + ", sha256=" + Q(e.sha256) +
        ", quick_hash=" + Q(e.quickHash) + ", chunk_size=" + N(e.chunkSize) +
        ", chunk_count=" + N(static_cast<int64_t>(e.chunkCount)) +
        ", created_at=" + N(e.createdAt) + ", modified_at=" + N(e.modifiedAt) +
        ", owner_id=" + N(static_cast<int64_t>(e.ownerId)) + ", trashed=" + B(e.trashed) +
        ", trashed_at=" + N(e.trashedAt) + ", share_token=" + Q(e.shareToken) +
        ", share_expires_at=" + N(e.shareExpiresAt) + ", starred=" + B(e.starred) +
        ", note=" + Q(e.note) + " WHERE id=" + N(e.id);
    return run(sql, error).ok;
}

bool MysqlDatabase::deleteEntry(int64_t id, std::string& error) {
    return run("DELETE FROM ttd_entries WHERE id=" + N(id), error).ok;
}

bool MysqlDatabase::getEntry(int64_t id, FileEntry& out, std::string& error) {
    MysqlResult r =
        run(std::string("SELECT ") + kEntryColumns + " FROM ttd_entries WHERE id=" + N(id), error);
    if (!r.ok) return false;
    if (r.rows.empty()) {
        error = "Không tìm thấy mục có mã " + N(id);
        return false;
    }
    readEntry(r.rows[0], out);
    return true;
}

bool MysqlDatabase::getEntryByPath(const std::string& path, FileEntry& out,
                                   std::string& error) {
    MysqlResult r = run(std::string("SELECT ") + kEntryColumns +
                            " FROM ttd_entries WHERE path=" + Q(path) + " AND trashed=0 LIMIT 1",
                        error);
    if (!r.ok) return false;
    if (r.rows.empty()) {
        error = "Không tìm thấy đường dẫn " + path;
        return false;
    }
    readEntry(r.rows[0], out);
    return true;
}

bool MysqlDatabase::listEntries(const ListOptions& opts, std::vector<FileEntry>& out,
                                std::string& error) {
    std::string sql = std::string("SELECT ") + kEntryColumns + " FROM ttd_entries WHERE 1=1";
    if (opts.onlyTrashed) sql += " AND trashed=1";
    else if (!opts.includeTrashed) sql += " AND trashed=0";
    if (opts.onlyStarred) sql += " AND starred=1";
    if (opts.search.empty() && !opts.onlyTrashed && !opts.onlyStarred)
        sql += " AND parent_id=" + N(opts.parentId);
    if (!opts.search.empty()) sql += " AND LOWER(name) LIKE " + Q("%" + toLowerUtf8(opts.search) + "%");
    if (opts.ownerId > 0) sql += " AND owner_id=" + N(static_cast<int64_t>(opts.ownerId));
    sql += orderByClause(opts.sortBy, opts.descending);
    sql += " LIMIT " + N(static_cast<int64_t>(opts.limit > 0 ? opts.limit : 500)) + " OFFSET " +
           N(static_cast<int64_t>(opts.offset));

    MysqlResult r = run(sql, error);
    if (!r.ok) return false;
    for (const auto& row : r.rows) {
        FileEntry e;
        readEntry(row, e);
        out.push_back(std::move(e));
    }
    return true;
}

bool MysqlDatabase::countEntries(const ListOptions& opts, uint64_t& out, std::string& error) {
    std::string sql = "SELECT COUNT(*) FROM ttd_entries WHERE 1=1";
    if (opts.onlyTrashed) sql += " AND trashed=1";
    else if (!opts.includeTrashed) sql += " AND trashed=0";
    if (opts.onlyStarred) sql += " AND starred=1";
    if (opts.search.empty() && !opts.onlyTrashed && !opts.onlyStarred)
        sql += " AND parent_id=" + N(opts.parentId);
    if (!opts.search.empty()) sql += " AND LOWER(name) LIKE " + Q("%" + toLowerUtf8(opts.search) + "%");
    if (opts.ownerId > 0) sql += " AND owner_id=" + N(static_cast<int64_t>(opts.ownerId));
    MysqlResult r = run(sql, error);
    if (!r.ok) return false;
    out = r.rows.empty() ? 0 : toUInt(r.rows[0].at(0));
    return true;
}

bool MysqlDatabase::findByHash(const std::string& sha256, std::vector<FileEntry>& out,
                               std::string& error) {
    if (sha256.empty()) return true;
    MysqlResult r =
        run(std::string("SELECT ") + kEntryColumns + " FROM ttd_entries WHERE sha256=" +
                Q(sha256) + " AND is_folder=0 AND trashed=0 LIMIT 50",
            error);
    if (!r.ok) return false;
    for (const auto& row : r.rows) {
        FileEntry e;
        readEntry(row, e);
        out.push_back(std::move(e));
    }
    return true;
}

bool MysqlDatabase::findByQuickHash(const std::string& quickHash, uint64_t size,
                                    std::vector<FileEntry>& out, std::string& error) {
    if (quickHash.empty()) return true;
    MysqlResult r = run(std::string("SELECT ") + kEntryColumns +
                            " FROM ttd_entries WHERE quick_hash=" + Q(quickHash) +
                            " AND size=" + N(size) + " AND is_folder=0 AND trashed=0 LIMIT 50",
                        error);
    if (!r.ok) return false;
    for (const auto& row : r.rows) {
        FileEntry e;
        readEntry(row, e);
        out.push_back(std::move(e));
    }
    return true;
}

bool MysqlDatabase::findByNameInFolder(int64_t parentId, const std::string& name, FileEntry& out,
                                       std::string& error) {
    MysqlResult r = run(std::string("SELECT ") + kEntryColumns +
                            " FROM ttd_entries WHERE parent_id=" + N(parentId) +
                            " AND name=" + Q(name) + " AND trashed=0 LIMIT 1",
                        error);
    if (!r.ok) return false;
    if (r.rows.empty()) {
        error = "Không tìm thấy";
        return false;
    }
    readEntry(r.rows[0], out);
    return true;
}

bool MysqlDatabase::listChildrenRecursive(int64_t folderId, std::vector<FileEntry>& out,
                                          std::string& error) {
    std::vector<int64_t> queue{folderId};
    while (!queue.empty()) {
        int64_t cur = queue.back();
        queue.pop_back();
        MysqlResult r = run(std::string("SELECT ") + kEntryColumns +
                                " FROM ttd_entries WHERE parent_id=" + N(cur),
                            error);
        if (!r.ok) return false;
        for (const auto& row : r.rows) {
            FileEntry e;
            readEntry(row, e);
            if (e.isFolder) queue.push_back(e.id);
            out.push_back(std::move(e));
        }
        if (out.size() > 200000) break;
    }
    return true;
}

bool MysqlDatabase::updatePathsUnder(const std::string& oldPrefix, const std::string& newPrefix,
                                     std::string& error) {
    std::string sql = "UPDATE ttd_entries SET path = CONCAT(" + Q(newPrefix) + ", SUBSTRING(path, " +
                      N(static_cast<int64_t>(utf8Length(oldPrefix) + 1)) + ")) WHERE path=" +
                      Q(oldPrefix) + " OR path LIKE " + Q(oldPrefix + "/%");
    return run(sql, error).ok;
}

bool MysqlDatabase::getEntryByShareToken(const std::string& token, FileEntry& out,
                                         std::string& error) {
    if (token.empty()) {
        error = "Mã chia sẻ rỗng";
        return false;
    }
    MysqlResult r = run(std::string("SELECT ") + kEntryColumns +
                            " FROM ttd_entries WHERE share_token=" + Q(token) +
                            " AND trashed=0 LIMIT 1",
                        error);
    if (!r.ok) return false;
    if (r.rows.empty()) {
        error = "Liên kết chia sẻ không tồn tại hoặc đã bị thu hồi";
        return false;
    }
    readEntry(r.rows[0], out);
    return true;
}

// ---------------------------------------------------------------------------
//  Mảnh dữ liệu
// ---------------------------------------------------------------------------
bool MysqlDatabase::addChunk(ChunkEntry& c, std::string& error) {
    if (c.createdAt == 0) c.createdAt = nowUnix();
    std::string sql =
        "INSERT INTO ttd_chunks (file_id, idx, offset_bytes, size, message_id, document_id, "
        "access_hash, file_reference, dc_id, account_id, sha256, created_at) VALUES (" +
        N(c.fileId) + "," + N(static_cast<int64_t>(c.index)) + "," + N(c.offset) + "," +
        N(c.size) + "," + N(c.messageId) + "," + N(c.documentId) + "," + N(c.accessHash) + "," +
        Q(c.fileReferenceHex) + "," + N(static_cast<int64_t>(c.dcId)) + "," +
        N(static_cast<int64_t>(c.accountId)) + "," + Q(c.sha256) + "," + N(c.createdAt) + ")";
    MysqlResult r = run(sql, error);
    if (!r.ok) return false;
    c.id = static_cast<int64_t>(r.lastInsertId);
    return true;
}

bool MysqlDatabase::listChunks(int64_t fileId, std::vector<ChunkEntry>& out,
                               std::string& error) {
    MysqlResult r = run(
        "SELECT id, file_id, idx, offset_bytes, size, message_id, document_id, access_hash, "
        "file_reference, dc_id, account_id, sha256, created_at FROM ttd_chunks WHERE file_id=" +
            N(fileId) + " ORDER BY idx ASC",
        error);
    if (!r.ok) return false;
    for (const auto& row : r.rows) {
        ChunkEntry c;
        c.id = toInt(row.at(0));
        c.fileId = toInt(row.at(1));
        c.index = static_cast<int>(toInt(row.at(2)));
        c.offset = toUInt(row.at(3));
        c.size = toUInt(row.at(4));
        c.messageId = toInt(row.at(5));
        c.documentId = toInt(row.at(6));
        c.accessHash = toInt(row.at(7));
        c.fileReferenceHex = row.at(8);
        c.dcId = static_cast<int>(toInt(row.at(9)));
        c.accountId = static_cast<int>(toInt(row.at(10)));
        c.sha256 = row.at(11);
        c.createdAt = toInt(row.at(12));
        out.push_back(std::move(c));
    }
    return true;
}

bool MysqlDatabase::deleteChunks(int64_t fileId, std::string& error) {
    return run("DELETE FROM ttd_chunks WHERE file_id=" + N(fileId), error).ok;
}

bool MysqlDatabase::updateChunkReference(int64_t chunkId, const std::string& fileReferenceHex,
                                         int64_t accessHash, int dcId, std::string& error) {
    return run("UPDATE ttd_chunks SET file_reference=" + Q(fileReferenceHex) +
                   ", access_hash=" + N(accessHash) + ", dc_id=" +
                   N(static_cast<int64_t>(dcId)) + " WHERE id=" + N(chunkId),
               error)
        .ok;
}

bool MysqlDatabase::countFilesWithHash(const std::string& sha256, uint64_t& out,
                                       std::string& error) {
    out = 0;
    if (sha256.empty()) return true;
    MysqlResult r = run(
        "SELECT COUNT(*) FROM ttd_entries WHERE sha256=" + Q(sha256) + " AND is_folder=0", error);
    if (!r.ok) return false;
    out = r.rows.empty() ? 0 : toUInt(r.rows[0].at(0));
    return true;
}

// ---------------------------------------------------------------------------
//  Người dùng
// ---------------------------------------------------------------------------
bool MysqlDatabase::createUser(UserEntry& u, std::string& error) {
    if (u.createdAt == 0) u.createdAt = nowUnix();
    MysqlResult r = run(
        "INSERT INTO ttd_users (username, display_name, password_hash, is_admin, enabled, "
        "quota_bytes, created_at, last_login_at) VALUES (" +
            Q(u.username) + "," + Q(u.displayName) + "," + Q(u.passwordHash) + "," +
            B(u.isAdmin) + "," + B(u.enabled) + "," + N(u.quotaBytes) + "," + N(u.createdAt) +
            "," + N(u.lastLoginAt) + ")",
        error);
    if (!r.ok) return false;
    u.id = static_cast<int>(r.lastInsertId);
    return true;
}

bool MysqlDatabase::updateUser(const UserEntry& u, std::string& error) {
    return run("UPDATE ttd_users SET username=" + Q(u.username) +
                   ", display_name=" + Q(u.displayName) + ", password_hash=" + Q(u.passwordHash) +
                   ", is_admin=" + B(u.isAdmin) + ", enabled=" + B(u.enabled) +
                   ", quota_bytes=" + N(u.quotaBytes) + ", last_login_at=" + N(u.lastLoginAt) +
                   " WHERE id=" + N(static_cast<int64_t>(u.id)),
               error)
        .ok;
}

bool MysqlDatabase::deleteUser(int id, std::string& error) {
    return run("DELETE FROM ttd_users WHERE id=" + N(static_cast<int64_t>(id)), error).ok;
}

bool MysqlDatabase::getUser(int id, UserEntry& out, std::string& error) {
    MysqlResult r = run(std::string("SELECT ") + kUserColumns + " FROM ttd_users WHERE id=" +
                            N(static_cast<int64_t>(id)),
                        error);
    if (!r.ok) return false;
    if (r.rows.empty()) {
        error = "Không tìm thấy người dùng";
        return false;
    }
    readUser(r.rows[0], out);
    return true;
}

bool MysqlDatabase::getUserByName(const std::string& username, UserEntry& out,
                                  std::string& error) {
    MysqlResult r = run(std::string("SELECT ") + kUserColumns +
                            " FROM ttd_users WHERE LOWER(username)=LOWER(" + Q(username) +
                            ") LIMIT 1",
                        error);
    if (!r.ok) return false;
    if (r.rows.empty()) {
        error = "Không tìm thấy người dùng";
        return false;
    }
    readUser(r.rows[0], out);
    return true;
}

bool MysqlDatabase::listUsers(std::vector<UserEntry>& out, std::string& error) {
    MysqlResult r =
        run(std::string("SELECT ") + kUserColumns + " FROM ttd_users ORDER BY id ASC", error);
    if (!r.ok) return false;
    for (const auto& row : r.rows) {
        UserEntry u;
        readUser(row, u);
        out.push_back(std::move(u));
    }
    return true;
}

bool MysqlDatabase::countUsers(uint64_t& out, std::string& error) {
    MysqlResult r = run("SELECT COUNT(*) FROM ttd_users", error);
    if (!r.ok) return false;
    out = r.rows.empty() ? 0 : toUInt(r.rows[0].at(0));
    return true;
}

// ---------------------------------------------------------------------------
//  Phiên web
// ---------------------------------------------------------------------------
bool MysqlDatabase::createSession(const WebSession& s, std::string& error) {
    return run("REPLACE INTO ttd_sessions (token, user_id, created_at, expires_at, user_agent, "
               "ip) VALUES (" +
                   Q(s.token) + "," + N(static_cast<int64_t>(s.userId)) + "," + N(s.createdAt) +
                   "," + N(s.expiresAt) + "," + Q(s.userAgent) + "," + Q(s.ip) + ")",
               error)
        .ok;
}

bool MysqlDatabase::getSession(const std::string& token, WebSession& out, std::string& error) {
    MysqlResult r = run("SELECT token, user_id, created_at, expires_at, user_agent, ip FROM "
                        "ttd_sessions WHERE token=" +
                            Q(token),
                        error);
    if (!r.ok) return false;
    if (r.rows.empty()) {
        error = "Phiên không tồn tại";
        return false;
    }
    const auto& row = r.rows[0];
    out.token = row.at(0);
    out.userId = static_cast<int>(toInt(row.at(1)));
    out.createdAt = toInt(row.at(2));
    out.expiresAt = toInt(row.at(3));
    out.userAgent = row.at(4);
    out.ip = row.at(5);
    return true;
}

bool MysqlDatabase::deleteSession(const std::string& token, std::string& error) {
    return run("DELETE FROM ttd_sessions WHERE token=" + Q(token), error).ok;
}

bool MysqlDatabase::deleteExpiredSessions(std::string& error) {
    return run("DELETE FROM ttd_sessions WHERE expires_at < " + N(nowUnix()), error).ok;
}

bool MysqlDatabase::deleteSessionsOfUser(int userId, std::string& error) {
    return run("DELETE FROM ttd_sessions WHERE user_id=" + N(static_cast<int64_t>(userId)),
               error)
        .ok;
}

// ---------------------------------------------------------------------------
//  Tài khoản Telegram
// ---------------------------------------------------------------------------
bool MysqlDatabase::createAccount(AccountEntry& a, std::string& error) {
    if (a.createdAt == 0) a.createdAt = nowUnix();
    MysqlResult r = run("INSERT INTO ttd_accounts (label, phone, display_name, enabled, home_dc, "
                        "created_at, last_used_at, note) VALUES (" +
                            Q(a.label) + "," + Q(a.phone) + "," + Q(a.displayName) + "," +
                            B(a.enabled) + "," + N(static_cast<int64_t>(a.homeDc)) + "," +
                            N(a.createdAt) + "," + N(a.lastUsedAt) + "," + Q(a.note) + ")",
                        error);
    if (!r.ok) return false;
    a.id = static_cast<int>(r.lastInsertId);
    return true;
}

bool MysqlDatabase::updateAccount(const AccountEntry& a, std::string& error) {
    return run("UPDATE ttd_accounts SET label=" + Q(a.label) + ", phone=" + Q(a.phone) +
                   ", display_name=" + Q(a.displayName) + ", enabled=" + B(a.enabled) +
                   ", home_dc=" + N(static_cast<int64_t>(a.homeDc)) +
                   ", last_used_at=" + N(a.lastUsedAt) + ", note=" + Q(a.note) +
                   " WHERE id=" + N(static_cast<int64_t>(a.id)),
               error)
        .ok;
}

bool MysqlDatabase::deleteAccount(int id, std::string& error) {
    return run("DELETE FROM ttd_accounts WHERE id=" + N(static_cast<int64_t>(id)), error).ok;
}

bool MysqlDatabase::listAccounts(std::vector<AccountEntry>& out, std::string& error) {
    MysqlResult r = run("SELECT id, label, phone, display_name, enabled, home_dc, created_at, "
                        "last_used_at, note FROM ttd_accounts ORDER BY id ASC",
                        error);
    if (!r.ok) return false;
    for (const auto& row : r.rows) {
        AccountEntry a;
        a.id = static_cast<int>(toInt(row.at(0)));
        a.label = row.at(1);
        a.phone = row.at(2);
        a.displayName = row.at(3);
        a.enabled = toInt(row.at(4)) != 0;
        a.homeDc = static_cast<int>(toInt(row.at(5)));
        a.createdAt = toInt(row.at(6));
        a.lastUsedAt = toInt(row.at(7));
        a.note = row.at(8);
        out.push_back(std::move(a));
    }
    return true;
}

bool MysqlDatabase::saveSessionKey(const SessionKeyEntry& k, std::string& error) {
    return run("REPLACE INTO ttd_session_keys (account_id, dc_id, auth_key, server_salt, "
               "updated_at) VALUES (" +
                   N(static_cast<int64_t>(k.accountId)) + "," +
                   N(static_cast<int64_t>(k.dcId)) + "," + Q(k.authKeyHex) + "," +
                   N(k.serverSalt) + "," + N(k.updatedAt ? k.updatedAt : nowUnix()) + ")",
               error)
        .ok;
}

bool MysqlDatabase::listSessionKeys(int accountId, std::vector<SessionKeyEntry>& out,
                                    std::string& error) {
    MysqlResult r = run("SELECT account_id, dc_id, auth_key, server_salt, updated_at FROM "
                        "ttd_session_keys WHERE account_id=" +
                            N(static_cast<int64_t>(accountId)),
                        error);
    if (!r.ok) return false;
    for (const auto& row : r.rows) {
        SessionKeyEntry k;
        k.accountId = static_cast<int>(toInt(row.at(0)));
        k.dcId = static_cast<int>(toInt(row.at(1)));
        k.authKeyHex = row.at(2);
        k.serverSalt = toInt(row.at(3));
        k.updatedAt = toInt(row.at(4));
        out.push_back(std::move(k));
    }
    return true;
}

bool MysqlDatabase::deleteSessionKeys(int accountId, std::string& error) {
    return run("DELETE FROM ttd_session_keys WHERE account_id=" +
                   N(static_cast<int64_t>(accountId)),
               error)
        .ok;
}

// ---------------------------------------------------------------------------
//  Phiên tải lên
// ---------------------------------------------------------------------------
bool MysqlDatabase::saveUpload(const UploadRecord& r, std::string& error) {
    return run("REPLACE INTO ttd_uploads (id, owner_id, name, target_path, total_size, "
               "received_bytes, stored_bytes, chunk_count, state, message, quick_hash, "
               "created_at, updated_at) VALUES (" +
                   Q(r.id) + "," + N(static_cast<int64_t>(r.ownerId)) + "," + Q(r.name) + "," +
                   Q(r.targetPath) + "," + N(r.totalSize) + "," + N(r.receivedBytes) + "," +
                   N(r.storedBytes) + "," + N(static_cast<int64_t>(r.chunkCount)) + "," +
                   Q(r.state) + "," + Q(r.message) + "," + Q(r.quickHash) + "," +
                   N(r.createdAt ? r.createdAt : nowUnix()) + "," + N(nowUnix()) + ")",
               error)
        .ok;
}

bool MysqlDatabase::getUpload(const std::string& id, UploadRecord& out, std::string& error) {
    MysqlResult r = run(std::string("SELECT ") + kUploadColumns + " FROM ttd_uploads WHERE id=" +
                            Q(id),
                        error);
    if (!r.ok) return false;
    if (r.rows.empty()) {
        error = "Không tìm thấy phiên tải lên";
        return false;
    }
    readUpload(r.rows[0], out);
    return true;
}

bool MysqlDatabase::listUploads(int ownerId, std::vector<UploadRecord>& out,
                                std::string& error) {
    std::string sql = std::string("SELECT ") + kUploadColumns + " FROM ttd_uploads";
    if (ownerId > 0) sql += " WHERE owner_id=" + N(static_cast<int64_t>(ownerId));
    sql += " ORDER BY created_at DESC LIMIT 200";
    MysqlResult r = run(sql, error);
    if (!r.ok) return false;
    for (const auto& row : r.rows) {
        UploadRecord u;
        readUpload(row, u);
        out.push_back(std::move(u));
    }
    return true;
}

bool MysqlDatabase::deleteUpload(const std::string& id, std::string& error) {
    return run("DELETE FROM ttd_uploads WHERE id=" + Q(id), error).ok;
}

bool MysqlDatabase::deleteStaleUploads(int64_t olderThan, std::string& error) {
    return run("DELETE FROM ttd_uploads WHERE updated_at < " + N(olderThan) +
                   " AND state <> 'hoan-tat'",
               error)
        .ok;
}

// ---------------------------------------------------------------------------
//  Cài đặt & thống kê
// ---------------------------------------------------------------------------
bool MysqlDatabase::getSetting(const std::string& key, std::string& value, std::string& error) {
    MysqlResult r = run("SELECT svalue FROM ttd_settings WHERE skey=" + Q(key), error);
    if (!r.ok || r.rows.empty()) return false;
    value = r.rows[0].at(0);
    return true;
}

bool MysqlDatabase::setSetting(const std::string& key, const std::string& value,
                               std::string& error) {
    return run("REPLACE INTO ttd_settings (skey, svalue) VALUES (" + Q(key) + "," + Q(value) + ")",
               error)
        .ok;
}

bool MysqlDatabase::listSettings(std::vector<std::pair<std::string, std::string>>& out,
                                 std::string& error) {
    MysqlResult r = run("SELECT skey, svalue FROM ttd_settings ORDER BY skey", error);
    if (!r.ok) return false;
    for (const auto& row : r.rows) out.emplace_back(row.at(0), row.at(1));
    return true;
}

bool MysqlDatabase::stats(StorageStats& out, std::string& error) {
    MysqlResult r = run("SELECT COALESCE(SUM(size),0), COUNT(*) FROM ttd_entries WHERE "
                        "is_folder=0 AND trashed=0",
                        error);
    if (!r.ok) return false;
    if (!r.rows.empty()) {
        out.totalBytes = toUInt(r.rows[0].at(0));
        out.fileCount = toUInt(r.rows[0].at(1));
    }
    MysqlResult r2 =
        run("SELECT COUNT(*) FROM ttd_entries WHERE is_folder=1 AND trashed=0", error);
    if (r2.ok && !r2.rows.empty()) out.folderCount = toUInt(r2.rows[0].at(0));
    MysqlResult r3 = run("SELECT COALESCE(SUM(size),0), COUNT(*) FROM ttd_entries WHERE "
                         "is_folder=0 AND trashed=1",
                         error);
    if (r3.ok && !r3.rows.empty()) {
        out.trashedBytes = toUInt(r3.rows[0].at(0));
        out.trashedCount = toUInt(r3.rows[0].at(1));
    }
    MysqlResult r4 = run("SELECT COUNT(*) FROM ttd_chunks", error);
    if (r4.ok && !r4.rows.empty()) out.chunkCount = toUInt(r4.rows[0].at(0));
    // Dung lượng thật: gộp theo document_id để mảnh dùng chung (tệp trùng nội
    // dung) chỉ được tính một lần. MySQL bắt buộc đặt bí danh cho bảng dẫn xuất.
    MysqlResult r5 = run("SELECT COALESCE(SUM(size),0), COUNT(*) FROM (SELECT document_id, "
                         "MAX(size) AS size FROM ttd_chunks GROUP BY document_id) AS m",
                         error);
    if (r5.ok && !r5.rows.empty()) {
        out.physicalBytes = toUInt(r5.rows[0].at(0));
        out.uniqueChunkCount = toUInt(r5.rows[0].at(1));
    }
    return true;
}

bool MysqlDatabase::usageByUser(int userId, uint64_t& bytes, std::string& error) {
    MysqlResult r = run("SELECT COALESCE(SUM(size),0) FROM ttd_entries WHERE owner_id=" +
                            N(static_cast<int64_t>(userId)) + " AND is_folder=0 AND trashed=0",
                        error);
    if (!r.ok) return false;
    bytes = r.rows.empty() ? 0 : toUInt(r.rows[0].at(0));
    return true;
}

}  // namespace db
}  // namespace ttd
