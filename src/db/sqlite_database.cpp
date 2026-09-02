#include "db/sqlite_database.h"

#include <cstring>

#include "common/fsutil.h"
#include "common/logging.h"
#include "common/strutil.h"
#include "common/timeutil.h"
#include "sqlite3.h"

namespace ttd {
namespace db {

namespace {
constexpr const char* kTag = "db.sqlite";

// Bọc sqlite3_stmt để luôn giải phóng đúng lúc.
class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql) : db_(db) {
        rc_ = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr);
    }
    ~Stmt() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool ok() const { return rc_ == SQLITE_OK && stmt_ != nullptr; }
    // sqlite3_errmsg(nullptr) trả về đúng chữ "out of memory", nghe như hết RAM
    // trong khi thật ra là cơ sở dữ liệu đã đóng. Nói thẳng cho đỡ đánh lạc hướng.
    std::string error() const {
        if (!db_) return "cơ sở dữ liệu đã đóng";
        return sqlite3_errmsg(db_);
    }

    void bindInt(int i, int64_t v) { sqlite3_bind_int64(stmt_, i, v); }
    void bindText(int i, const std::string& v) {
        sqlite3_bind_text(stmt_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }
    void bindNull(int i) { sqlite3_bind_null(stmt_, i); }

    // Trả về true nếu có hàng dữ liệu.
    bool step() {
        rc_ = sqlite3_step(stmt_);
        return rc_ == SQLITE_ROW;
    }
    bool done() const { return rc_ == SQLITE_DONE; }

    int64_t colInt(int i) const { return sqlite3_column_int64(stmt_, i); }
    std::string colText(int i) const {
        const unsigned char* t = sqlite3_column_text(stmt_, i);
        if (!t) return "";
        int n = sqlite3_column_bytes(stmt_, i);
        return std::string(reinterpret_cast<const char*>(t), static_cast<size_t>(n));
    }
    bool colBool(int i) const { return sqlite3_column_int(stmt_, i) != 0; }

    sqlite3_stmt* raw() { return stmt_; }

private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
    int rc_ = SQLITE_OK;
};

const char* kEntryColumns =
    "id, parent_id, name, path, is_folder, size, mime_type, sha256, quick_hash, chunk_size, "
    "chunk_count, created_at, modified_at, owner_id, trashed, trashed_at, share_token, "
    "share_expires_at, starred, note";

void readEntry(Stmt& st, FileEntry& e) {
    e.id = st.colInt(0);
    e.parentId = st.colInt(1);
    e.name = st.colText(2);
    e.path = st.colText(3);
    e.isFolder = st.colBool(4);
    e.size = static_cast<uint64_t>(st.colInt(5));
    e.mimeType = st.colText(6);
    e.sha256 = st.colText(7);
    e.quickHash = st.colText(8);
    e.chunkSize = static_cast<uint64_t>(st.colInt(9));
    e.chunkCount = static_cast<int>(st.colInt(10));
    e.createdAt = st.colInt(11);
    e.modifiedAt = st.colInt(12);
    e.ownerId = static_cast<int>(st.colInt(13));
    e.trashed = st.colBool(14);
    e.trashedAt = st.colInt(15);
    e.shareToken = st.colText(16);
    e.shareExpiresAt = st.colInt(17);
    e.starred = st.colBool(18);
    e.note = st.colText(19);
}

// Hàm SQL tuỳ biến: đổi chữ thường có hiểu dấu tiếng Việt.
// SQLite dựng sẵn chỉ đổi được chữ ASCII nên tìm "BÁO CÁO" sẽ không ra "Báo cáo".
void sqlLowerUtf8(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    if (argc < 1) {
        sqlite3_result_null(ctx);
        return;
    }
    const unsigned char* text = sqlite3_value_text(argv[0]);
    if (!text) {
        sqlite3_result_null(ctx);
        return;
    }
    int n = sqlite3_value_bytes(argv[0]);
    std::string out = toLowerUtf8(std::string(reinterpret_cast<const char*>(text),
                                              static_cast<size_t>(n)));
    sqlite3_result_text(ctx, out.c_str(), static_cast<int>(out.size()), SQLITE_TRANSIENT);
}

std::string orderByClause(const std::string& sortBy, bool desc) {
    std::string col;
    if (sortBy == "size") col = "size";
    else if (sortBy == "modified") col = "modified_at";
    else if (sortBy == "created") col = "created_at";
    else if (sortBy == "type") col = "mime_type";
    else col = "name";
    // Thư mục luôn hiện trước tệp.
    return " ORDER BY is_folder DESC, " + col + (desc ? " DESC" : " ASC") +
           ", name COLLATE NOCASE ASC";
}

}  // namespace

