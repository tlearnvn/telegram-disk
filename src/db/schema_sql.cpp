#include "db/database.h"

namespace ttd {
namespace db {

std::vector<std::string> schemaStatements(const std::string& kind) {
    const bool mysql = (kind == "mysql");

    // Kiểu dữ liệu khác nhau giữa SQLite và MySQL.
    const char* pk = mysql ? "BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY"
                           : "INTEGER PRIMARY KEY AUTOINCREMENT";
    const char* pkInt = mysql ? "INT NOT NULL AUTO_INCREMENT PRIMARY KEY"
                              : "INTEGER PRIMARY KEY AUTOINCREMENT";
    const char* text = mysql ? "VARCHAR(1024)" : "TEXT";
    const char* textShort = mysql ? "VARCHAR(255)" : "TEXT";
    const char* textLong = mysql ? "TEXT" : "TEXT";
    const char* bigint = "BIGINT";
    const char* boolean = mysql ? "TINYINT(1)" : "INTEGER";
    const char* suffix = mysql ? " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci"
                               : "";

    std::vector<std::string> out;
    auto add = [&](const std::string& s) { out.push_back(s); };
    // MySQL không hỗ trợ "CREATE INDEX IF NOT EXISTS"; trình điều khiển MySQL sẽ
    // bỏ qua lỗi 1061 (chỉ mục đã tồn tại) nên cứ phát lệnh không có mệnh đề đó.
    auto addIndex = [&](const std::string& name, const std::string& tableAndCols) {
        if (mysql)
            out.push_back("CREATE INDEX " + name + " ON " + tableAndCols);
        else
            out.push_back("CREATE INDEX IF NOT EXISTS " + name + " ON " + tableAndCols);
    };

    add(std::string("CREATE TABLE IF NOT EXISTS ttd_users (") +
        "  id " + pkInt + "," +
        "  username " + (mysql ? "VARCHAR(190)" : "TEXT") + " NOT NULL," +
        "  display_name " + textShort + "," +
        "  password_hash " + text + " NOT NULL," +
        "  is_admin " + boolean + " NOT NULL DEFAULT 0," +
        "  enabled " + boolean + " NOT NULL DEFAULT 1," +
        "  quota_bytes " + bigint + " NOT NULL DEFAULT 0," +
        "  created_at " + bigint + " NOT NULL DEFAULT 0," +
        "  last_login_at " + bigint + " NOT NULL DEFAULT 0," +
        std::string(mysql ? "  UNIQUE KEY uq_users_username (username)" : "  UNIQUE (username)") +
        ")" + suffix);

    add(std::string("CREATE TABLE IF NOT EXISTS ttd_entries (") +
        "  id " + pk + "," +
        "  parent_id " + bigint + " NOT NULL DEFAULT 0," +
        "  name " + (mysql ? "VARCHAR(512)" : "TEXT") + " NOT NULL," +
        "  path " + (mysql ? "VARCHAR(1024)" : "TEXT") + " NOT NULL," +
        "  is_folder " + boolean + " NOT NULL DEFAULT 0," +
        "  size " + bigint + " NOT NULL DEFAULT 0," +
        "  mime_type " + textShort + "," +
        "  sha256 " + (mysql ? "VARCHAR(64)" : "TEXT") + "," +
        "  quick_hash " + (mysql ? "VARCHAR(64)" : "TEXT") + "," +
        "  chunk_size " + bigint + " NOT NULL DEFAULT 0," +
        "  chunk_count INT NOT NULL DEFAULT 0," +
        "  created_at " + bigint + " NOT NULL DEFAULT 0," +
        "  modified_at " + bigint + " NOT NULL DEFAULT 0," +
        "  owner_id INT NOT NULL DEFAULT 0," +
        "  trashed " + boolean + " NOT NULL DEFAULT 0," +
        "  trashed_at " + bigint + " NOT NULL DEFAULT 0," +
        "  share_token " + (mysql ? "VARCHAR(64)" : "TEXT") + "," +
        "  share_expires_at " + bigint + " NOT NULL DEFAULT 0," +
        "  starred " + boolean + " NOT NULL DEFAULT 0," +
        "  note " + textLong +
        ")" + suffix);

    addIndex("idx_entries_parent", "ttd_entries (parent_id)");
    addIndex("idx_entries_owner", "ttd_entries (owner_id)");
    addIndex("idx_entries_sha", mysql ? "ttd_entries (sha256)" : "ttd_entries (sha256)");
    addIndex("idx_entries_quick", "ttd_entries (quick_hash)");
    addIndex("idx_entries_trashed", "ttd_entries (trashed)");
    addIndex("idx_entries_path", mysql ? "ttd_entries (path(255))" : "ttd_entries (path)");
    addIndex("idx_entries_share", "ttd_entries (share_token)");

    add(std::string("CREATE TABLE IF NOT EXISTS ttd_chunks (") +
        "  id " + pk + "," +
        "  file_id " + bigint + " NOT NULL," +
        "  idx INT NOT NULL," +
        "  offset_bytes " + bigint + " NOT NULL DEFAULT 0," +
        "  size " + bigint + " NOT NULL DEFAULT 0," +
        "  message_id " + bigint + " NOT NULL DEFAULT 0," +
        "  document_id " + bigint + " NOT NULL DEFAULT 0," +
        "  access_hash " + bigint + " NOT NULL DEFAULT 0," +
        "  file_reference " + text + "," +
        "  dc_id INT NOT NULL DEFAULT 0," +
        "  account_id INT NOT NULL DEFAULT 0," +
        "  sha256 " + (mysql ? "VARCHAR(64)" : "TEXT") + "," +
        "  created_at " + bigint + " NOT NULL DEFAULT 0" +
        ")" + suffix);
    addIndex("idx_chunks_file", "ttd_chunks (file_id)");

    add(std::string("CREATE TABLE IF NOT EXISTS ttd_sessions (") +
        "  token " + (mysql ? "VARCHAR(128)" : "TEXT") + " NOT NULL PRIMARY KEY," +
        "  user_id INT NOT NULL," +
        "  created_at " + bigint + " NOT NULL DEFAULT 0," +
        "  expires_at " + bigint + " NOT NULL DEFAULT 0," +
        "  user_agent " + text + "," +
        "  ip " + textShort +
        ")" + suffix);
    addIndex("idx_sessions_user", "ttd_sessions (user_id)");

    add(std::string("CREATE TABLE IF NOT EXISTS ttd_accounts (") +
        "  id " + pkInt + "," +
        "  label " + textShort + " NOT NULL," +
        "  phone " + (mysql ? "VARCHAR(64)" : "TEXT") + "," +
        "  display_name " + textShort + "," +
        "  enabled " + boolean + " NOT NULL DEFAULT 1," +
        "  home_dc INT NOT NULL DEFAULT 2," +
        "  created_at " + bigint + " NOT NULL DEFAULT 0," +
        "  last_used_at " + bigint + " NOT NULL DEFAULT 0," +
        "  note " + textLong +
        ")" + suffix);

    add(std::string("CREATE TABLE IF NOT EXISTS ttd_session_keys (") +
        "  account_id INT NOT NULL," +
        "  dc_id INT NOT NULL," +
        "  auth_key " + textLong + " NOT NULL," +
        "  server_salt " + bigint + " NOT NULL DEFAULT 0," +
        "  updated_at " + bigint + " NOT NULL DEFAULT 0," +
        "  PRIMARY KEY (account_id, dc_id)" +
        ")" + suffix);

    add(std::string("CREATE TABLE IF NOT EXISTS ttd_uploads (") +
        "  id " + (mysql ? "VARCHAR(64)" : "TEXT") + " NOT NULL PRIMARY KEY," +
        "  owner_id INT NOT NULL DEFAULT 0," +
        "  name " + (mysql ? "VARCHAR(512)" : "TEXT") + "," +
        "  target_path " + (mysql ? "VARCHAR(1024)" : "TEXT") + "," +
        "  total_size " + bigint + " NOT NULL DEFAULT 0," +
        "  received_bytes " + bigint + " NOT NULL DEFAULT 0," +
        "  stored_bytes " + bigint + " NOT NULL DEFAULT 0," +
        "  chunk_count INT NOT NULL DEFAULT 0," +
        "  state " + (mysql ? "VARCHAR(32)" : "TEXT") + "," +
        "  message " + text + "," +
        "  quick_hash " + (mysql ? "VARCHAR(64)" : "TEXT") + "," +
        "  created_at " + bigint + " NOT NULL DEFAULT 0," +
        "  updated_at " + bigint + " NOT NULL DEFAULT 0" +
        ")" + suffix);

    add(std::string("CREATE TABLE IF NOT EXISTS ttd_settings (") +
        "  skey " + (mysql ? "VARCHAR(190)" : "TEXT") + " NOT NULL PRIMARY KEY," +
        "  svalue " + textLong +
        ")" + suffix);

    return out;
}

}  // namespace db
}  // namespace ttd
