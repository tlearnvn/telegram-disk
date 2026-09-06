// Quản lý các phiên tải lên: nhận dữ liệu theo từng phần từ trình duyệt,
// cắt thành mảnh, đẩy lên nơi lưu, theo dõi tiến độ, xử lý huỷ và trùng lặp.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
    // >0 nghĩa là dòng vào đang bị chặn để đợi mảnh số này lên xong (vùng đệm
    // đã đầy). Không phải lỗi — nhưng là lý do con số nhận vào ngừng nhảy.
    int waitingChunk = 0;
};

class UploadManager;

// Một mảnh đã nằm trọn trong vùng đệm và đang được một luồng nền đẩy lên
// Telegram bằng tài khoản riêng của nó.
//
// Mảnh được GIAO ĐI theo thứ tự index, và cũng được THU VỀ theo đúng thứ tự đó.
// Ràng buộc "thu về đúng thứ tự" là thứ giữ cho mọi chuyện đơn giản: phần đã
// thu luôn là một tiền tố LIỀN MẠCH, nên mốc nối lại không bao giờ nhảy qua một
// mảnh chưa thật sự nằm trên Telegram. Thu về theo thứ tự KHÔNG làm mất tính
// song song — các mảnh vẫn bay cùng lúc, chỉ là ta đợi mảnh đầu hàng trước.
struct ManhBay {
    int index = 0;
    uint64_t offset = 0;          // vị trí byte đầu mảnh trong tệp
    uint64_t size = 0;
    std::string sha256;           // băm của riêng mảnh này
    crypto::Sha256 hasherSauManh; // ảnh chụp băm CẢ TỆP tính tới hết mảnh này
    std::unique_ptr<ChunkBuffer> buffer;
    std::thread luong;

    // Kết quả — chỉ được đọc sau khi join().
    bool ok = false;
    std::string loi;
    tg::ChunkLocation viTri;
    std::string nhanTaiKhoan;

    // Lưới an toàn: huỷ một std::thread còn join được là chương trình chết ngay
    // (std::terminate). Mọi đường thoát đều phải đi qua đây.
    ~ManhBay() {
        if (luong.joinable()) luong.join();
    }
};

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

    // RAM phiên này đang giữ cho vùng đệm — để ngân sách RAM là ngân sách CHUNG
    // của cả máy chủ chứ không phải mỗi phiên một suất.
    uint64_t ramDangGiu() const {
        if (cheDoDem_ != BufferMode::Memory) return 0;
        return static_cast<uint64_t>(soManhSongSong_) * chunkSize_;
    }

    // SHA-256 của đúng receivedBytes() byte đã nhận. Dùng để nối lại qua WebDAV:
    // máy khách gửi lại từ đầu, ta băm phần trùng rồi đối chiếu, khớp mới nối.
    Bytes digestSoFar() const;
    // Thông tin để nhận ra phiên bỏ dở của cùng một tệp.
    uint64_t totalSize() const;
    std::string targetKey() const;   // "<thư mục>/<tên>"

    // Giành quyền dùng phiên: chống hai lượt PUT cùng lúc giẫm lên nhau.
    bool claim() { bool cho = false; return busy_.compare_exchange_strong(cho, true); }
    void release() { busy_.store(false); }

    // Đợi mọi mảnh đang bay hạ cánh và lùi mốc nhận về tiền tố liền mạch nếu có
    // mảnh hỏng. Phải gọi TRƯỚC khi đọc receivedBytes()/digestSoFar() để nối
    // lại, không thì mốc nối tiếp trỏ vào chỗ dữ liệu chưa nằm trên Telegram.
    void chotSoTruocKhiNoi();