SqliteDatabase::SqliteDatabase(std::string path) : path_(std::move(path)) {}

SqliteDatabase::~SqliteDatabase() { close(); }

bool SqliteDatabase::open(std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_) return true;

    std::string dir = parentDirectoryOf(path_);
    if (!dir.empty() && !ensureDirectoryExists(dir)) {
        error = "Không tạo được thư mục chứa cơ sở dữ liệu: " + dir;
        return false;
    }

    int rc = sqlite3_open_v2(path_.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        error = "Không mở được tệp cơ sở dữ liệu: " +
                std::string(db_ ? sqlite3_errmsg(db_) : "lỗi không rõ");
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }
    sqlite3_busy_timeout(db_, 10000);

    // Đăng ký hàm đổi chữ thường hiểu tiếng Việt để tìm kiếm không phân biệt hoa thường.
    if (sqlite3_create_function_v2(db_, "ttd_lower", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                   nullptr, &sqlLowerUtf8, nullptr, nullptr,
                                   nullptr) != SQLITE_OK) {
        LOG_WARN(kTag, "Không đăng ký được hàm ttd_lower — tìm kiếm sẽ phân biệt dấu");
    }

    std::string err;
    // WAL cho phép đọc song song trong khi ghi — quan trọng khi vừa tải lên vừa duyệt.
    exec("PRAGMA journal_mode=WAL", err);
    exec("PRAGMA synchronous=NORMAL", err);
    exec("PRAGMA foreign_keys=ON", err);
    exec("PRAGMA temp_store=MEMORY", err);
    exec("PRAGMA cache_size=-32000", err);

    LOG_INFO(kTag, "Đã mở cơ sở dữ liệu SQLite: %s", path_.c_str());
    return true;
}

void SqliteDatabase::close() {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SqliteDatabase::healthy() const {
    std::lock_guard<std::mutex> lk(mu_);
    return db_ != nullptr;
}

std::string SqliteDatabase::description() const {
    return "SQLite " + std::string(sqlite3_libversion()) + " — " + path_;
}

std::string SqliteDatabase::lastError() const {
    return db_ ? sqlite3_errmsg(db_) : "cơ sở dữ liệu chưa mở";
}

bool SqliteDatabase::exec(const std::string& sql, std::string& error) {
    char* msg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
        error = msg ? msg : "lỗi SQL không rõ";
        if (msg) sqlite3_free(msg);
        return false;
    }
    if (msg) sqlite3_free(msg);
    return true;
}

bool SqliteDatabase::migrate(std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) {
        error = "Cơ sở dữ liệu chưa mở";
        return false;
    }
    for (const auto& sql : schemaStatements("sqlite")) {
        if (!exec(sql, error)) {
            error = "Lỗi khi tạo bảng: " + error + " | Câu lệnh: " + sql.substr(0, 120);
            return false;
        }
    }
    LOG_INFO(kTag, "Cấu trúc bảng đã sẵn sàng");
    return true;
}

// ---------------------------------------------------------------------------
//  Tệp & thư mục
// ---------------------------------------------------------------------------
bool SqliteDatabase::createEntry(FileEntry& e, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "INSERT INTO ttd_entries (parent_id, name, path, is_folder, size, mime_type, "
            "sha256, quick_hash, chunk_size, chunk_count, created_at, modified_at, owner_id, "
            "trashed, trashed_at, share_token, share_expires_at, starred, note) VALUES "
            "(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    if (e.createdAt == 0) e.createdAt = nowUnix();
    if (e.modifiedAt == 0) e.modifiedAt = e.createdAt;

    st.bindInt(1, e.parentId);
    st.bindText(2, e.name);
    st.bindText(3, e.path);
    st.bindInt(4, e.isFolder ? 1 : 0);
    st.bindInt(5, static_cast<int64_t>(e.size));
    st.bindText(6, e.mimeType);
    st.bindText(7, e.sha256);
    st.bindText(8, e.quickHash);
    st.bindInt(9, static_cast<int64_t>(e.chunkSize));
    st.bindInt(10, e.chunkCount);
    st.bindInt(11, e.createdAt);
    st.bindInt(12, e.modifiedAt);
    st.bindInt(13, e.ownerId);
    st.bindInt(14, e.trashed ? 1 : 0);
    st.bindInt(15, e.trashedAt);
    st.bindText(16, e.shareToken);
    st.bindInt(17, e.shareExpiresAt);
    st.bindInt(18, e.starred ? 1 : 0);
    st.bindText(19, e.note);

    st.step();
    if (!st.done()) {
        error = st.error();
        return false;
    }
    e.id = sqlite3_last_insert_rowid(db_);
    return true;
}

