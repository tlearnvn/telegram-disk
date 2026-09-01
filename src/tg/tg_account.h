// Một tài khoản Telegram: quản lý phiên trên nhiều trung tâm dữ liệu, tải lên,
// tải xuống và xoá tệp trong siêu nhóm.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tg/mtproto_session.h"
#include "tg/tl_schema.h"

namespace ttd {
namespace tg {

// Vị trí một mảnh dữ liệu đã lưu trên Telegram.
struct ChunkLocation {
    int64_t messageId = 0;
    int64_t documentId = 0;
    int64_t accessHash = 0;
    Bytes fileReference;
    int dcId = 0;
    uint64_t size = 0;
    int accountId = 0;   // tài khoản đã tải lên
    std::string fileName;

    bool valid() const { return documentId != 0 && messageId != 0; }
};

// Định danh siêu nhóm dùng làm nơi lưu trữ.
struct SupergroupRef {
    int64_t channelId = 0;
    int64_t accessHash = 0;
    std::string title;
    bool valid() const { return channelId != 0; }
};

struct TgAccountConfig {
    int id = 0;
    std::string label;
    std::string phone;
    int homeDc = 2;
    bool testMode = false;
    bool obfuscated = false;
    int connectionsPerDc = 1;
    int requestTimeoutMs = 90000;
};

using ProgressCallback = std::function<void(uint64_t uploadedBytes)>;
using CancelCheck = std::function<bool()>;

class TgAccount {
public:
    TgAccount(const TlSchema& schema, AppInfo appInfo, TgAccountConfig config);
    ~TgAccount();

    int id() const { return config_.id; }
    const std::string& label() const { return config_.label; }
    const TgAccountConfig& config() const { return config_; }
    int32_t appApiId() const { return appInfo_.apiId; }
    const std::string& appApiHash() const { return appInfo_.apiHash; }

    // Đổi nóng thông tin ứng dụng (api_id, api_hash, tên thiết bị…) khi quản
    // trị viên lưu lại Cài đặt. Khoá xác thực đang có được giữ lại, còn các
    // phiên bị đóng để lần gọi sau dựng lại và gửi initConnection mới.
    void updateAppInfo(const AppInfo& info);

    // Nạp khoá xác thực đã lưu (chuỗi hex 256 byte + salt + dc).
    void loadSession(int dcId, const Bytes& authKey, int64_t serverSalt);
    // Lấy toàn bộ khoá đang giữ để lưu vào cơ sở dữ liệu.
    std::map<int, AuthKey> exportSessions() const;

    bool connect(std::string& error);
    void disconnect();
    bool connected() const;
    bool authorized() const { return authorized_.load(); }
    void setAuthorized(bool v) { authorized_.store(v); }

    // Trạng thái hiển thị trên giao diện.
    std::string statusText() const;
    void setLastError(const std::string& e);
    std::string lastError() const;

    // Gọi một hàm API trên DC chỉ định (mặc định là DC nhà).
    InvokeResult invoke(const TlValue& request, int dcId = 0, int timeoutMs = 0);

    // Lấy phiên (tạo và uỷ quyền nếu cần).
    MtprotoSession* session(int dcId, std::string& error);

    // Cập nhật danh sách DC từ máy chủ.
    bool refreshDcConfig(std::string& error);
    // Lấy thông tin người dùng hiện tại (để kiểm tra đăng nhập).
    bool fetchSelf(std::string& error, std::string& displayName, int64_t& userId);

    // Tìm siêu nhóm theo tên người dùng công khai (ví dụ "kho_luu_tru").
    bool resolveSupergroupByUsername(const std::string& username, SupergroupRef& out,
                                     std::string& error);
    // Lấy access_hash cho một siêu nhóm đã biết channel_id.
    bool resolveSupergroupById(int64_t channelId, int64_t accessHashHint, SupergroupRef& out,
                               std::string& error);
    // Liệt kê các cuộc trò chuyện dạng siêu nhóm mà tài khoản đang tham gia.
    bool listSupergroups(std::vector<SupergroupRef>& out, std::string& error);

    // Tải một mảnh dữ liệu đã nằm sẵn trong bộ nhớ lên Telegram.
    bool uploadChunk(const SupergroupRef& group, const Bytes& data, const std::string& fileName,
                     ChunkLocation& out, const ProgressCallback& progress,
                     const CancelCheck& cancelled, std::string& error);

    // Tải lên theo luồng: gọi begin -> feed nhiều lần -> finish.
    class StreamUpload {
    public:
        StreamUpload(TgAccount& account, SupergroupRef group, uint64_t totalSize,
                     std::string fileName);
        bool feed(const uint8_t* data, size_t len, std::string& error);
        bool finish(ChunkLocation& out, std::string& error);
        void abort();
        uint64_t bytesSent() const { return sent_; }

    private:
        bool flushPart(std::string& error, bool last);

        TgAccount& account_;
        SupergroupRef group_;
        uint64_t totalSize_;
        std::string fileName_;
        int64_t fileId_ = 0;
        int partSize_ = 512 * 1024;
        int totalParts_ = 0;
        int nextPart_ = 0;
        Bytes buffer_;
        uint64_t sent_ = 0;
        bool aborted_ = false;
        bool finished_ = false;
    };

    std::unique_ptr<StreamUpload> beginStreamUpload(const SupergroupRef& group,
                                                    uint64_t totalSize,
                                                    const std::string& fileName);

    // Tải một đoạn byte của mảnh dữ liệu. offset phải chia hết cho 4096.
    bool downloadRange(const ChunkLocation& loc, uint64_t offset, uint32_t limit, Bytes& out,
                       std::string& error);

    // Xoá các thông điệp chứa mảnh dữ liệu.
    bool deleteMessages(const SupergroupRef& group, const std::vector<int64_t>& messageIds,
                        std::string& error);

    // Lấy lại file_reference mới cho một mảnh (tham chiếu cũ hết hạn sau vài giờ).
    bool refreshFileReference(const SupergroupRef& group, ChunkLocation& loc,
                              std::string& error);

    uint64_t bytesUploaded() const { return bytesUploaded_.load(); }
    uint64_t bytesDownloaded() const { return bytesDownloaded_.load(); }

private:
    MtprotoSession* createSession(int dcId, std::string& error);
    bool authorizeSession(int dcId, MtprotoSession* target, std::string& error);
    // Xử lý các lỗi cần chuyển DC hoặc chờ (FLOOD_WAIT, FILE_MIGRATE…).
    bool handleMigration(const RpcError& err, int& dcIdInOut);

    const TlSchema& schema_;
    AppInfo appInfo_;
    TgAccountConfig config_;

    mutable std::mutex mu_;
    std::map<int, std::unique_ptr<MtprotoSession>> sessions_;
    std::map<int, AuthKey> loadedKeys_;
    std::atomic<bool> authorized_{false};
    std::atomic<uint64_t> bytesUploaded_{0};
    std::atomic<uint64_t> bytesDownloaded_{0};
    std::string lastError_;
    std::atomic<int64_t> floodWaitUntil_{0};
};

}  // namespace tg
}  // namespace ttd
