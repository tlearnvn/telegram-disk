#include "db/mysql_protocol.h"

#include <cstring>

#include "common/logging.h"
#include "crypto/hash.h"
#include "crypto/rsa.h"

namespace ttd {
namespace db {

namespace {
constexpr const char* kTag = "db.mysql";

// Cờ khả năng của máy khách.
constexpr uint32_t CLIENT_LONG_PASSWORD = 0x00000001;
constexpr uint32_t CLIENT_FOUND_ROWS = 0x00000002;
constexpr uint32_t CLIENT_LONG_FLAG = 0x00000004;
constexpr uint32_t CLIENT_CONNECT_WITH_DB = 0x00000008;
constexpr uint32_t CLIENT_LOCAL_FILES = 0x00000080;
constexpr uint32_t CLIENT_PROTOCOL_41 = 0x00000200;
constexpr uint32_t CLIENT_INTERACTIVE = 0x00000400;
constexpr uint32_t CLIENT_TRANSACTIONS = 0x00002000;
constexpr uint32_t CLIENT_SECURE_CONNECTION = 0x00008000;
constexpr uint32_t CLIENT_MULTI_STATEMENTS = 0x00010000;
constexpr uint32_t CLIENT_MULTI_RESULTS = 0x00020000;
constexpr uint32_t CLIENT_PLUGIN_AUTH = 0x00080000;
constexpr uint32_t CLIENT_CONNECT_ATTRS = 0x00100000;
constexpr uint32_t CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA = 0x00200000;
constexpr uint32_t CLIENT_DEPRECATE_EOF = 0x01000000;

void putUInt8(Bytes& b, uint8_t v) { b.push_back(v); }
void putUInt16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xff));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
}
void putUInt32(Bytes& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
}
void putString(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
void putNulString(Bytes& b, const std::string& s) {
    putString(b, s);
    b.push_back(0);
}
void putLenEncBytes(Bytes& b, const Bytes& data) {
    size_t n = data.size();
    if (n < 251) {
        b.push_back(static_cast<uint8_t>(n));
    } else if (n < 65536) {
        b.push_back(0xfc);
        putUInt16(b, static_cast<uint16_t>(n));
    } else if (n < 16777216) {
        b.push_back(0xfd);
        b.push_back(static_cast<uint8_t>(n & 0xff));
        b.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
        b.push_back(static_cast<uint8_t>((n >> 16) & 0xff));
    } else {
        b.push_back(0xfe);
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((n >> (8 * i)) & 0xff));
    }
    b.insert(b.end(), data.begin(), data.end());
}

class Reader {
public:
    Reader(const Bytes& b) : d_(b.data()), n_(b.size()) {}