bool SqliteDatabase::updateEntry(const FileEntry& e, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "UPDATE ttd_entries SET parent_id=?, name=?, path=?, is_folder=?, size=?, "
            "mime_type=?, sha256=?, quick_hash=?, chunk_size=?, chunk_count=?, created_at=?, "
            "modified_at=?, owner_id=?, trashed=?, trashed_at=?, share_token=?, "
            "share_expires_at=?, starred=?, note=? WHERE id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, e.parentId);
    st.bindText(2, e.name);
    st.bindText(3, e.path);
    st.bindInt(4, e.isFolder ? 1 : 0);
    st.bindInt(5, static_cast<int64_t>(e.size));
    st.bindText(6, e.mimeType);
    st.bindText(7, e.sha256);
    st.bindText(8, e.quickHash);
    st.bindInt(9, static_cast<int64_t>(e.chunkSize));
    st.bindInt(10, e.chunkCount);
    st.bindInt(11, e.createdAt);
    st.bindInt(12, e.modifiedAt);
    st.bindInt(13, e.ownerId);
    st.bindInt(14, e.trashed ? 1 : 0);
    st.bindInt(15, e.trashedAt);
    st.bindText(16, e.shareToken);
    st.bindInt(17, e.shareExpiresAt);
    st.bindInt(18, e.starred ? 1 : 0);
    st.bindText(19, e.note);
    st.bindInt(20, e.id);
    st.step();
    if (!st.done()) {
        error = st.error();
        return false;
    }
    return true;
}

bool SqliteDatabase::deleteEntry(int64_t id, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "DELETE FROM ttd_entries WHERE id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, id);
    st.step();
    if (!st.done()) {
        error = st.error();
        return false;
    }
    return true;
}

bool SqliteDatabase::getEntry(int64_t id, FileEntry& out, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, std::string("SELECT ") + kEntryColumns + " FROM ttd_entries WHERE id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, id);
    if (!st.step()) {
        error = "Không tìm thấy mục có mã " + std::to_string(id);
        return false;
    }
    readEntry(st, out);
    return true;
}

bool SqliteDatabase::getEntryByPath(const std::string& path, FileEntry& out,
                                    std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, std::string("SELECT ") + kEntryColumns +
                     " FROM ttd_entries WHERE path=? AND trashed=0 LIMIT 1");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, path);
    if (!st.step()) {
        error = "Không tìm thấy đường dẫn " + path;
        return false;
    }
    readEntry(st, out);
    return true;
}

bool SqliteDatabase::listEntries(const ListOptions& opts, std::vector<FileEntry>& out,
                                 std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = std::string("SELECT ") + kEntryColumns + " FROM ttd_entries WHERE 1=1";
    if (opts.onlyTrashed) sql += " AND trashed=1";
    else if (!opts.includeTrashed) sql += " AND trashed=0";
    if (opts.onlyStarred) sql += " AND starred=1";
    if (opts.search.empty() && !opts.onlyTrashed && !opts.onlyStarred)
        sql += " AND parent_id=?";
    if (!opts.search.empty()) sql += " AND ttd_lower(name) LIKE ?";
    if (opts.ownerId > 0) sql += " AND owner_id=?";
    sql += orderByClause(opts.sortBy, opts.descending);
    sql += " LIMIT ? OFFSET ?";

    Stmt st(db_, sql);
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    int idx = 1;
    if (opts.search.empty() && !opts.onlyTrashed && !opts.onlyStarred)
        st.bindInt(idx++, opts.parentId);
    if (!opts.search.empty()) st.bindText(idx++, "%" + toLowerUtf8(opts.search) + "%");
    if (opts.ownerId > 0) st.bindInt(idx++, opts.ownerId);
    st.bindInt(idx++, opts.limit > 0 ? opts.limit : 500);
    st.bindInt(idx++, opts.offset);

    while (st.step()) {
        FileEntry e;
        readEntry(st, e);
        out.push_back(std::move(e));
    }
    return true;
}

bool SqliteDatabase::countEntries(const ListOptions& opts, uint64_t& out, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = "SELECT COUNT(*) FROM ttd_entries WHERE 1=1";
    if (opts.onlyTrashed) sql += " AND trashed=1";
    else if (!opts.includeTrashed) sql += " AND trashed=0";
    if (opts.onlyStarred) sql += " AND starred=1";
    if (opts.search.empty() && !opts.onlyTrashed && !opts.onlyStarred)
        sql += " AND parent_id=?";
    if (!opts.search.empty()) sql += " AND ttd_lower(name) LIKE ?";
    if (opts.ownerId > 0) sql += " AND owner_id=?";

    Stmt st(db_, sql);
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    int idx = 1;
    if (opts.search.empty() && !opts.onlyTrashed && !opts.onlyStarred)
        st.bindInt(idx++, opts.parentId);
    if (!opts.search.empty()) st.bindText(idx++, "%" + toLowerUtf8(opts.search) + "%");
    if (opts.ownerId > 0) st.bindInt(idx++, opts.ownerId);
    out = st.step() ? static_cast<uint64_t>(st.colInt(0)) : 0;
    return true;
}

