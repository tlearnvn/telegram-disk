// Trình điều khiển MySQL viết trực tiếp theo giao thức mạng của MySQL/MariaDB.
// Nhờ vậy tệp thực thi không cần libmysqlclient — vẫn chạy độc lập.
//
// Hỗ trợ: bắt tay v10, xác thực mysql_native_password và caching_sha2_password
// (cả đường nhanh lẫn đường đầy đủ có mã hoá RSA-OAEP), truy vấn văn bản
// (COM_QUERY), đọc bảng kết quả, COM_PING và COM_QUIT.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/net.h"
#include "common/strutil.h"

namespace ttd {
namespace db {

struct MysqlField {
    std::string name;
    std::string table;
    uint8_t type = 0;
};

struct MysqlRow {
    // Giá trị NULL được biểu diễn bằng isNull[i] = true.
    std::vector<std::string> values;
    std::vector<bool> isNull;

    const std::string& at(size_t i) const {
        static const std::string kEmpty;
        return i < values.size() ? values[i] : kEmpty;
    }
    bool null(size_t i) const { return i < isNull.size() ? isNull[i] : true; }
};

struct MysqlResult {
    bool ok = false;
    std::string error;
    uint16_t errorCode = 0;
    std::string sqlState;
    uint64_t affectedRows = 0;
    uint64_t lastInsertId = 0;
    std::vector<MysqlField> fields;
    std::vector<MysqlRow> rows;

    int columnIndex(const std::string& name) const;
};

struct MysqlConnectionParams {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user = "root";
    std::string password;
    std::string database;
    std::string charset = "utf8mb4";
    int timeoutMs = 15000;
};

class MysqlConnection {
public:
    MysqlConnection() = default;
    ~MysqlConnection();

    bool connect(const MysqlConnectionParams& params, std::string& error);
    void close();
    bool connected() const { return socket_.valid(); }

    MysqlResult query(const std::string& sql);
    bool ping(std::string& error);
    const std::string& serverVersion() const { return serverVersion_; }

    // Bọc chuỗi thành hằng SQL an toàn (kèm dấu nháy).
    static std::string quote(const std::string& value);
    // Bọc dữ liệu nhị phân thành X'...' của MySQL.
    static std::string quoteBlob(const Bytes& value);

private:
    bool readPacket(Bytes& payload, uint8_t& sequence, std::string& error);
    bool writePacket(const Bytes& payload, uint8_t sequence, std::string& error);
    bool handshake(const MysqlConnectionParams& params, std::string& error);
    bool authenticate(const MysqlConnectionParams& params, const Bytes& scramble,
                      const std::string& plugin, uint8_t& seq, std::string& error);
    MysqlResult readQueryResponse();

    net::TcpSocket socket_;
    MysqlConnectionParams params_;
    std::string serverVersion_;
    uint32_t capabilities_ = 0;
    uint8_t seq_ = 0;
};

}  // namespace db
}  // namespace ttd
