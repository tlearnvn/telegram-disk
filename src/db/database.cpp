#include "db/database.h"

#include "common/logging.h"
#include "common/strutil.h"
#include "db/mysql_database.h"
#include "db/sqlite_database.h"

namespace ttd {
namespace db {

std::unique_ptr<Database> createDatabase(const DatabaseConfig& config, std::string& error) {
    std::string kind = toLower(trim(config.kind));
    if (kind.empty() || kind == "sqlite" || kind == "sqlite3" || kind == "tep") {
        return std::unique_ptr<Database>(new SqliteDatabase(config.sqlitePath));
    }
    if (kind == "mysql" || kind == "mariadb") {
        return std::unique_ptr<Database>(new MysqlDatabase(config));
    }
    error = "Loại cơ sở dữ liệu không hỗ trợ: '" + config.kind +
            "'. Chỉ chấp nhận 'sqlite' hoặc 'mysql'.";
    return nullptr;
}

}  // namespace db
}  // namespace ttd