bool SqliteDatabase::findByHash(const std::string& sha256, std::vector<FileEntry>& out,
                                std::string& error) {
    if (sha256.empty()) return true;
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, std::string("SELECT ") + kEntryColumns +
                     " FROM ttd_entries WHERE sha256=? AND is_folder=0 AND trashed=0 LIMIT 50");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, sha256);
    while (st.step()) {
        FileEntry e;
        readEntry(st, e);
        out.push_back(std::move(e));
    }
    return true;
}

bool SqliteDatabase::findByQuickHash(const std::string& quickHash, uint64_t size,
                                     std::vector<FileEntry>& out, std::string& error) {
    if (quickHash.empty()) return true;
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, std::string("SELECT ") + kEntryColumns +
                     " FROM ttd_entries WHERE quick_hash=? AND size=? AND is_folder=0 AND "
                     "trashed=0 LIMIT 50");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, quickHash);
    st.bindInt(2, static_cast<int64_t>(size));
    while (st.step()) {
        FileEntry e;
        readEntry(st, e);
        out.push_back(std::move(e));
    }
    return true;
}

bool SqliteDatabase::findByNameInFolder(int64_t parentId, const std::string& name,
                                        FileEntry& out, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, std::string("SELECT ") + kEntryColumns +
                     " FROM ttd_entries WHERE parent_id=? AND name=? AND trashed=0 LIMIT 1");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, parentId);
    st.bindText(2, name);
    if (!st.step()) {
        error = "Không tìm thấy";
        return false;
    }
    readEntry(st, out);
    return true;
}

bool SqliteDatabase::listChildrenRecursive(int64_t folderId, std::vector<FileEntry>& out,
                                           std::string& error) {
    // Duyệt theo tầng để không phụ thuộc vào CTE đệ quy.
    std::vector<int64_t> queue{folderId};
    while (!queue.empty()) {
        int64_t cur = queue.back();
        queue.pop_back();
        std::vector<FileEntry> children;
        {
            std::lock_guard<std::mutex> lk(mu_);
            Stmt st(db_, std::string("SELECT ") + kEntryColumns +
                             " FROM ttd_entries WHERE parent_id=?");
            if (!st.ok()) {
                error = st.error();
                return false;
            }
            st.bindInt(1, cur);
            while (st.step()) {
                FileEntry e;
                readEntry(st, e);
                children.push_back(std::move(e));
            }
        }
        for (auto& c : children) {
            if (c.isFolder) queue.push_back(c.id);
            out.push_back(std::move(c));
        }
        if (out.size() > 200000) break;  // chặn vòng lặp bất thường
    }
    return true;
}

bool SqliteDatabase::updatePathsUnder(const std::string& oldPrefix, const std::string& newPrefix,
                                      std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "UPDATE ttd_entries SET path = ? || substr(path, ?) WHERE path = ? OR path LIKE ?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, newPrefix);
    // substr() của SQLite đếm theo ký tự, không phải byte.
    st.bindInt(2, static_cast<int64_t>(utf8Length(oldPrefix) + 1));
    st.bindText(3, oldPrefix);
    st.bindText(4, oldPrefix + "/%");
    st.step();
    if (!st.done()) {
        error = st.error();
        return false;
    }
    return true;
}

bool SqliteDatabase::getEntryByShareToken(const std::string& token, FileEntry& out,
                                          std::string& error) {
    if (token.empty()) {
        error = "Mã chia sẻ rỗng";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, std::string("SELECT ") + kEntryColumns +
                     " FROM ttd_entries WHERE share_token=? AND trashed=0 LIMIT 1");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, token);
    if (!st.step()) {
        error = "Liên kết chia sẻ không tồn tại hoặc đã bị thu hồi";
        return false;
    }
    readEntry(st, out);
    return true;
}

// ---------------------------------------------------------------------------
//  Mảnh dữ liệu
// ---------------------------------------------------------------------------
bool SqliteDatabase::addChunk(ChunkEntry& c, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "INSERT INTO ttd_chunks (file_id, idx, offset_bytes, size, message_id, document_id, "
            "access_hash, file_reference, dc_id, account_id, sha256, created_at) VALUES "
            "(?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    if (c.createdAt == 0) c.createdAt = nowUnix();
    st.bindInt(1, c.fileId);
    st.bindInt(2, c.index);
    st.bindInt(3, static_cast<int64_t>(c.offset));
    st.bindInt(4, static_cast<int64_t>(c.size));
    st.bindInt(5, c.messageId);
    st.bindInt(6, c.documentId);
    st.bindInt(7, c.accessHash);
    st.bindText(8, c.fileReferenceHex);
    st.bindInt(9, c.dcId);
    st.bindInt(10, c.accountId);
    st.bindText(11, c.sha256);
    st.bindInt(12, c.createdAt);
    st.step();
    if (!st.done()) {
        error = st.error();
        return false;
    }
    c.id = sqlite3_last_insert_rowid(db_);
    return true;
}