    bool u8(uint8_t& v) {
        if (p_ + 1 > n_) return false;
        v = d_[p_++];
        return true;
    }
    bool u16(uint16_t& v) {
        if (p_ + 2 > n_) return false;
        v = static_cast<uint16_t>(d_[p_] | (d_[p_ + 1] << 8));
        p_ += 2;
        return true;
    }
    bool u24(uint32_t& v) {
        if (p_ + 3 > n_) return false;
        v = static_cast<uint32_t>(d_[p_]) | (static_cast<uint32_t>(d_[p_ + 1]) << 8) |
            (static_cast<uint32_t>(d_[p_ + 2]) << 16);
        p_ += 3;
        return true;
    }
    bool u32(uint32_t& v) {
        if (p_ + 4 > n_) return false;
        v = static_cast<uint32_t>(d_[p_]) | (static_cast<uint32_t>(d_[p_ + 1]) << 8) |
            (static_cast<uint32_t>(d_[p_ + 2]) << 16) | (static_cast<uint32_t>(d_[p_ + 3]) << 24);
        p_ += 4;
        return true;
    }
    bool nulString(std::string& v) {
        size_t start = p_;
        while (p_ < n_ && d_[p_] != 0) ++p_;
        if (p_ >= n_) return false;
        v.assign(reinterpret_cast<const char*>(d_ + start), p_ - start);
        ++p_;
        return true;
    }
    bool fixedString(size_t len, std::string& v) {
        if (p_ + len > n_) return false;
        v.assign(reinterpret_cast<const char*>(d_ + p_), len);
        p_ += len;
        return true;
    }
    bool fixedBytes(size_t len, Bytes& v) {
        if (p_ + len > n_) return false;
        v.assign(d_ + p_, d_ + p_ + len);
        p_ += len;
        return true;
    }
    // Số nguyên mã hoá theo độ dài. isNull = true khi gặp 0xfb.
    bool lenEncInt(uint64_t& v, bool& isNull) {
        isNull = false;
        uint8_t first;
        if (!u8(first)) return false;
        if (first < 251) {
            v = first;
            return true;
        }
        if (first == 0xfb) {
            isNull = true;
            v = 0;
            return true;
        }
        if (first == 0xfc) {
            uint16_t t;
            if (!u16(t)) return false;
            v = t;
            return true;
        }
        if (first == 0xfd) {
            uint32_t t;
            if (!u24(t)) return false;
            v = t;
            return true;
        }
        if (first == 0xfe) {
            if (p_ + 8 > n_) return false;
            v = 0;
            for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(d_[p_ + static_cast<size_t>(i)])
                                            << (8 * i);
            p_ += 8;
            return true;
        }
        return false;
    }
    bool lenEncString(std::string& v, bool& isNull) {
        uint64_t len;
        if (!lenEncInt(len, isNull)) return false;
        if (isNull) {
            v.clear();
            return true;
        }
        return fixedString(static_cast<size_t>(len), v);
    }
    bool skip(size_t n) {
        if (p_ + n > n_) return false;
        p_ += n;
        return true;
    }
    size_t remaining() const { return n_ > p_ ? n_ - p_ : 0; }
    size_t position() const { return p_; }

private:
    const uint8_t* d_;
    size_t n_;
    size_t p_ = 0;
};

// mysql_native_password: SHA1(pwd) XOR SHA1(scramble + SHA1(SHA1(pwd)))
Bytes nativePasswordAuth(const std::string& password, const Bytes& scramble) {
    if (password.empty()) return Bytes();
    Bytes stage1 = crypto::Sha1::hash(password);
    Bytes stage2 = crypto::Sha1::hash(stage1);
    Bytes concat = scramble;
    concat.insert(concat.end(), stage2.begin(), stage2.end());
    Bytes hash = crypto::Sha1::hash(concat);
    Bytes out(20);
    for (size_t i = 0; i < 20; ++i) out[i] = static_cast<uint8_t>(stage1[i] ^ hash[i]);
    return out;
}

// caching_sha2_password: SHA256(pwd) XOR SHA256(SHA256(SHA256(pwd)) + scramble)
Bytes cachingSha2Auth(const std::string& password, const Bytes& scramble) {
    if (password.empty()) return Bytes();
    Bytes stage1 = crypto::Sha256::hash(password);
    Bytes stage2 = crypto::Sha256::hash(stage1);
    Bytes concat = stage2;
    concat.insert(concat.end(), scramble.begin(), scramble.end());
    Bytes hash = crypto::Sha256::hash(concat);
    Bytes out(32);
    for (size_t i = 0; i < 32; ++i) out[i] = static_cast<uint8_t>(stage1[i] ^ hash[i]);
    return out;
}

}  // namespace

int MysqlResult::columnIndex(const std::string& name) const {
    for (size_t i = 0; i < fields.size(); ++i)
        if (fields[i].name == name) return static_cast<int>(i);
    return -1;
}

MysqlConnection::~MysqlConnection() { close(); }

void MysqlConnection::close() {
    if (socket_.valid()) {
        // Gửi COM_QUIT cho lịch sự (bỏ qua lỗi).
        Bytes quit;
        quit.push_back(0x01);
        std::string err;
        writePacket(quit, 0, err);
        socket_.shutdownBoth();
        socket_.close();
    }
}

