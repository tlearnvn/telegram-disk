// Quản lý các phiên tải lên: nhận dữ liệu theo từng phần từ trình duyệt,
// cắt thành mảnh, đẩy lên nơi lưu, theo dõi tiến độ, xử lý huỷ và trùng lặp.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "crypto/hash.h"
#include "db/database.h"
#include "storage/chunk_buffer.h"
#include "storage/storage_engine.h"

namespace ttd {
namespace storage {

// Cách xử lý khi phát hiện tệp trùng.
enum class ConflictPolicy {
    Ask,        // hỏi người dùng (mặc định)
    Skip,       // bỏ qua, không tải lên
    Replace,    // ghi đè tệp cũ
    KeepBoth,   // giữ cả hai, tự đổi tên
    LinkExisting  // dùng lại dữ liệu đã có, chỉ tạo thêm mục mới
};

ConflictPolicy parseConflictPolicy(const std::string& s);
const char* conflictPolicyName(ConflictPolicy p);

struct DuplicateInfo {
    int64_t fileId = 0;
    std::string name;
    std::string path;
    uint64_t size = 0;
    int64_t modifiedAt = 0;
    std::string reason;  // "cùng tên" | "cùng nội dung" | "giống hệt"
};

enum class UploadState {
    Preparing,
    Receiving,
    Flushing,
    Completed,
    Cancelled,
    Failed,
};

const char* uploadStateName(UploadState s);
const char* uploadStateNameVi(UploadState s);

// Lỗi này có phải loại "chờ chút rồi làm lại" không? Nếu đúng, `giayOut` nhận số
// giây Telegram yêu cầu chờ (0 nếu không đọc được).
//
// Phải nhận diện bằng chuỗi vì thông điệp lỗi đi từ tầng MTProto lên tới tầng
// HTTP dưới dạng văn bản. Bù lại, thứ đem so là **định danh lỗi của Telegram** —
// FLOOD_WAIT, FLOOD_PREMIUM_WAIT, SLOWMODE_WAIT — chứ không phải câu tiếng Việt
// do ứng dụng tự đặt, nên đổi cách diễn đạt thông báo không làm hỏng phép so.
bool laLoiTamThoi(const std::string& error, int& giayOut);

struct UploadProgress {
    std::string id;
    std::string name;
    std::string targetFolder;
    uint64_t totalSize = 0;
    uint64_t receivedBytes = 0;
    uint64_t storedBytes = 0;
    int chunkIndex = 0;
    int chunkTotal = 0;
    UploadState state = UploadState::Preparing;
    std::string message;
    std::string currentAccount;
    double speedBytesPerSecond = 0;
    int64_t startedAt = 0;
    int64_t updatedAt = 0;
    int64_t etaSeconds = 0;
    int ownerId = 0;
    int64_t fileId = 0;
};

class UploadManager;

// Một phiên tải lên.
class UploadSession {
public:
    UploadSession(UploadManager& manager, std::string id, int ownerId);
    ~UploadSession();

    const std::string& id() const { return id_; }
    UploadProgress progress() const;

    // Nhận dữ liệu theo thứ tự từ trình duyệt.
    bool receive(const uint8_t* data, size_t len, std::string& error);
    // Hoàn tất: đóng mảnh cuối, ghi siêu dữ liệu tệp.
    bool complete(db::FileEntry& out, std::string& error);
    // Huỷ: dừng ngay, xoá dữ liệu đã đẩy lên.
    void cancel(const std::string& reason);
    bool cancelled() const { return cancelled_.load(); }

    uint64_t receivedBytes() const { return receivedBytes_.load(); }
    int64_t lastActivity() const { return lastActivity_.load(); }

    // SHA-256 của đúng receivedBytes() byte đã nhận. Dùng để nối lại qua WebDAV:
    // máy khách gửi lại từ đầu, ta băm phần trùng rồi đối chiếu, khớp mới nối.
    Bytes digestSoFar() const;
    // Thông tin để nhận ra phiên bỏ dở của cùng một tệp.
    uint64_t totalSize() const;
    std::string targetKey() const;   // "<thư mục>/<tên>"

    // Giành quyền dùng phiên: chống hai lượt PUT cùng lúc giẫm lên nhau.
    bool claim() { bool cho = false; return busy_.compare_exchange_strong(cho, true); }
    void release() { busy_.store(false); }

private:
    friend class UploadManager;