bool SqliteDatabase::listChunks(int64_t fileId, std::vector<ChunkEntry>& out,
                                std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "SELECT id, file_id, idx, offset_bytes, size, message_id, document_id, access_hash, "
            "file_reference, dc_id, account_id, sha256, created_at FROM ttd_chunks WHERE "
            "file_id=? ORDER BY idx ASC");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, fileId);
    while (st.step()) {
        ChunkEntry c;
        c.id = st.colInt(0);
        c.fileId = st.colInt(1);
        c.index = static_cast<int>(st.colInt(2));
        c.offset = static_cast<uint64_t>(st.colInt(3));
        c.size = static_cast<uint64_t>(st.colInt(4));
        c.messageId = st.colInt(5);
        c.documentId = st.colInt(6);
        c.accessHash = st.colInt(7);
        c.fileReferenceHex = st.colText(8);
        c.dcId = static_cast<int>(st.colInt(9));
        c.accountId = static_cast<int>(st.colInt(10));
        c.sha256 = st.colText(11);
        c.createdAt = st.colInt(12);
        out.push_back(std::move(c));
    }
    return true;
}

bool SqliteDatabase::deleteChunks(int64_t fileId, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "DELETE FROM ttd_chunks WHERE file_id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, fileId);
    st.step();
    return st.done();
}

bool SqliteDatabase::updateChunkReference(int64_t chunkId, const std::string& fileReferenceHex,
                                          int64_t accessHash, int dcId, int accountId,
                                          std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    // access_hash lưu ở đây là của riêng tài khoản đã lấy nó, nên account_id
    // phải đi cùng — nếu không, lần đọc sau sẽ đưa hash của người này cho
    // người khác dùng và bị Telegram từ chối.
    Stmt st(db_,
            "UPDATE ttd_chunks SET file_reference=?, access_hash=?, dc_id=?, account_id=? "
            "WHERE id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, fileReferenceHex);
    st.bindInt(2, accessHash);
    st.bindInt(3, dcId);
    st.bindInt(4, accountId);
    st.bindInt(5, chunkId);
    st.step();
    return st.done();
}

bool SqliteDatabase::countFilesWithHash(const std::string& sha256, uint64_t& out,
                                        std::string& error) {
    out = 0;
    if (sha256.empty()) return true;
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "SELECT COUNT(*) FROM ttd_entries WHERE sha256=? AND is_folder=0");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, sha256);
    if (st.step()) out = static_cast<uint64_t>(st.colInt(0));
    return true;
}

// ---------------------------------------------------------------------------
//  Người dùng
// ---------------------------------------------------------------------------
namespace {
void readUser(Stmt& st, UserEntry& u) {
    u.id = static_cast<int>(st.colInt(0));
    u.username = st.colText(1);
    u.displayName = st.colText(2);
    u.passwordHash = st.colText(3);
    u.isAdmin = st.colBool(4);
    u.enabled = st.colBool(5);
    u.quotaBytes = static_cast<uint64_t>(st.colInt(6));
    u.createdAt = st.colInt(7);
    u.lastLoginAt = st.colInt(8);
}
const char* kUserColumns =
    "id, username, display_name, password_hash, is_admin, enabled, quota_bytes, created_at, "
    "last_login_at";
}  // namespace

bool SqliteDatabase::createUser(UserEntry& u, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "INSERT INTO ttd_users (username, display_name, password_hash, is_admin, enabled, "
            "quota_bytes, created_at, last_login_at) VALUES (?,?,?,?,?,?,?,?)");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    if (u.createdAt == 0) u.createdAt = nowUnix();
    st.bindText(1, u.username);
    st.bindText(2, u.displayName);
    st.bindText(3, u.passwordHash);
    st.bindInt(4, u.isAdmin ? 1 : 0);
    st.bindInt(5, u.enabled ? 1 : 0);
    st.bindInt(6, static_cast<int64_t>(u.quotaBytes));
    st.bindInt(7, u.createdAt);
    st.bindInt(8, u.lastLoginAt);
    st.step();
    if (!st.done()) {
        error = "Không tạo được người dùng: " + st.error();
        return false;
    }
    u.id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    return true;
}