private:
    friend class UploadManager;

    // Lỗi tạm thời thì giữ phiên đang nhận; lỗi thật mới đánh dấu Failed.
    void ghiNhanLoi(const std::string& error);
    bool openChunk(std::string& error);
    bool closeChunk(std::string& error);
    void rollback();

    // --- Đường đẩy song song (chỉ dùng khi soManhSongSong_ > 1) ---------------
    // Giao mảnh đang đầy cho một luồng nền rồi mở mảnh kế tiếp ngay.
    bool giaoManhChoNen(std::string& error);
    // Thu mảnh ở đầu hàng: đợi luồng nền xong, ghi nhận kết quả, dịch mốc liền
    // mạch. Giả định caller giữ mu_.
    bool thuMotManh(std::string& error);
    // Thu hết mảnh đang bay. Dùng trước khi nối lại, khi hoàn tất, và khi huỷ.
    // Nếu có mảnh hỏng thì lùi mốc nhận về đúng tiền tố liền mạch đã đẩy xong.
    bool thuHetManh(std::string& error);

    // Chép trạng thái hiển thị sang ảnh chụp cho progress(). Giả định caller
    // giữ mu_.
    void capNhatTienDo();

    // Đặt sau mỗi `lock_guard(mu_)`: thoát hàm bằng đường nào thì ảnh chụp cũng
    // được cập nhật. receive() có gần chục đường thoát nên đây là cách duy nhất
    // không sót đường nào.
    struct DangTienDo {
        UploadSession* s;
        ~DangTienDo() { s->capNhatTienDo(); }
    };

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

    // Số mảnh được phép bay cùng lúc. 1 = đúng đường cũ, tuần tự, không sinh
    // luồng nào. Chỉ >1 khi vùng đệm giữ được trọn mảnh (memory hoặc disk).
    int soManhSongSong_ = 1;
    BufferMode cheDoDem_ = BufferMode::Stream;
    std::deque<std::unique_ptr<ManhBay>> dangBay_;

    // Mốc LIỀN MẠCH đã thật sự nằm trên Telegram — khác với receivedBytes_, thứ
    // chạy trước tới N mảnh. Khi có mảnh hỏng, receivedBytes_ và hasher_ được
    // lùi về đúng hai giá trị này.
    uint64_t committedBytes_ = 0;
    crypto::Sha256 hasherCommitted_;
    // Đã có mảnh hỏng trong lô đang bay. Từ lúc này mọi mảnh thu về sau đó đều
    // không còn liền mạch, dù bản thân chúng đẩy xong.
    bool manhHong_ = false;
    std::string loiManhDau_;

    crypto::Sha256 hasher_;
    crypto::Sha256 chunkHasher_;

    std::atomic<uint64_t> receivedBytes_{0};
    std::atomic<uint64_t> storedBytes_{0};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> busy_{false};
    std::atomic<int64_t> lastActivity_{0};
    // Số hiệu (đếm từ 1) của mảnh mà dòng vào đang đứng đợi vì vùng đệm đã đầy;
    // 0 là không đợi ai cả. Đợi ở đây là trạng thái BÌNH THƯỜNG của đẩy song
    // song — nó chỉ có nghĩa "đã đệm đủ số mảnh cho phép, giờ chạy đúng nhịp
    // Telegram" — nhưng phải nói ra, không thì người dùng thấy số ngừng nhảy và
    // tưởng máy treo.
    std::atomic<int> manhDangDoi_{0};
    UploadState state_ = UploadState::Preparing;
    std::string message_;
    std::string currentAccount_;
    int64_t startedAt_ = 0;
    int64_t startedMonotonic_ = 0;

    // --- Ảnh chụp để trả lời "tiến độ tới đâu rồi?" --------------------------
    //
    // Vì sao phải có bản sao riêng thay vì đọc thẳng các trường ở trên:
    // receive() giữ mu_ trong SUỐT lúc nó đứng đợi mảnh đầu hàng lên xong khi
    // vùng đệm đầy — mà đợi ở đây tính bằng phút, vì đó là thời gian thật để
    // 512 MB bò lên Telegram. Nếu progress() cũng phải giành mu_ thì suốt lúc
    // đó không ai hỏi được tiến độ: trang web đứng hình, và /api/uploads duyệt
    // qua từng phiên nên danh sách tải lên của MỌI phiên khác treo theo.
    //
    // tienDoMu_ chỉ được giữ đúng mấy dòng gán chuỗi, không bao giờ giữ trong
    // lúc đợi thứ gì. Thứ tự khoá là mu_ → tienDoMu_, không bao giờ ngược lại.
    struct AnhChupTienDo {
        std::string name;
        std::string targetFolder;
        std::string message;
        std::string currentAccount;
        uint64_t totalSize = 0;
        int chunkIndex = 0;
        int chunkTotal = 0;
        UploadState state = UploadState::Preparing;
    };
    mutable std::mutex tienDoMu_;
    AnhChupTienDo anhChup_;
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
    // Nối tiếp một phiên bỏ dở của đúng tệp này: máy khách phải bắt đầu gửi từ
    // byte thứ `resumeFrom` chứ không phải từ 0.
    bool resumed = false;
    uint64_t resumeFrom = 0;
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

    // Quyết định phiên này đẩy tuần tự hay song song, và đệm bằng gì.
    void chonCachDay(UploadSession& s);
    // Dọn tệp tạm sót lại từ lần chạy trước (máy chủ bị kill giữa chừng).
    void donTepTamBoLai();

    StorageEngine& engine_;
    db::Database& db_;
    const Config& config_;

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<UploadSession>> sessions_;
};

}  // namespace storage
}  // namespace ttd
