// Lớp trừu tượng cho cơ sở dữ liệu siêu dữ liệu (metadata).
// Có hai cài đặt: SQLite (tệp) và MySQL (giao thức gốc, không cần thư viện ngoài).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/strutil.h"

namespace ttd {
namespace db {

// ---------------------------------------------------------------------------
//  Mô hình dữ liệu
// ---------------------------------------------------------------------------
struct FileEntry {
    int64_t id = 0;
    int64_t parentId = 0;      // 0 = thư mục gốc
    std::string name;
    std::string path;          // đường dẫn ảo đầy đủ, ví dụ /Tài liệu/abc.pdf
    bool isFolder = false;
    uint64_t size = 0;
    std::string mimeType;
    std::string sha256;        // băm toàn tệp (rỗng nếu là thư mục)
    std::string quickHash;     // băm nhanh: 1 MB đầu + 1 MB cuối + kích thước
    uint64_t chunkSize = 0;
    int chunkCount = 0;
    int64_t createdAt = 0;
    int64_t modifiedAt = 0;
    int ownerId = 0;
    bool trashed = false;
    int64_t trashedAt = 0;
    std::string shareToken;    // rỗng nếu không chia sẻ
    int64_t shareExpiresAt = 0;
    bool starred = false;
    std::string note;
};

struct ChunkEntry {
    int64_t id = 0;
    int64_t fileId = 0;
    int index = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    int64_t messageId = 0;
    int64_t documentId = 0;
    int64_t accessHash = 0;
    std::string fileReferenceHex;
    int dcId = 0;
    int accountId = 0;
    std::string sha256;
    int64_t createdAt = 0;
};

struct UserEntry {
    int id = 0;
    std::string username;
    std::string displayName;
    std::string passwordHash;   // pbkdf2$iterations$salt$hash
    bool isAdmin = false;
    bool enabled = true;
    uint64_t quotaBytes = 0;    // 0 = không giới hạn
    int64_t createdAt = 0;
    int64_t lastLoginAt = 0;
};

struct AccountEntry {
    int id = 0;
    std::string label;
    std::string phone;
    std::string displayName;
    bool enabled = true;
    int homeDc = 2;
    int64_t createdAt = 0;
    int64_t lastUsedAt = 0;
    std::string note;
};

struct SessionKeyEntry {
    int accountId = 0;
    int dcId = 0;
    std::string authKeyHex;
    int64_t serverSalt = 0;
    int64_t updatedAt = 0;
};

struct UploadRecord {
    std::string id;
    int ownerId = 0;
    std::string name;
    std::string targetPath;
    uint64_t totalSize = 0;
    uint64_t receivedBytes = 0;
    uint64_t storedBytes = 0;
    int chunkCount = 0;
    std::string state;         // dang-tai | tam-dung | hoan-tat | huy | loi
    std::string message;
    std::string quickHash;
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

struct WebSession {
    std::string token;
    int userId = 0;
    int64_t createdAt = 0;
    int64_t expiresAt = 0;
    std::string userAgent;
    std::string ip;
};

struct ListOptions {
    int64_t parentId = 0;
    bool includeTrashed = false;
    bool onlyTrashed = false;
    bool onlyStarred = false;
    std::string search;         // tìm theo tên (không phân biệt hoa thường)
    std::string sortBy = "name";  // name | size | modified | created | type
    bool descending = false;
    int limit = 500;
    int offset = 0;
    int ownerId = 0;            // 0 = mọi người dùng (quản trị)
};

struct StorageStats {
    uint64_t totalBytes = 0;
    uint64_t fileCount = 0;
    uint64_t folderCount = 0;
    uint64_t chunkCount = 0;
    uint64_t trashedBytes = 0;
    uint64_t trashedCount = 0;
};

// ---------------------------------------------------------------------------
//  Giao diện cơ sở dữ liệu
// ---------------------------------------------------------------------------
class Database {
public:
    virtual ~Database() = default;

    virtual std::string kind() const = 0;
    virtual bool open(std::string& error) = 0;
    virtual void close() = 0;
    virtual bool migrate(std::string& error) = 0;
    virtual bool healthy() const = 0;
    virtual std::string description() const = 0;

    // --- Tệp & thư mục ---
    virtual bool createEntry(FileEntry& entry, std::string& error) = 0;
    virtual bool updateEntry(const FileEntry& entry, std::string& error) = 0;
    virtual bool deleteEntry(int64_t id, std::string& error) = 0;
    virtual bool getEntry(int64_t id, FileEntry& out, std::string& error) = 0;
    virtual bool getEntryByPath(const std::string& path, FileEntry& out, std::string& error) = 0;
    virtual bool listEntries(const ListOptions& opts, std::vector<FileEntry>& out,
                             std::string& error) = 0;
    virtual bool countEntries(const ListOptions& opts, uint64_t& out, std::string& error) = 0;
    virtual bool findByHash(const std::string& sha256, std::vector<FileEntry>& out,
                            std::string& error) = 0;
    virtual bool findByQuickHash(const std::string& quickHash, uint64_t size,
                                 std::vector<FileEntry>& out, std::string& error) = 0;
    virtual bool findByNameInFolder(int64_t parentId, const std::string& name, FileEntry& out,
                                    std::string& error) = 0;
    virtual bool listChildrenRecursive(int64_t folderId, std::vector<FileEntry>& out,
                                       std::string& error) = 0;
    virtual bool updatePathsUnder(const std::string& oldPrefix, const std::string& newPrefix,
                                  std::string& error) = 0;
    virtual bool getEntryByShareToken(const std::string& token, FileEntry& out,
                                      std::string& error) = 0;