bool SqliteDatabase::updateUser(const UserEntry& u, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "UPDATE ttd_users SET username=?, display_name=?, password_hash=?, is_admin=?, "
            "enabled=?, quota_bytes=?, last_login_at=? WHERE id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, u.username);
    st.bindText(2, u.displayName);
    st.bindText(3, u.passwordHash);
    st.bindInt(4, u.isAdmin ? 1 : 0);
    st.bindInt(5, u.enabled ? 1 : 0);
    st.bindInt(6, static_cast<int64_t>(u.quotaBytes));
    st.bindInt(7, u.lastLoginAt);
    st.bindInt(8, u.id);
    st.step();
    if (!st.done()) {
        error = st.error();
        return false;
    }
    return true;
}

bool SqliteDatabase::deleteUser(int id, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "DELETE FROM ttd_users WHERE id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, id);
    st.step();
    return st.done();
}

bool SqliteDatabase::getUser(int id, UserEntry& out, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, std::string("SELECT ") + kUserColumns + " FROM ttd_users WHERE id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, id);
    if (!st.step()) {
        error = "Không tìm thấy người dùng";
        return false;
    }
    readUser(st, out);
    return true;
}

bool SqliteDatabase::getUserByName(const std::string& username, UserEntry& out,
                                   std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, std::string("SELECT ") + kUserColumns +
                     " FROM ttd_users WHERE ttd_lower(username)=ttd_lower(?) LIMIT 1");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, username);
    if (!st.step()) {
        error = "Không tìm thấy người dùng";
        return false;
    }
    readUser(st, out);
    return true;
}

bool SqliteDatabase::listUsers(std::vector<UserEntry>& out, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, std::string("SELECT ") + kUserColumns + " FROM ttd_users ORDER BY id ASC");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    while (st.step()) {
        UserEntry u;
        readUser(st, u);
        out.push_back(std::move(u));
    }
    return true;
}

bool SqliteDatabase::countUsers(uint64_t& out, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "SELECT COUNT(*) FROM ttd_users");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    out = st.step() ? static_cast<uint64_t>(st.colInt(0)) : 0;
    return true;
}

// ---------------------------------------------------------------------------
//  Phiên web
// ---------------------------------------------------------------------------
bool SqliteDatabase::createSession(const WebSession& s, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "INSERT OR REPLACE INTO ttd_sessions (token, user_id, created_at, expires_at, "
            "user_agent, ip) VALUES (?,?,?,?,?,?)");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, s.token);
    st.bindInt(2, s.userId);
    st.bindInt(3, s.createdAt);
    st.bindInt(4, s.expiresAt);
    st.bindText(5, s.userAgent);
    st.bindText(6, s.ip);
    st.step();
    return st.done();
}

bool SqliteDatabase::getSession(const std::string& token, WebSession& out, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "SELECT token, user_id, created_at, expires_at, user_agent, ip FROM ttd_sessions "
            "WHERE token=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, token);
    if (!st.step()) {
        error = "Phiên không tồn tại";
        return false;
    }
    out.token = st.colText(0);
    out.userId = static_cast<int>(st.colInt(1));
    out.createdAt = st.colInt(2);
    out.expiresAt = st.colInt(3);
    out.userAgent = st.colText(4);
    out.ip = st.colText(5);
    return true;
}

bool SqliteDatabase::deleteSession(const std::string& token, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "DELETE FROM ttd_sessions WHERE token=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, token);
    st.step();
    return st.done();
}

bool SqliteDatabase::deleteExpiredSessions(std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "DELETE FROM ttd_sessions WHERE expires_at < ?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, nowUnix());
    st.step();
    return st.done();
}

bool SqliteDatabase::deleteSessionsOfUser(int userId, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "DELETE FROM ttd_sessions WHERE user_id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, userId);
    st.step();
    return st.done();
}

// ---------------------------------------------------------------------------
//  Tài khoản Telegram
// ---------------------------------------------------------------------------
bool SqliteDatabase::createAccount(AccountEntry& a, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "INSERT INTO ttd_accounts (label, phone, display_name, enabled, home_dc, "
            "created_at, last_used_at, note) VALUES (?,?,?,?,?,?,?,?)");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    if (a.createdAt == 0) a.createdAt = nowUnix();
    st.bindText(1, a.label);
    st.bindText(2, a.phone);
    st.bindText(3, a.displayName);
    st.bindInt(4, a.enabled ? 1 : 0);
    st.bindInt(5, a.homeDc);
    st.bindInt(6, a.createdAt);
    st.bindInt(7, a.lastUsedAt);
    st.bindText(8, a.note);
    st.step();
    if (!st.done()) {
        error = st.error();
        return false;
    }
    a.id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    return true;
}