bool MysqlConnection::readPacket(Bytes& payload, uint8_t& sequence, std::string& error) {
    uint8_t header[4];
    if (!socket_.recvAll(header, 4, params_.timeoutMs)) {
        error = "Mất kết nối tới máy chủ MySQL khi đọc tiêu đề gói tin";
        return false;
    }
    uint32_t len = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
                   (static_cast<uint32_t>(header[2]) << 16);
    sequence = header[3];
    payload.resize(len);
    if (len > 0 && !socket_.recvAll(payload.data(), len, params_.timeoutMs)) {
        error = "Mất kết nối tới máy chủ MySQL khi đọc phần thân gói tin";
        return false;
    }
    // Gói tin dài hơn 16 MB được chia nhiều phần liên tiếp.
    while (len == 0xffffff) {
        if (!socket_.recvAll(header, 4, params_.timeoutMs)) {
            error = "Mất kết nối khi đọc gói tin nối tiếp";
            return false;
        }
        len = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
              (static_cast<uint32_t>(header[2]) << 16);
        sequence = header[3];
        size_t before = payload.size();
        payload.resize(before + len);
        if (len > 0 && !socket_.recvAll(payload.data() + before, len, params_.timeoutMs)) {
            error = "Mất kết nối khi đọc phần nối tiếp";
            return false;
        }
    }
    return true;
}

