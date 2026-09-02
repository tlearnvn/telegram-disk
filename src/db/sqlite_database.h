// Cài đặt cơ sở dữ liệu bằng SQLite (bản amalgamation đi kèm trong mã nguồn).
#pragma once

#include <mutex>
#include <string>

#include "db/database.h"

struct sqlite3;
struct sqlite3_stmt;

namespace ttd {
namespace db {

class SqliteDatabase : public Database {
public:
    explicit SqliteDatabase(std::string path);
    ~SqliteDatabase() override;

    std::string kind() const override { return "sqlite"; }
    bool open(std::string& error) override;
    void close() override;
    bool migrate(std::string& error) override;
    bool healthy() const override;
    std::string description() const override;

    bool createEntry(FileEntry& entry, std::string& error) override;
    bool updateEntry(const FileEntry& entry, std::string& error) override;
    bool deleteEntry(int64_t id, std::string& error) override;
    bool getEntry(int64_t id, FileEntry& out, std::string& error) override;
    bool getEntryByPath(const std::string& path, FileEntry& out, std::string& error) override;
    bool listEntries(const ListOptions& opts, std::vector<FileEntry>& out,
                     std::string& error) override;
    bool countEntries(const ListOptions& opts, uint64_t& out, std::string& error) override;
    bool findByHash(const std::string& sha256, std::vector<FileEntry>& out,
                    std::string& error) override;
    bool findByQuickHash(const std::string& quickHash, uint64_t size,
                         std::vector<FileEntry>& out, std::string& error) override;
    bool findByNameInFolder(int64_t parentId, const std::string& name, FileEntry& out,
                            std::string& error) override;
    bool listChildrenRecursive(int64_t folderId, std::vector<FileEntry>& out,
                               std::string& error) override;
    bool updatePathsUnder(const std::string& oldPrefix, const std::string& newPrefix,
                          std::string& error) override;
    bool getEntryByShareToken(const std::string& token, FileEntry& out,
                              std::string& error) override;

    bool addChunk(ChunkEntry& chunk, std::string& error) override;
    bool listChunks(int64_t fileId, std::vector<ChunkEntry>& out, std::string& error) override;
    bool deleteChunks(int64_t fileId, std::string& error) override;
    bool updateChunkReference(int64_t chunkId, const std::string& fileReferenceHex,
                              int64_t accessHash, int dcId, int accountId,
                              std::string& error) override;
    bool countFilesWithHash(const std::string& sha256, uint64_t& out,
                            std::string& error) override;

    bool createUser(UserEntry& user, std::string& error) override;
    bool updateUser(const UserEntry& user, std::string& error) override;
    bool deleteUser(int id, std::string& error) override;
    bool getUser(int id, UserEntry& out, std::string& error) override;
    bool getUserByName(const std::string& username, UserEntry& out, std::string& error) override;
    bool listUsers(std::vector<UserEntry>& out, std::string& error) override;
    bool countUsers(uint64_t& out, std::string& error) override;

    bool createSession(const WebSession& session, std::string& error) override;
    bool getSession(const std::string& token, WebSession& out, std::string& error) override;
    bool deleteSession(const std::string& token, std::string& error) override;
    bool deleteExpiredSessions(std::string& error) override;
    bool deleteSessionsOfUser(int userId, std::string& error) override;

    bool createAccount(AccountEntry& account, std::string& error) override;
    bool updateAccount(const AccountEntry& account, std::string& error) override;
    bool deleteAccount(int id, std::string& error) override;
    bool listAccounts(std::vector<AccountEntry>& out, std::string& error) override;
    bool saveSessionKey(const SessionKeyEntry& key, std::string& error) override;
    bool listSessionKeys(int accountId, std::vector<SessionKeyEntry>& out,
                         std::string& error) override;
    bool deleteSessionKeys(int accountId, std::string& error) override;

    bool saveUpload(const UploadRecord& rec, std::string& error) override;
    bool getUpload(const std::string& id, UploadRecord& out, std::string& error) override;
    bool listUploads(int ownerId, std::vector<UploadRecord>& out, std::string& error) override;
    bool deleteUpload(const std::string& id, std::string& error) override;
    bool deleteStaleUploads(int64_t olderThan, std::string& error) override;

    bool getSetting(const std::string& key, std::string& value, std::string& error) override;
    bool setSetting(const std::string& key, const std::string& value,
                    std::string& error) override;
    bool listSettings(std::vector<std::pair<std::string, std::string>>& out,
                      std::string& error) override;

    bool stats(StorageStats& out, std::string& error) override;
    bool usageByUser(int userId, uint64_t& bytes, std::string& error) override;

private:
    bool exec(const std::string& sql, std::string& error);
    std::string lastError() const;

    std::string path_;
    sqlite3* db_ = nullptr;
    mutable std::mutex mu_;
};

}  // namespace db
}  // namespace ttd