bool SqliteDatabase::updateAccount(const AccountEntry& a, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "UPDATE ttd_accounts SET label=?, phone=?, display_name=?, enabled=?, home_dc=?, "
            "last_used_at=?, note=? WHERE id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, a.label);
    st.bindText(2, a.phone);
    st.bindText(3, a.displayName);
    st.bindInt(4, a.enabled ? 1 : 0);
    st.bindInt(5, a.homeDc);
    st.bindInt(6, a.lastUsedAt);
    st.bindText(7, a.note);
    st.bindInt(8, a.id);
    st.step();
    return st.done();
}

bool SqliteDatabase::deleteAccount(int id, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "DELETE FROM ttd_accounts WHERE id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, id);
    st.step();
    return st.done();
}

bool SqliteDatabase::listAccounts(std::vector<AccountEntry>& out, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "SELECT id, label, phone, display_name, enabled, home_dc, created_at, last_used_at, "
            "note FROM ttd_accounts ORDER BY id ASC");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    while (st.step()) {
        AccountEntry a;
        a.id = static_cast<int>(st.colInt(0));
        a.label = st.colText(1);
        a.phone = st.colText(2);
        a.displayName = st.colText(3);
        a.enabled = st.colBool(4);
        a.homeDc = static_cast<int>(st.colInt(5));
        a.createdAt = st.colInt(6);
        a.lastUsedAt = st.colInt(7);
        a.note = st.colText(8);
        out.push_back(std::move(a));
    }
    return true;
}

bool SqliteDatabase::saveSessionKey(const SessionKeyEntry& k, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "INSERT OR REPLACE INTO ttd_session_keys (account_id, dc_id, auth_key, server_salt, "
            "updated_at) VALUES (?,?,?,?,?)");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, k.accountId);
    st.bindInt(2, k.dcId);
    st.bindText(3, k.authKeyHex);
    st.bindInt(4, k.serverSalt);
    st.bindInt(5, k.updatedAt ? k.updatedAt : nowUnix());
    st.step();
    return st.done();
}

bool SqliteDatabase::listSessionKeys(int accountId, std::vector<SessionKeyEntry>& out,
                                     std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "SELECT account_id, dc_id, auth_key, server_salt, updated_at FROM ttd_session_keys "
            "WHERE account_id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, accountId);
    while (st.step()) {
        SessionKeyEntry k;
        k.accountId = static_cast<int>(st.colInt(0));
        k.dcId = static_cast<int>(st.colInt(1));
        k.authKeyHex = st.colText(2);
        k.serverSalt = st.colInt(3);
        k.updatedAt = st.colInt(4);
        out.push_back(std::move(k));
    }
    return true;
}

bool SqliteDatabase::deleteSessionKeys(int accountId, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "DELETE FROM ttd_session_keys WHERE account_id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, accountId);
    st.step();
    return st.done();
}

// ---------------------------------------------------------------------------
//  Phiên tải lên
// ---------------------------------------------------------------------------
bool SqliteDatabase::saveUpload(const UploadRecord& r, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "INSERT OR REPLACE INTO ttd_uploads (id, owner_id, name, target_path, total_size, "
            "received_bytes, stored_bytes, chunk_count, state, message, quick_hash, created_at, "
            "updated_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, r.id);
    st.bindInt(2, r.ownerId);
    st.bindText(3, r.name);
    st.bindText(4, r.targetPath);
    st.bindInt(5, static_cast<int64_t>(r.totalSize));
    st.bindInt(6, static_cast<int64_t>(r.receivedBytes));
    st.bindInt(7, static_cast<int64_t>(r.storedBytes));
    st.bindInt(8, r.chunkCount);
    st.bindText(9, r.state);
    st.bindText(10, r.message);
    st.bindText(11, r.quickHash);
    st.bindInt(12, r.createdAt ? r.createdAt : nowUnix());
    st.bindInt(13, nowUnix());
    st.step();
    return st.done();
}

namespace {
void readUpload(Stmt& st, UploadRecord& r) {
    r.id = st.colText(0);
    r.ownerId = static_cast<int>(st.colInt(1));
    r.name = st.colText(2);
    r.targetPath = st.colText(3);
    r.totalSize = static_cast<uint64_t>(st.colInt(4));
    r.receivedBytes = static_cast<uint64_t>(st.colInt(5));
    r.storedBytes = static_cast<uint64_t>(st.colInt(6));
    r.chunkCount = static_cast<int>(st.colInt(7));
    r.state = st.colText(8);
    r.message = st.colText(9);
    r.quickHash = st.colText(10);
    r.createdAt = st.colInt(11);
    r.updatedAt = st.colInt(12);
}
const char* kUploadColumns =
    "id, owner_id, name, target_path, total_size, received_bytes, stored_bytes, chunk_count, "
    "state, message, quick_hash, created_at, updated_at";
}  // namespace