bool MysqlConnection::writePacket(const Bytes& payload, uint8_t sequence, std::string& error) {
    size_t offset = 0;
    do {
        size_t chunk = std::min<size_t>(0xffffff, payload.size() - offset);
        Bytes out;
        out.reserve(chunk + 4);
        out.push_back(static_cast<uint8_t>(chunk & 0xff));
        out.push_back(static_cast<uint8_t>((chunk >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>((chunk >> 16) & 0xff));
        out.push_back(sequence++);
        out.insert(out.end(), payload.begin() + static_cast<long>(offset),
                   payload.begin() + static_cast<long>(offset + chunk));
        if (!socket_.sendAll(out, params_.timeoutMs)) {
            error = "Không gửi được dữ liệu tới MySQL";
            return false;
        }
        offset += chunk;
    } while (offset < payload.size());
    seq_ = sequence;
    return true;
}

bool MysqlConnection::connect(const MysqlConnectionParams& params, std::string& error) {
    close();
    params_ = params;
    net::initNetworking();
    if (!socket_.connect(params.host, params.port, params.timeoutMs, error)) {
        error = "Không kết nối được tới MySQL " + params.host + ":" +
                std::to_string(params.port) + " — " + error;
        return false;
    }
    if (!handshake(params, error)) {
        socket_.close();
        return false;
    }
    LOG_INFO(kTag, "Đã kết nối MySQL %s tại %s:%u", serverVersion_.c_str(), params.host.c_str(),
             static_cast<unsigned>(params.port));
    return true;
}

bool MysqlConnection::handshake(const MysqlConnectionParams& params, std::string& error) {
    Bytes packet;
    uint8_t seq = 0;
    if (!readPacket(packet, seq, error)) return false;
    if (packet.empty()) {
        error = "Máy chủ MySQL gửi gói bắt tay rỗng";
        return false;
    }
    if (packet[0] == 0xff) {
        Reader r(packet);
        uint8_t tag;
        uint16_t code = 0;
        r.u8(tag);
        r.u16(code);
        std::string msg;
        r.fixedString(r.remaining(), msg);
        error = "MySQL từ chối kết nối: " + msg;
        return false;
    }

    Reader r(packet);
    uint8_t protocolVersion;
    if (!r.u8(protocolVersion) || protocolVersion != 10) {
        error = "Chỉ hỗ trợ giao thức MySQL phiên bản 10";
        return false;
    }
    if (!r.nulString(serverVersion_)) {
        error = "Gói bắt tay không hợp lệ";
        return false;
    }
    uint32_t connectionId;
    r.u32(connectionId);
    Bytes scramble;
    Bytes part1;
    if (!r.fixedBytes(8, part1)) {
        error = "Gói bắt tay thiếu dữ liệu";
        return false;
    }
    scramble = part1;
    r.skip(1);  // filler

    uint16_t capLower = 0;
    r.u16(capLower);
    uint8_t charset = 0;
    uint16_t status = 0;
    uint16_t capUpper = 0;
    std::string plugin = "mysql_native_password";
    if (r.remaining() > 0) {
        r.u8(charset);
        r.u16(status);
        r.u16(capUpper);
        uint8_t authDataLen = 0;
        r.u8(authDataLen);
        r.skip(10);  // reserved
        uint32_t serverCaps = static_cast<uint32_t>(capLower) |
                              (static_cast<uint32_t>(capUpper) << 16);
        if (serverCaps & CLIENT_SECURE_CONNECTION) {
            size_t len = authDataLen > 8 ? static_cast<size_t>(authDataLen) - 8 : 12;
            if (len < 12) len = 12;
            Bytes part2;
            if (r.fixedBytes(len, part2)) {
                // Bỏ byte NUL cuối nếu có.
                while (!part2.empty() && part2.back() == 0) part2.pop_back();
                scramble.insert(scramble.end(), part2.begin(), part2.end());
            }
        }
        if (serverCaps & CLIENT_PLUGIN_AUTH) {
            std::string p;
            if (r.nulString(p) && !p.empty()) plugin = p;
        }
        capabilities_ = serverCaps;
    } else {
        capabilities_ = capLower;
    }

    seq_ = static_cast<uint8_t>(seq + 1);
    return authenticate(params, scramble, plugin, seq_, error);
}

bool MysqlConnection::authenticate(const MysqlConnectionParams& params, const Bytes& scramble,
                                   const std::string& plugin, uint8_t& seq,
                                   std::string& error) {
    uint32_t clientFlags = CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_PROTOCOL_41 |
                           CLIENT_TRANSACTIONS | CLIENT_SECURE_CONNECTION | CLIENT_PLUGIN_AUTH |
                           CLIENT_MULTI_RESULTS | CLIENT_DEPRECATE_EOF |
                           CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA;
    if (!params.database.empty()) clientFlags |= CLIENT_CONNECT_WITH_DB;
    clientFlags &= (capabilities_ | CLIENT_LONG_PASSWORD | CLIENT_PROTOCOL_41 |
                    CLIENT_SECURE_CONNECTION | CLIENT_PLUGIN_AUTH | CLIENT_CONNECT_WITH_DB |
                    CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA);

    Bytes authResponse;
    std::string usePlugin = plugin;
    if (plugin == "caching_sha2_password") {
        authResponse = cachingSha2Auth(params.password, scramble);
    } else {
        usePlugin = "mysql_native_password";
        authResponse = nativePasswordAuth(params.password, scramble);
    }

    Bytes packet;
    putUInt32(packet, clientFlags);
    putUInt32(packet, 64 * 1024 * 1024);  // kích thước gói tối đa
    putUInt8(packet, 45);                 // utf8mb4_general_ci
    packet.insert(packet.end(), 23, 0);   // reserved
    putNulString(packet, params.user);
    if (clientFlags & CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA) {
        putLenEncBytes(packet, authResponse);
    } else {
        putUInt8(packet, static_cast<uint8_t>(authResponse.size()));
        packet.insert(packet.end(), authResponse.begin(), authResponse.end());
    }
    if (clientFlags & CLIENT_CONNECT_WITH_DB) putNulString(packet, params.database);
    if (clientFlags & CLIENT_PLUGIN_AUTH) putNulString(packet, usePlugin);

    if (!writePacket(packet, seq, error)) return false;

    // Vòng lặp xử lý các bước xác thực tiếp theo.
    for (int round = 0; round < 6; ++round) {
        Bytes resp;
        uint8_t rseq = 0;
        if (!readPacket(resp, rseq, error)) return false;
        if (resp.empty()) {
            error = "Máy chủ MySQL gửi phản hồi rỗng khi xác thực";
            return false;
        }
        uint8_t tag = resp[0];

        if (tag == 0x00) return true;  // OK

        if (tag == 0xff) {
            Reader r(resp);
            uint8_t t = 0;
            uint16_t code = 0;
            r.u8(t);
            r.u16(code);
            std::string state;
            if (resp.size() > 3 && resp[3] == '#') {
                r.skip(1);
                r.fixedString(5, state);
            }
            std::string msg;
            r.fixedString(r.remaining(), msg);
            error = "MySQL từ chối đăng nhập (" + std::to_string(code) + "): " + msg;
            return false;
        }

        if (tag == 0xfe) {
            // Đổi phương thức xác thực.
            Reader r(resp);
            uint8_t t = 0;
            r.u8(t);
            std::string newPlugin;
            if (!r.nulString(newPlugin)) {
                error = "Gói đổi phương thức xác thực không hợp lệ";
                return false;
            }
            Bytes newScramble;
            r.fixedBytes(r.remaining(), newScramble);
            while (!newScramble.empty() && newScramble.back() == 0) newScramble.pop_back();

            Bytes reply;
            if (newPlugin == "caching_sha2_password") {
                reply = cachingSha2Auth(params.password, newScramble);
            } else if (newPlugin == "mysql_native_password") {
                reply = nativePasswordAuth(params.password, newScramble);
            } else if (newPlugin == "mysql_clear_password") {
                error =
                    "Máy chủ yêu cầu gửi mật khẩu dạng rõ — hãy đổi phương thức xác thực của "
                    "tài khoản MySQL sang mysql_native_password hoặc caching_sha2_password.";
                return false;
            } else {
                error = "Phương thức xác thực MySQL chưa hỗ trợ: " + newPlugin;
                return false;
            }
            if (!writePacket(reply, static_cast<uint8_t>(rseq + 1), error)) return false;
            continue;
        }

        if (tag == 0x01 && resp.size() >= 2) {
            uint8_t sub = resp[1];
            if (sub == 0x03) continue;  // fast auth thành công, chờ gói OK
            if (sub == 0x04) {
                // Cần xác thực đầy đủ. Không dùng TLS nên phải mã hoá mật khẩu bằng
                // khoá công khai RSA của máy chủ.
                if (params.password.empty()) {
                    Bytes empty;
                    empty.push_back(0);
                    if (!writePacket(empty, static_cast<uint8_t>(rseq + 1), error)) return false;
                    continue;
                }
                Bytes askKey;
                askKey.push_back(0x02);
                if (!writePacket(askKey, static_cast<uint8_t>(rseq + 1), error)) return false;

                Bytes keyPacket;
                uint8_t kseq = 0;
                if (!readPacket(keyPacket, kseq, error)) return false;
                if (keyPacket.empty() || keyPacket[0] != 0x01) {
                    error = "Máy chủ không gửi khoá công khai RSA";
                    return false;
                }
                std::string pem(keyPacket.begin() + 1, keyPacket.end());
                crypto::RsaPublicKey key;
                if (!crypto::RsaPublicKey::fromPem(pem, key)) {
                    error = "Không đọc được khoá công khai RSA của MySQL";
                    return false;
                }
                // Mật khẩu (kèm NUL) XOR scramble rồi mã hoá RSA-OAEP.
                Bytes pwd(params.password.begin(), params.password.end());
                pwd.push_back(0);
                for (size_t i = 0; i < pwd.size(); ++i)
                    pwd[i] = static_cast<uint8_t>(pwd[i] ^ scramble[i % scramble.size()]);
                Bytes encrypted;
                if (!key.encryptOaepSha1(pwd, encrypted)) {
                    error = "Mã hoá mật khẩu MySQL bằng RSA thất bại";
                    return false;
                }
                if (!writePacket(encrypted, static_cast<uint8_t>(kseq + 1), error)) return false;
                continue;
            }
        }
        error = "Phản hồi xác thực MySQL không hiểu được (tag=" + std::to_string(tag) + ")";
        return false;
    }
    error = "Quá nhiều bước xác thực MySQL";
    return false;
}

MysqlResult MysqlConnection::readQueryResponse() {
    MysqlResult result;
    Bytes packet;
    uint8_t seq = 0;
    std::string error;
    if (!readPacket(packet, seq, error)) {
        result.error = error;
        return result;
    }
    if (packet.empty()) {
        result.error = "Phản hồi rỗng từ MySQL";
        return result;
    }

    uint8_t tag = packet[0];
    if (tag == 0xff) {
        Reader r(packet);
        uint8_t t = 0;
        uint16_t code = 0;
        r.u8(t);
        r.u16(code);
        std::string state;
        if (packet.size() > 3 && packet[3] == '#') {
            r.skip(1);
            r.fixedString(5, state);
        }
        std::string msg;
        r.fixedString(r.remaining(), msg);
        result.errorCode = code;
        result.sqlState = state;
        result.error = msg;
        return result;
    }
    if (tag == 0x00 || tag == 0xfe) {
        Reader r(packet);
        uint8_t t = 0;
        r.u8(t);
        bool isNull = false;
        r.lenEncInt(result.affectedRows, isNull);
        r.lenEncInt(result.lastInsertId, isNull);
        result.ok = true;
        return result;
    }
    if (tag == 0xfb) {
        result.error = "MySQL yêu cầu LOCAL INFILE — không hỗ trợ";
        return result;
    }

    // Bảng kết quả: số cột, mô tả cột, rồi các hàng.
    Reader r(packet);
    uint64_t columnCount = 0;
    bool isNull = false;
    if (!r.lenEncInt(columnCount, isNull) || columnCount == 0) {
        result.error = "Không đọc được số cột trong kết quả";
        return result;
    }

    for (uint64_t i = 0; i < columnCount; ++i) {
        if (!readPacket(packet, seq, error)) {
            result.error = error;
            return result;
        }
        Reader fr(packet);
        std::string catalog, schema, table, orgTable, name, orgName;
        bool n = false;
        fr.lenEncString(catalog, n);
        fr.lenEncString(schema, n);
        fr.lenEncString(table, n);
        fr.lenEncString(orgTable, n);
        fr.lenEncString(name, n);
        fr.lenEncString(orgName, n);
        uint64_t fixedLen = 0;
        fr.lenEncInt(fixedLen, n);
        uint16_t charsetId = 0;
        fr.u16(charsetId);
        uint32_t columnLength = 0;
        fr.u32(columnLength);
        uint8_t type = 0;
        fr.u8(type);
        MysqlField f;
        f.name = name;
        f.table = table;
        f.type = type;
        result.fields.push_back(std::move(f));
    }

    // Với CLIENT_DEPRECATE_EOF thì không có gói EOF sau phần mô tả cột.
    if (!(capabilities_ & CLIENT_DEPRECATE_EOF)) {
        if (!readPacket(packet, seq, error)) {
            result.error = error;
            return result;
        }
    }

    while (true) {
        if (!readPacket(packet, seq, error)) {
            result.error = error;
            return result;
        }
        if (packet.empty()) break;
        uint8_t rtag = packet[0];
        if (rtag == 0xfe && packet.size() < 9) break;  // EOF / OK kết thúc
        if (rtag == 0xff) {
            Reader er(packet);
            uint8_t t = 0;
            uint16_t code = 0;
            er.u8(t);
            er.u16(code);
            std::string msg;
            er.fixedString(er.remaining(), msg);
            result.errorCode = code;
            result.error = msg;
            return result;
        }
        Reader rr(packet);
        MysqlRow row;
        row.values.reserve(result.fields.size());
        row.isNull.reserve(result.fields.size());
        bool bad = false;
        for (size_t i = 0; i < result.fields.size(); ++i) {
            std::string v;
            bool n = false;
            if (!rr.lenEncString(v, n)) {
                bad = true;
                break;
            }
            row.values.push_back(std::move(v));
            row.isNull.push_back(n);
        }
        if (bad) break;
        result.rows.push_back(std::move(row));
    }

    result.ok = true;
    return result;
}

MysqlResult MysqlConnection::query(const std::string& sql) {
    MysqlResult result;
    if (!socket_.valid()) {
        result.error = "Chưa kết nối tới MySQL";
        return result;
    }
    Bytes packet;
    packet.reserve(sql.size() + 1);
    packet.push_back(0x03);  // COM_QUERY
    packet.insert(packet.end(), sql.begin(), sql.end());
    std::string error;
    if (!writePacket(packet, 0, error)) {
        result.error = error;
        return result;
    }
    return readQueryResponse();
}

bool MysqlConnection::ping(std::string& error) {
    if (!socket_.valid()) {
        error = "Chưa kết nối";
        return false;
    }
    Bytes packet;
    packet.push_back(0x0e);  // COM_PING
    if (!writePacket(packet, 0, error)) return false;
    Bytes resp;
    uint8_t seq = 0;
    if (!readPacket(resp, seq, error)) return false;
    if (resp.empty() || resp[0] == 0xff) {
        error = "MySQL không phản hồi ping";
        return false;
    }
    return true;
}

std::string MysqlConnection::quote(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char c : value) {
        switch (c) {
            case '\0': out += "\\0"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '"': out += "\\\""; break;
            case '\x1a': out += "\\Z"; break;
            default: out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string MysqlConnection::quoteBlob(const Bytes& value) {
    if (value.empty()) return "''";
    return "X'" + toHex(value) + "'";
}

}  // namespace db
}  // namespace ttd
