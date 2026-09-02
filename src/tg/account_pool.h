// Quản lý nhiều tài khoản Telegram cùng lưu vào một siêu nhóm.
// Việc phân bổ mảnh dữ liệu theo kiểu ít việc nhất trước giúp tăng tốc tải lên
// và giảm nguy cơ bị Telegram giới hạn tần suất trên một tài khoản.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tg/storage_backend.h"
#include "tg/tg_account.h"

namespace ttd {
namespace tg {

struct AccountStatus {
    int id = 0;
    std::string label;
    std::string phone;
    std::string displayName;
    bool enabled = true;
    bool authorized = false;
    bool connected = false;
    int homeDc = 0;
    int activeUploads = 0;
    uint64_t bytesUploaded = 0;
    uint64_t bytesDownloaded = 0;
    int64_t floodWaitUntil = 0;
    std::string status;
    std::string lastError;
};

// Hàm lưu lại phiên đăng nhập vào cơ sở dữ liệu sau khi thay đổi.
using SessionPersistFn = std::function<void(int accountId, const std::map<int, AuthKey>&)>;

class AccountPool : public StorageBackend {
public:
    AccountPool(const TlSchema& schema, AppInfo appInfo);
    ~AccountPool() override;

    void setSupergroup(const SupergroupRef& group);
    SupergroupRef supergroup() const;

    // Đổi nóng thông tin ứng dụng cho pool và mọi tài khoản đang có.
    void updateAppInfo(const AppInfo& info);
    AppInfo appInfo() const;
    void setSessionPersist(SessionPersistFn fn) { persistFn_ = std::move(fn); }

    // Thêm tài khoản. Trả về con trỏ (thuộc sở hữu của pool).
    TgAccount* addAccount(const TgAccountConfig& config);
    TgAccount* findAccount(int id);
    void removeAccount(int id);
    void setAccountEnabled(int id, bool enabled);
    std::vector<AccountStatus> statuses() const;
    size_t accountCount() const;

    // Kết nối tất cả tài khoản đã bật (không chặn quá lâu; lỗi được ghi nhật ký).
    void connectAll();
    void disconnectAll();
    void persistAll();

    // --- StorageBackend ---
    std::string name() const override { return "Telegram"; }
    bool ready(std::string& why) const override;
    std::unique_ptr<ChunkWriter> beginChunk(uint64_t totalSize, const std::string& chunkName,
                                            std::string& error) override;
    bool readRange(ChunkLocation& loc, uint64_t offset, uint32_t limit, Bytes& out,
                   std::string& error) override;
    bool removeChunks(const std::vector<ChunkLocation>& locations, std::string& error) override;
    BackendStats stats() const override;

    // Chọn tài khoản rảnh nhất để giao việc. `boQua` liệt kê mã những tài khoản
    // vừa thử mà không dùng được, để vòng sau chọn tài khoản khác.
    TgAccount* pickAccount(std::string& error, const std::vector<int>& boQua = {});
    // Chọn tài khoản phù hợp để đọc dữ liệu (ưu tiên tài khoản đã tải lên mảnh đó).
    TgAccount* pickForRead(const ChunkLocation& loc, std::string& error);

    void noteUploadStarted(int accountId);
    void noteUploadFinished(int accountId);

private:
    const TlSchema& schema_;
    AppInfo appInfo_;

    mutable std::mutex mu_;
    std::vector<std::unique_ptr<TgAccount>> accounts_;
    std::map<int, bool> enabled_;
    std::map<int, int> activeUploads_;
    std::map<int, std::string> displayNames_;
    SupergroupRef group_;
    SessionPersistFn persistFn_;
    std::atomic<size_t> roundRobin_{0};
};

}  // namespace tg
}  // namespace ttd