bool SqliteDatabase::getUpload(const std::string& id, UploadRecord& out, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, std::string("SELECT ") + kUploadColumns + " FROM ttd_uploads WHERE id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, id);
    if (!st.step()) {
        error = "Không tìm thấy phiên tải lên";
        return false;
    }
    readUpload(st, out);
    return true;
}

bool SqliteDatabase::listUploads(int ownerId, std::vector<UploadRecord>& out,
                                 std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = std::string("SELECT ") + kUploadColumns + " FROM ttd_uploads";
    if (ownerId > 0) sql += " WHERE owner_id=?";
    sql += " ORDER BY created_at DESC LIMIT 200";
    Stmt st(db_, sql);
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    if (ownerId > 0) st.bindInt(1, ownerId);
    while (st.step()) {
        UploadRecord r;
        readUpload(st, r);
        out.push_back(std::move(r));
    }
    return true;
}

bool SqliteDatabase::deleteUpload(const std::string& id, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "DELETE FROM ttd_uploads WHERE id=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, id);
    st.step();
    return st.done();
}

bool SqliteDatabase::deleteStaleUploads(int64_t olderThan, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "DELETE FROM ttd_uploads WHERE updated_at < ? AND state <> 'hoan-tat'");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, olderThan);
    st.step();
    return st.done();
}

// ---------------------------------------------------------------------------
//  Cài đặt & thống kê
// ---------------------------------------------------------------------------
bool SqliteDatabase::getSetting(const std::string& key, std::string& value,
                                std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "SELECT svalue FROM ttd_settings WHERE skey=?");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, key);
    if (!st.step()) return false;
    value = st.colText(0);
    return true;
}

bool SqliteDatabase::setSetting(const std::string& key, const std::string& value,
                                std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "INSERT OR REPLACE INTO ttd_settings (skey, svalue) VALUES (?,?)");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindText(1, key);
    st.bindText(2, value);
    st.step();
    return st.done();
}

bool SqliteDatabase::listSettings(std::vector<std::pair<std::string, std::string>>& out,
                                  std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_, "SELECT skey, svalue FROM ttd_settings ORDER BY skey");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    while (st.step()) out.emplace_back(st.colText(0), st.colText(1));
    return true;
}

bool SqliteDatabase::stats(StorageStats& out, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    {
        Stmt st(db_,
                "SELECT COALESCE(SUM(size),0), COUNT(*) FROM ttd_entries WHERE is_folder=0 AND "
                "trashed=0");
        if (!st.ok()) {
            error = st.error();
            return false;
        }
        if (st.step()) {
            out.totalBytes = static_cast<uint64_t>(st.colInt(0));
            out.fileCount = static_cast<uint64_t>(st.colInt(1));
        }
    }
    {
        Stmt st(db_, "SELECT COUNT(*) FROM ttd_entries WHERE is_folder=1 AND trashed=0");
        if (st.ok() && st.step()) out.folderCount = static_cast<uint64_t>(st.colInt(0));
    }
    {
        Stmt st(db_,
                "SELECT COALESCE(SUM(size),0), COUNT(*) FROM ttd_entries WHERE is_folder=0 AND "
                "trashed=1");
        if (st.ok() && st.step()) {
            out.trashedBytes = static_cast<uint64_t>(st.colInt(0));
            out.trashedCount = static_cast<uint64_t>(st.colInt(1));
        }
    }
    {
        Stmt st(db_, "SELECT COUNT(*) FROM ttd_chunks");
        if (st.ok() && st.step()) out.chunkCount = static_cast<uint64_t>(st.colInt(0));
    }
    {
        // Dung lượng thật: gộp theo document_id để mảnh dùng chung (tệp trùng
        // nội dung) chỉ được tính một lần.
        Stmt st(db_,
                "SELECT COALESCE(SUM(size),0), COUNT(*) FROM (SELECT document_id, MAX(size) AS "
                "size FROM ttd_chunks GROUP BY document_id)");
        if (st.ok() && st.step()) {
            out.physicalBytes = static_cast<uint64_t>(st.colInt(0));
            out.uniqueChunkCount = static_cast<uint64_t>(st.colInt(1));
        }
    }
    return true;
}

bool SqliteDatabase::usageByUser(int userId, uint64_t& bytes, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt st(db_,
            "SELECT COALESCE(SUM(size),0) FROM ttd_entries WHERE owner_id=? AND is_folder=0 AND "
            "trashed=0");
    if (!st.ok()) {
        error = st.error();
        return false;
    }
    st.bindInt(1, userId);
    bytes = st.step() ? static_cast<uint64_t>(st.colInt(0)) : 0;
    return true;
}

}  // namespace db
}  // namespace ttd