    // --- Mảnh dữ liệu ---
    virtual bool addChunk(ChunkEntry& chunk, std::string& error) = 0;
    virtual bool listChunks(int64_t fileId, std::vector<ChunkEntry>& out,
                            std::string& error) = 0;
    virtual bool deleteChunks(int64_t fileId, std::string& error) = 0;
    virtual bool updateChunkReference(int64_t chunkId, const std::string& fileReferenceHex,
                                      int64_t accessHash, int dcId, std::string& error) = 0;
    // Đếm số tệp còn tham chiếu tới cùng nội dung (khử trùng lặp).
    virtual bool countFilesWithHash(const std::string& sha256, uint64_t& out,
                                    std::string& error) = 0;

    // --- Người dùng ---
    virtual bool createUser(UserEntry& user, std::string& error) = 0;
    virtual bool updateUser(const UserEntry& user, std::string& error) = 0;
    virtual bool deleteUser(int id, std::string& error) = 0;
    virtual bool getUser(int id, UserEntry& out, std::string& error) = 0;
    virtual bool getUserByName(const std::string& username, UserEntry& out,
                               std::string& error) = 0;
    virtual bool listUsers(std::vector<UserEntry>& out, std::string& error) = 0;
    virtual bool countUsers(uint64_t& out, std::string& error) = 0;

    // --- Phiên đăng nhập web ---
    virtual bool createSession(const WebSession& session, std::string& error) = 0;
    virtual bool getSession(const std::string& token, WebSession& out, std::string& error) = 0;
    virtual bool deleteSession(const std::string& token, std::string& error) = 0;
    virtual bool deleteExpiredSessions(std::string& error) = 0;
    virtual bool deleteSessionsOfUser(int userId, std::string& error) = 0;

    // --- Tài khoản Telegram ---
    virtual bool createAccount(AccountEntry& account, std::string& error) = 0;
    virtual bool updateAccount(const AccountEntry& account, std::string& error) = 0;
    virtual bool deleteAccount(int id, std::string& error) = 0;
    virtual bool listAccounts(std::vector<AccountEntry>& out, std::string& error) = 0;
    virtual bool saveSessionKey(const SessionKeyEntry& key, std::string& error) = 0;
    virtual bool listSessionKeys(int accountId, std::vector<SessionKeyEntry>& out,
                                 std::string& error) = 0;
    virtual bool deleteSessionKeys(int accountId, std::string& error) = 0;

    // --- Phiên tải lên ---
    virtual bool saveUpload(const UploadRecord& rec, std::string& error) = 0;
    virtual bool getUpload(const std::string& id, UploadRecord& out, std::string& error) = 0;
    virtual bool listUploads(int ownerId, std::vector<UploadRecord>& out,
                             std::string& error) = 0;
    virtual bool deleteUpload(const std::string& id, std::string& error) = 0;
    virtual bool deleteStaleUploads(int64_t olderThan, std::string& error) = 0;

    // --- Cài đặt ---
    virtual bool getSetting(const std::string& key, std::string& value, std::string& error) = 0;
    virtual bool setSetting(const std::string& key, const std::string& value,
                            std::string& error) = 0;
    virtual bool listSettings(std::vector<std::pair<std::string, std::string>>& out,
                              std::string& error) = 0;

    // --- Thống kê ---
    virtual bool stats(StorageStats& out, std::string& error) = 0;
    virtual bool usageByUser(int userId, uint64_t& bytes, std::string& error) = 0;
};

// Tạo đối tượng cơ sở dữ liệu theo cấu hình.
struct DatabaseConfig {
    std::string kind = "sqlite";  // sqlite | mysql
    std::string sqlitePath = "data/tuan-telegram-disk.db";
    std::string mysqlHost = "127.0.0.1";
    uint16_t mysqlPort = 3306;
    std::string mysqlUser = "root";
    std::string mysqlPassword;
    std::string mysqlDatabase = "tuan_telegram_disk";
    std::string mysqlCharset = "utf8mb4";
    int mysqlTimeoutMs = 15000;
};

std::unique_ptr<Database> createDatabase(const DatabaseConfig& config, std::string& error);

// Câu lệnh tạo bảng (dùng chung, có biến thể cho từng loại cơ sở dữ liệu).
std::vector<std::string> schemaStatements(const std::string& kind);

}  // namespace db
}  // namespace ttd