    // Lỗi tạm thời thì giữ phiên đang nhận; lỗi thật mới đánh dấu Failed.
    void ghiNhanLoi(const std::string& error);
    bool openChunk(std::string& error);
    bool closeChunk(std::string& error);
    void rollback();

    UploadManager& manager_;
    std::string id_;
    int ownerId_ = 0;

    mutable std::mutex mu_;
    std::string name_;
    std::string targetFolderPath_;
    int64_t parentId_ = 0;
    uint64_t totalSize_ = 0;
    std::string mimeType_;
    std::string quickHash_;
    ConflictPolicy policy_ = ConflictPolicy::Ask;
    int64_t replaceFileId_ = 0;

    uint64_t chunkSize_ = 0;
    int chunkIndex_ = 0;
    int chunkTotal_ = 0;
    uint64_t chunkWritten_ = 0;
    uint64_t chunkOffset_ = 0;

    std::unique_ptr<tg::ChunkWriter> writer_;
    std::unique_ptr<ChunkBuffer> buffer_;
    std::vector<tg::ChunkLocation> uploaded_;
    std::vector<db::ChunkEntry> chunkRecords_;

    crypto::Sha256 hasher_;
    crypto::Sha256 chunkHasher_;

    std::atomic<uint64_t> receivedBytes_{0};
    std::atomic<uint64_t> storedBytes_{0};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> busy_{false};
    std::atomic<int64_t> lastActivity_{0};
    UploadState state_ = UploadState::Preparing;
    std::string message_;
    std::string currentAccount_;
    int64_t startedAt_ = 0;
    int64_t startedMonotonic_ = 0;
};

struct UploadInitRequest {
    std::string name;
    std::string targetFolderPath = "/";
    uint64_t totalSize = 0;
    std::string mimeType;
    std::string quickHash;
    std::string sha256;  // nếu trình duyệt đã tính sẵn
    ConflictPolicy policy = ConflictPolicy::Ask;
    int ownerId = 0;
};

struct UploadInitResult {
    bool ok = false;
    std::string uploadId;
    uint64_t chunkSize = 0;
    uint64_t browserChunkSize = 0;
    std::vector<DuplicateInfo> duplicates;
    bool needsDecision = false;
    bool skipped = false;       // đã bỏ qua theo chính sách Skip
    bool linked = false;        // đã liên kết tới dữ liệu có sẵn
    int64_t linkedFileId = 0;
    std::string message;
    std::string error;
};

class UploadManager {
public:
    UploadManager(StorageEngine& engine, db::Database& database, const Config& config);
    ~UploadManager();

    UploadInitResult begin(const UploadInitRequest& req);
    std::shared_ptr<UploadSession> find(const std::string& id);
    // Tìm phiên bỏ dở của đúng tệp này (cùng chủ, cùng đường dẫn, cùng kích
    // thước) để nối tiếp thay vì tải lại từ đầu. Trả về phiên đã được giành
    // quyền — người gọi phải release() khi xong.
    std::shared_ptr<UploadSession> claimResumable(int ownerId, const std::string& folder,
                                                  const std::string& name, uint64_t totalSize);
    bool complete(const std::string& id, db::FileEntry& out, std::string& error);
    bool cancel(const std::string& id, const std::string& reason);
    std::vector<UploadProgress> activeUploads(int ownerId) const;
    // Dọn các phiên bỏ dở quá lâu.
    void reapStale();

    StorageEngine& engine() { return engine_; }
    db::Database& db() { return db_; }
    const Config& config() const { return config_; }

    // Tìm các tệp có khả năng trùng.
    std::vector<DuplicateInfo> findDuplicates(const std::string& name, int64_t parentId,
                                              const std::string& quickHash,
                                              const std::string& sha256, uint64_t size);

private:
    friend class UploadSession;

    // Bảo đảm thư mục tồn tại, trả về id.
    bool ensureFolder(const std::string& path, int ownerId, int64_t& folderId,
                      std::string& error);

    StorageEngine& engine_;
    db::Database& db_;
    const Config& config_;

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<UploadSession>> sessions_;
};

}  // namespace storage
}  // namespace ttd
