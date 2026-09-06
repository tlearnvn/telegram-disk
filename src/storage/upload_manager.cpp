#include "storage/upload_manager.h"

#include <algorithm>
#include <cstdio>

#include "common/fsutil.h"
#include "common/logging.h"
#include "common/strutil.h"
#include "common/timeutil.h"
#include "crypto/random.h"
#include "http/mime.h"

namespace ttd {
namespace storage {

namespace {
constexpr const char* kTag = "upload";
}

ConflictPolicy parseConflictPolicy(const std::string& s) {
    std::string t = toLower(trim(s));
    if (t == "skip" || t == "bo-qua") return ConflictPolicy::Skip;
    if (t == "replace" || t == "ghi-de") return ConflictPolicy::Replace;
    if (t == "keep_both" || t == "keep-both" || t == "giu-ca-hai") return ConflictPolicy::KeepBoth;
    if (t == "link" || t == "lien-ket") return ConflictPolicy::LinkExisting;
    return ConflictPolicy::Ask;
}

const char* conflictPolicyName(ConflictPolicy p) {
    switch (p) {
        case ConflictPolicy::Skip: return "skip";
        case ConflictPolicy::Replace: return "replace";
        case ConflictPolicy::KeepBoth: return "keep_both";
        case ConflictPolicy::LinkExisting: return "link";
        default: return "ask";
    }
}

const char* uploadStateName(UploadState s) {
    switch (s) {
        case UploadState::Preparing: return "preparing";
        case UploadState::Receiving: return "receiving";
        case UploadState::Flushing: return "flushing";
        case UploadState::Completed: return "completed";
        case UploadState::Cancelled: return "cancelled";
        default: return "failed";
    }
}

const char* uploadStateNameVi(UploadState s) {
    switch (s) {
        case UploadState::Preparing: return "Đang chuẩn bị";
        case UploadState::Receiving: return "Đang tải lên";
        case UploadState::Flushing: return "Đang hoàn tất";
        case UploadState::Completed: return "Hoàn tất";
        case UploadState::Cancelled: return "Đã huỷ";
        default: return "Lỗi";
    }
}

bool laLoiTamThoi(const std::string& error, int& giayOut) {
    giayOut = 0;
    // Lỗi nội bộ của Telegram (mã 500): máy chủ tự hỏng, gửi lại là xong. Không
    // kèm số giây nên để mặc cho tầng trên chọn quãng nghỉ.
    static const char* kMayChuHong[] = {"RPC_CALL_FAIL", "RPC_MCGET_FAIL",
                                        "INTERNAL_SERVER_ERROR", "WORKER_BUSY_TOO_LONG_RETRY",
                                        "MSG_WAIT_FAILED", "MSGID_DECREASE_RETRY",
                                        "Telegram lỗi nội bộ"};
    for (const char* d : kMayChuHong) {
        if (error.find(d) != std::string::npos) return true;
    }
    static const char* kDinhDanh[] = {"FLOOD_PREMIUM_WAIT_", "FLOOD_WAIT_", "SLOWMODE_WAIT_",
                                      "TAKEOUT_INIT_DELAY_", "FLOOD_PREMIUM_WAIT",
                                      "FLOOD_WAIT"};
    for (const char* d : kDinhDanh) {
        size_t p = error.find(d);
        if (p == std::string::npos) continue;
        // Đọc số ngay sau định danh, nếu có.
        size_t i = p + std::strlen(d);
        while (i < error.size() && (error[i] == '_' || error[i] == ' ')) ++i;
        int n = 0;
        bool coSo = false;
        while (i < error.size() && error[i] >= '0' && error[i] <= '9') {
            n = n * 10 + (error[i] - '0');
            ++i;
            coSo = true;
            if (n > 86400) break;   // vô lý thì thôi
        }
        if (coSo) giayOut = n;
        return true;
    }
    return false;
}

// Đẩy song song bắt buộc phải ĐỆM trọn mảnh: luồng nền chỉ nhận được một mảnh
// khi mảnh đó đã nằm đủ ở đâu đó. Chế độ stream đẩy thẳng từng phần 512 KB ra
// mạng ngay lúc nhận nên chẳng có gì để giao — nó luôn tuần tự.
//
// Đây cũng là chỗ memoryBudget có nghĩa: đệm bằng RAM thì số mảnh bay cùng lúc
// bị chặn bởi ngân sách RAM chia cho cỡ mảnh.
void UploadManager::chonCachDay(UploadSession& s) {
    int muon = config_.storage.parallelChunks;
    if (muon < 1) muon = 1;
    BufferMode mode = parseBufferMode(config_.storage.bufferMode);

    if (muon == 1) {
        s.soManhSongSong_ = 1;
        s.cheDoDem_ = mode;
        return;
    }

    if (mode == BufferMode::Stream) {
        // Người dùng muốn song song mà lại để stream. Đệm ra ĐĨA là lựa chọn
        // đúng tinh thần "ít RAM nhất" của họ, chỉ đổi chỗ chứa tạm — nhưng
        // phải xem đĩa có chỗ đã, kẻo hết đĩa giữa chừng làm hỏng cả lượt tải.
        std::string thuMuc = config_.resolvePath(config_.storage.spoolDirectory);
        ensureDirectoryExists(thuMuc);
        uint64_t can = static_cast<uint64_t>(muon) * s.chunkSize_;
        uint64_t trong = freeDiskSpace(thuMuc);
        if (trong > 0 && trong < can + can / 4) {   // đòi dư thêm 25% cho an toàn
            LOG_WARN(kTag,
                     "[%s] Đệm đĩa cần %s mà %s chỉ còn %s — quay về đẩy TUẦN TỰ"
                     " (giảm cỡ mảnh hoặc số mảnh song song để bật lại)",
                     s.id_.c_str(), formatBytes(can).c_str(), thuMuc.c_str(),
                     formatBytes(trong).c_str());
            s.cheDoDem_ = BufferMode::Stream;
            s.soManhSongSong_ = 1;
            return;
        }
        mode = BufferMode::Disk;
        LOG_INFO(kTag,
                 "[%s] Đẩy %d mảnh song song nên cần đệm — dùng đệm ĐĨA tại %s (cần %s)",
                 s.id_.c_str(), muon, thuMuc.c_str(), formatBytes(can).c_str());
    } else if (mode == BufferMode::Memory) {
        // memoryBudget là ngân sách CHUNG cho cả máy chủ, không phải cho từng
        // phiên: bốn lượt tải cùng lúc mà mỗi lượt tự lấy trọn ngân sách thì
        // dùng gấp bốn mức người ta đặt, và máy hết RAM.
        uint64_t dangDung = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& kv : sessions_) {
                if (kv.second && kv.second.get() != &s)
                    dangDung += kv.second->ramDangGiu();
            }
        }
        uint64_t nganSach = config_.storage.memoryBudget;
        uint64_t conLai = nganSach > dangDung ? nganSach - dangDung : 0;
        uint64_t vuaDuoc = s.chunkSize_ > 0 ? conLai / s.chunkSize_ : 0;
        if (vuaDuoc < 1) vuaDuoc = 1;
        if (static_cast<uint64_t>(muon) > vuaDuoc) {
            LOG_WARN(kTag,
                     "[%s] Đệm RAM còn %s (ngân sách %s, phiên khác đang giữ %s) — chỉ chứa"
                     " nổi %llu mảnh cỡ %s, hạ từ %d xuống %llu mảnh song song",
                     s.id_.c_str(), formatBytes(conLai).c_str(), formatBytes(nganSach).c_str(),
                     formatBytes(dangDung).c_str(), static_cast<unsigned long long>(vuaDuoc),
                     formatBytes(s.chunkSize_).c_str(), muon,
                     static_cast<unsigned long long>(vuaDuoc));
            muon = static_cast<int>(vuaDuoc);
        }
    }

    s.cheDoDem_ = mode;
    s.soManhSongSong_ = muon;
    if (muon > 1) {
        LOG_INFO(kTag, "[%s] Đẩy tối đa %d mảnh song song, mỗi mảnh một tài khoản, đệm %s",
                 s.id_.c_str(), muon, bufferModeName(mode));
    }
}

// ---------------------------------------------------------------------------
//  UploadSession
// ---------------------------------------------------------------------------
UploadSession::UploadSession(UploadManager& manager, std::string id, int ownerId)
    : manager_(manager), id_(std::move(id)), ownerId_(ownerId) {
    startedAt_ = nowUnix();
    startedMonotonic_ = monotonicMillis();
    lastActivity_.store(startedAt_);
}

UploadSession::~UploadSession() {
    // Thu hết luồng nền TRƯỚC khi chạm vào bất cứ thứ gì khác. Luồng nền còn
    // đọc cancelled_/storedBytes_ của phiên, mà thành viên được huỷ theo thứ tự
    // NGƯỢC với thứ tự khai báo — dangBay_ khai trước hai cái đó nên nó bị huỷ
    // SAU: tới lúc ManhBay::~ManhBay() join thì luồng đang đọc biến đã chết.
    // Hiện chưa với tới được vì mọi đường đều thu sạch trước, nhưng chỉ cần một
    // lần sửa đổi bất cẩn là thành dùng-sau-khi-giải-phóng.
    cancelled_.store(true);
    while (!dangBay_.empty()) {
        std::unique_ptr<ManhBay> manh = std::move(dangBay_.front());
        dangBay_.pop_front();
        if (manh->luong.joinable()) manh->luong.join();
        if (manh->ok) uploaded_.push_back(manh->viTri);
    }
    if (state_ != UploadState::Completed && !uploaded_.empty()) rollback();
}

Bytes UploadSession::digestSoFar() const {
    std::lock_guard<std::mutex> lk(mu_);
    // Sha256 chỉ chứa số và mảng cố định nên sao chép được: chốt bản sao để lấy
    // tổng kiểm của phần đã nhận mà không phá trạng thái băm đang chạy.
    crypto::Sha256 banSao = hasher_;
    uint8_t out[32];
    banSao.finish(out);
    return Bytes(out, out + 32);
}

uint64_t UploadSession::totalSize() const {
    std::lock_guard<std::mutex> lk(mu_);
    return totalSize_;
}

std::string UploadSession::targetKey() const {
    std::lock_guard<std::mutex> lk(mu_);
    return normalizeVirtualPath(targetFolderPath_ + "/" + name_);
}

UploadProgress UploadSession::progress() const {
    std::lock_guard<std::mutex> lk(mu_);
    UploadProgress p;
    p.id = id_;
    p.name = name_;
    p.targetFolder = targetFolderPath_;
    p.totalSize = totalSize_;
    p.receivedBytes = receivedBytes_.load();
    p.storedBytes = storedBytes_.load();
    p.chunkIndex = chunkIndex_;
    p.chunkTotal = chunkTotal_;
    p.state = state_;
    p.message = message_;
    p.currentAccount = currentAccount_;
    p.startedAt = startedAt_;
    p.updatedAt = lastActivity_.load();
    p.ownerId = ownerId_;

    int64_t elapsed = monotonicMillis() - startedMonotonic_;
    if (elapsed > 0) p.speedBytesPerSecond = static_cast<double>(p.receivedBytes) * 1000.0 /
                                             static_cast<double>(elapsed);
    if (p.speedBytesPerSecond > 1 && p.totalSize > p.receivedBytes) {
        p.etaSeconds = static_cast<int64_t>(
            static_cast<double>(p.totalSize - p.receivedBytes) / p.speedBytesPerSecond);
    }
    return p;
}

bool UploadSession::openChunk(std::string& error) {
    // Giả định caller giữ mu_.
    uint64_t remaining = totalSize_ > chunkOffset_ ? totalSize_ - chunkOffset_ : 0;
    uint64_t thisChunkSize = std::min(chunkSize_, remaining);
    if (thisChunkSize == 0) thisChunkSize = chunkSize_;

    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s.p%04d", id_.c_str(), chunkIndex_);
    std::string chunkName = nameBuf;

    std::string spool = manager_.config().resolvePath(
        joinPath(manager_.config().storage.spoolDirectory, chunkName + ".tmp"));

    if (soManhSongSong_ > 1) {
        // Đường song song: chỉ mở VÙNG ĐỆM, chưa đụng gì tới Telegram. Tài khoản
        // được chọn muộn — lúc luồng nền thật sự đẩy — nên nó chọn được tài
        // khoản rảnh nhất tại đúng thời điểm đó chứ không phải lúc mở mảnh.
        buffer_.reset(new ChunkBuffer(cheDoDem_, thisChunkSize, spool));
        writer_.reset();
        currentAccount_ = "đang đợi tài khoản";
        chunkWritten_ = 0;
        chunkHasher_.reset();
        LOG_DEBUG(kTag, "[%s] Mở mảnh %d/%d (%s) vào vùng đệm %s", id_.c_str(), chunkIndex_ + 1,
                  chunkTotal_, formatBytes(thisChunkSize).c_str(), bufferModeName(cheDoDem_));
        return true;
    }

    writer_ = manager_.engine().backend().beginChunk(thisChunkSize, chunkName, error);
    if (!writer_) return false;
    currentAccount_ = writer_->sourceLabel();

    tg::ChunkWriter* w = writer_.get();
    UploadSession* self = this;
    buffer_.reset(new ChunkBuffer(
        cheDoDem_, thisChunkSize, spool,
        [w, self](const uint8_t* data, size_t len, std::string& err) -> bool {
            if (self->cancelled_.load()) {
                err = "Phiên tải lên đã bị huỷ";
                return false;
            }
            if (!w->write(data, len, err)) return false;
            self->storedBytes_.fetch_add(len);
            return true;
        }));

    chunkWritten_ = 0;
    chunkHasher_.reset();
    LOG_DEBUG(kTag, "[%s] Mở mảnh %d/%d (%s) qua %s", id_.c_str(), chunkIndex_ + 1, chunkTotal_,
              formatBytes(thisChunkSize).c_str(), currentAccount_.c_str());
    return true;
}

// Giao mảnh vừa đầy cho một luồng nền, rồi mở mảnh kế tiếp ngay lập tức.
// Giả định caller giữ mu_.
bool UploadSession::giaoManhChoNen(std::string& error) {
    if (!buffer_) return true;

    auto manh = std::unique_ptr<ManhBay>(new ManhBay());
    manh->index = chunkIndex_;
    manh->offset = chunkOffset_;
    manh->size = chunkWritten_;
    manh->buffer = std::move(buffer_);

    uint8_t digest[32];
    chunkHasher_.finish(digest);
    manh->sha256 = toHex(digest, 32);
    // Ảnh chụp băm CẢ TỆP tại đúng ranh giới cuối mảnh này. Sha256 là kiểu dữ
    // liệu thuần (mảng số, không con trỏ) nên sao chép được nguyên trạng — đây
    // chính là thứ cho phép lùi mốc nối lại về đúng ranh giới mảnh khi có mảnh
    // hỏng, mà không phải băm lại từ đầu tệp.
    manh->hasherSauManh = hasher_;

    char nameBuf[64];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s.p%04d", id_.c_str(), manh->index);
    std::string chunkName = nameBuf;

    ManhBay* raw = manh.get();
    UploadSession* self = this;
    uint64_t coManh = manh->size;
    raw->luong = std::thread([self, raw, chunkName, coManh]() {
        if (self->cancelled_.load()) {
            raw->loi = "Phiên tải lên đã bị huỷ";
            return;
        }
        std::string err;
        std::unique_ptr<tg::ChunkWriter> w =
            self->manager_.engine().backend().beginChunk(coManh, chunkName, err);
        if (!w) {
            raw->loi = err.empty() ? "Không mở được mảnh trên Telegram" : err;
            return;
        }
        raw->nhanTaiKhoan = w->sourceLabel();
        bool oke = raw->buffer->flush(
            [self, &w](const uint8_t* data, size_t len, std::string& e) -> bool {
                if (self->cancelled_.load()) {
                    e = "Phiên tải lên đã bị huỷ";
                    return false;
                }
                if (!w->write(data, len, e)) return false;
                self->storedBytes_.fetch_add(len);
                return true;
            },
            err);
        raw->buffer->discard();
        if (!oke) {
            w->abort();
            raw->loi = err;
            return;
        }
        if (!w->finish(raw->viTri, err)) {
            raw->loi = err;
            return;
        }
        raw->ok = true;
    });

    dangBay_.push_back(std::move(manh));

    chunkOffset_ += chunkWritten_;
    chunkWritten_ = 0;
    ++chunkIndex_;
    (void)error;
    return true;
}

// Thu mảnh ở ĐẦU hàng — đợi nó xong, ghi nhận, rồi dịch mốc liền mạch.
// Giả định caller giữ mu_.
bool UploadSession::thuMotManh(std::string& error) {
    if (dangBay_.empty()) return true;
    std::unique_ptr<ManhBay> manh = std::move(dangBay_.front());
    dangBay_.pop_front();
    if (manh->luong.joinable()) manh->luong.join();

    // Một khi đã có mảnh hỏng thì MỌI mảnh sau nó đều mất tính liền mạch — kể
    // cả những mảnh tự nó đẩy xong ngon lành, vì giữa chúng và phần đã chốt có
    // một lỗ hổng. Giữ lại vị trí để bộ dọn thu hồi, nhưng tuyệt đối không ghi
    // vào danh sách mảnh và không dịch mốc liền mạch.
    if (manhHong_) {
        if (manh->ok) uploaded_.push_back(manh->viTri);
        error = loiManhDau_.empty() ? "Có mảnh đẩy lên thất bại" : loiManhDau_;
        return false;
    }

    if (!manh->ok) {
        manhHong_ = true;
        loiManhDau_ = manh->loi.empty() ? "Đẩy mảnh lên Telegram thất bại" : manh->loi;
        error = loiManhDau_;
        return false;
    }

    db::ChunkEntry rec;
    rec.index = manh->index;
    rec.offset = manh->offset;
    rec.size = manh->size;
    rec.sha256 = manh->sha256;
    StorageEngine::fromLocation(manh->viTri, rec);
    rec.size = manh->size;
    chunkRecords_.push_back(rec);
    uploaded_.push_back(manh->viTri);

    // Thu theo đúng thứ tự nên tới đây, mọi mảnh trước nó đều đã xong: mốc này
    // là một tiền tố LIỀN MẠCH thật sự nằm trên Telegram.
    committedBytes_ = manh->offset + manh->size;
    hasherCommitted_ = manh->hasherSauManh;

    LOG_INFO(kTag, "[%s] Đã lưu mảnh %d/%d — %s qua %s", id_.c_str(), manh->index + 1, chunkTotal_,
             formatBytes(manh->size).c_str(), manh->nhanTaiKhoan.c_str());
    currentAccount_ = manh->nhanTaiKhoan;
    return true;
}

// Thu HẾT mảnh đang bay. Nếu có mảnh hỏng thì lùi mốc nhận về đúng tiền tố liền
// mạch — nếu không, lượt gửi sau sẽ nối tiếp từ một chỗ mà dữ liệu chưa hề nằm
// trên Telegram, và tệp hỏng âm thầm.
bool UploadSession::thuHetManh(std::string& error) {
    // Thu cho bằng hết — luồng nền nào cũng phải được join, không thì std::thread
    // bị huỷ trong lúc còn chạy và chương trình chết ngay (std::terminate).
    // thuMotManh() tự nhớ mảnh hỏng đầu tiên nên vòng lặp này cứ chạy tới cạn.
    while (!dangBay_.empty()) {
        std::string bo;
        thuMotManh(bo);
    }
    if (!manhHong_) return true;

    // Lùi cả số byte đã nhận lẫn băm về đúng ranh giới mảnh cuối đã đẩy xong.
    // Không lùi thì lượt gửi sau nối tiếp từ chỗ chưa có dữ liệu trên Telegram.
    receivedBytes_.store(committedBytes_);
    hasher_ = hasherCommitted_;
    chunkOffset_ = committedBytes_;
    chunkIndex_ = chunkRecords_.empty() ? 0 : chunkRecords_.back().index + 1;
    chunkWritten_ = 0;
    chunkHasher_.reset();
    if (buffer_) {
        buffer_->discard();
        buffer_.reset();
    }
    error = loiManhDau_;
    // Xoá cờ để lượt gửi sau bắt đầu lại sạch sẽ từ mốc vừa lùi về.
    manhHong_ = false;
    loiManhDau_.clear();
    return false;
}

bool UploadSession::closeChunk(std::string& error) {
    // Giả định caller giữ mu_.
    if (!writer_) return true;
    if (!buffer_->flush(error)) {
        writer_->abort();
        writer_.reset();
        buffer_.reset();
        return false;
    }
    tg::ChunkLocation loc;
    if (!writer_->finish(loc, error)) {
        writer_.reset();
        buffer_.reset();
        return false;
    }
    writer_.reset();
    buffer_.reset();

    uint8_t digest[32];
    chunkHasher_.finish(digest);

    db::ChunkEntry rec;
    rec.index = chunkIndex_;
    rec.offset = chunkOffset_;
    rec.size = chunkWritten_;
    rec.sha256 = toHex(digest, 32);
    StorageEngine::fromLocation(loc, rec);
    rec.size = chunkWritten_;  // giữ kích thước thực tế đã ghi
    chunkRecords_.push_back(rec);
    uploaded_.push_back(loc);

    LOG_INFO(kTag, "[%s] Đã lưu mảnh %d/%d — %s", id_.c_str(), chunkIndex_ + 1, chunkTotal_,
             formatBytes(chunkWritten_).c_str());

    chunkOffset_ += chunkWritten_;
    chunkWritten_ = 0;
    ++chunkIndex_;
    // Đường tuần tự: mảnh vừa đóng là đã nằm trên Telegram, nên mốc liền mạch
    // tiến ngay tới đây. Giữ hai mốc này đúng ở CẢ HAI đường để phần lùi mốc khi
    // hỏng không phải phân biệt mình đang chạy kiểu nào.
    committedBytes_ = chunkOffset_;
    hasherCommitted_ = hasher_;
    return true;
}

// Ghi nhận lỗi trong lúc nhận dữ liệu. Lỗi TẠM THỜI (Telegram bảo chờ) thì giữ
// phiên ở trạng thái đang nhận để lượt gửi sau nối tiếp được; chỉ lỗi thật mới
// đánh dấu Failed, vì phiên Failed không cho nối lại nữa.
void UploadSession::ghiNhanLoi(const std::string& error) {
    message_ = error;
    int giay = 0;
    if (laLoiTamThoi(error, giay)) {
        state_ = UploadState::Receiving;
        lastActivity_.store(nowUnix());
        return;
    }
    state_ = UploadState::Failed;
}

bool UploadSession::receive(const uint8_t* data, size_t len, std::string& error) {
    if (cancelled_.load()) {
        error = "Phiên tải lên đã bị huỷ";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (state_ == UploadState::Failed || state_ == UploadState::Cancelled) {
        error = message_.empty() ? "Phiên tải lên không còn hoạt động" : message_;
        return false;
    }
    state_ = UploadState::Receiving;
    lastActivity_.store(nowUnix());

    // Đóng mảnh: tuần tự thì đẩy tại chỗ, song song thì giao cho luồng nền.
    auto dongManh = [this, &error]() -> bool {
        if (soManhSongSong_ <= 1) return closeChunk(error);
        if (!giaoManhChoNen(error)) return false;
        // Chặn dòng vào khi đã đủ số mảnh bay cùng lúc: thu bớt mảnh đầu hàng
        // rồi mới nhận tiếp. Không có bước này thì cả tệp 58 GB sẽ chui hết vào
        // RAM hoặc ổ đĩa.
        while (static_cast<int>(dangBay_.size()) >= soManhSongSong_) {
            if (!thuMotManh(error)) return false;
        }
        return true;
    };
    // Hỏng giữa chừng thì phải thu hết mảnh đang bay TRƯỚC khi báo lỗi ra ngoài:
    // luồng nền phải được join, và mốc nhận phải lùi về tiền tố liền mạch để
    // lượt gửi sau nối đúng chỗ.
    auto bao = [this, &error](const std::string& e) {
        error = e;
        if (soManhSongSong_ > 1 && !dangBay_.empty()) {
            std::string bo;
            thuHetManh(bo);
        }
        ghiNhanLoi(error);
    };

    size_t offset = 0;
    while (offset < len) {
        if (cancelled_.load()) {
            error = "Phiên tải lên đã bị huỷ";
            return false;
        }
        // Ở đường song song writer_ luôn rỗng (tài khoản chọn muộn, trong luồng
        // nền), nên mốc "đã mở mảnh chưa" phải là VÙNG ĐỆM chứ không phải writer.
        if (!buffer_) {
            if (!openChunk(error)) {
                bao(error);
                return false;
            }
        }

        uint64_t remainingInChunk = chunkSize_ - chunkWritten_;
        size_t take = static_cast<size_t>(
            std::min<uint64_t>(remainingInChunk, static_cast<uint64_t>(len - offset)));
        if (take == 0) {
            if (!dongManh()) {
                bao(error);
                return false;
            }
            continue;
        }
        if (!buffer_->append(data + offset, take, error)) {
            bao(error);
            return false;
        }
        hasher_.update(data + offset, take);
        chunkHasher_.update(data + offset, take);
        chunkWritten_ += take;
        offset += take;
        receivedBytes_.fetch_add(take);

        if (chunkWritten_ >= chunkSize_) {
            if (!dongManh()) {
                bao(error);
                return false;
            }
        }
    }
    lastActivity_.store(nowUnix());
    return true;
}

bool UploadSession::complete(db::FileEntry& out, std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (cancelled_.load()) {
        error = "Phiên tải lên đã bị huỷ";
        return false;
    }
    state_ = UploadState::Flushing;

    if (soManhSongSong_ > 1) {
        // Mảnh cuối (thường chưa đầy) cũng phải được giao đi, rồi đợi cho tất cả
        // mảnh đang bay hạ cánh mới được ghi siêu dữ liệu.
        if (buffer_ && chunkWritten_ > 0) {
            if (!giaoManhChoNen(error)) {
                std::string bo;
                thuHetManh(bo);
                ghiNhanLoi(error);
                return false;
            }
        } else if (buffer_) {
            buffer_->discard();
            buffer_.reset();
        }
        if (!thuHetManh(error)) {
            ghiNhanLoi(error);
            return false;
        }
        // Thu theo thứ tự nên chunkRecords_ đã đúng thứ tự sẵn, nhưng sắp lại
        // cho chắc: siêu dữ liệu sai thứ tự là tệp hỏng mà không ai báo.
        std::sort(chunkRecords_.begin(), chunkRecords_.end(),
                  [](const db::ChunkEntry& a, const db::ChunkEntry& b) {
                      return a.index < b.index;
                  });
    } else if (writer_ && chunkWritten_ > 0) {
        if (!closeChunk(error)) {
            ghiNhanLoi(error);
            return false;
        }
    } else if (writer_) {
        // Mảnh rỗng — huỷ bỏ.
        writer_->abort();
        writer_.reset();
        buffer_.reset();
    }

    uint8_t digest[32];
    hasher_.finish(digest);
    std::string sha = toHex(digest, 32);
    uint64_t actualSize = receivedBytes_.load();

    db::Database& database = manager_.db();

    // Khử trùng lặp sau khi tải xong: nếu đã có tệp giống hệt, bỏ dữ liệu vừa tải.
    if (manager_.config().storage.deduplicate && !sha.empty() && actualSize > 0) {
        std::vector<db::FileEntry> same;
        std::string findError;
        if (database.findByHash(sha, same, findError) && !same.empty()) {
            const db::FileEntry* src = nullptr;
            for (const auto& f : same)
                if (f.id != replaceFileId_ && f.size == actualSize) {
                    src = &f;
                    break;
                }
            if (src) {
                LOG_INFO(kTag, "[%s] Nội dung trùng với '%s' — dùng lại dữ liệu đã có",
                         id_.c_str(), src->path.c_str());
                // Xoá dữ liệu vừa đẩy lên để không tốn dung lượng.
                if (!uploaded_.empty()) {
                    std::string removeError;
                    manager_.engine().backend().removeChunks(uploaded_, removeError);
                    uploaded_.clear();
                }
                // Sao chép danh sách mảnh của tệp gốc.
                std::vector<db::ChunkEntry> srcChunks;
                if (database.listChunks(src->id, srcChunks, findError)) {
                    chunkRecords_ = srcChunks;
                    for (auto& c : chunkRecords_) c.id = 0;
                }
            }
        }
    }

    // Ghi mục tệp vào cơ sở dữ liệu.
    db::FileEntry entry;
    entry.parentId = parentId_;
    entry.name = name_;
    entry.path = normalizeVirtualPath(joinPath(targetFolderPath_, name_));
    entry.isFolder = false;
    entry.size = actualSize;
    entry.mimeType = mimeType_.empty() ? http::guessMimeType(name_) : mimeType_;
    entry.sha256 = sha;
    entry.quickHash = quickHash_;
    entry.chunkSize = chunkSize_;
    entry.chunkCount = static_cast<int>(chunkRecords_.size());
    entry.createdAt = nowUnix();
    entry.modifiedAt = entry.createdAt;
    entry.ownerId = ownerId_;

    if (policy_ == ConflictPolicy::Replace && replaceFileId_ > 0) {
        db::FileEntry old;
        std::string getError;
        if (database.getEntry(replaceFileId_, old, getError)) {
            std::string purgeError;
            manager_.engine().purgeFileData(old, purgeError);
            entry.id = old.id;
            entry.createdAt = old.createdAt;
            entry.starred = old.starred;
            entry.shareToken = old.shareToken;
            entry.shareExpiresAt = old.shareExpiresAt;
            if (!database.updateEntry(entry, error)) {
                state_ = UploadState::Failed;
                message_ = error;
                return false;
            }
        } else {
            if (!database.createEntry(entry, error)) {
                state_ = UploadState::Failed;
                message_ = error;
                return false;
            }
        }
    } else {
        if (!database.createEntry(entry, error)) {
            state_ = UploadState::Failed;
            message_ = error;
            return false;
        }
    }

    for (auto& c : chunkRecords_) {
        c.fileId = entry.id;
        c.id = 0;
        std::string chunkError;
        if (!database.addChunk(c, chunkError)) {
            LOG_ERROR(kTag, "[%s] Không ghi được thông tin mảnh %d: %s", id_.c_str(), c.index,
                      chunkError.c_str());
            error = chunkError;
            state_ = UploadState::Failed;
            message_ = error;
            return false;
        }
    }

    // Dọn mảnh MỒ CÔI trước khi buông danh sách. Chạy song song mà có mảnh
    // hỏng thì những mảnh bay SAU nó vẫn hạ cánh bình thường, nhưng chúng nằm
    // ngoài tiền tố liền mạch nên lượt gửi sau đã đẩy lại từ đầu chỗ đó — bản
    // cũ thành rác không tệp nào trỏ tới. Không dọn ở đây thì mỗi lần Telegram
    // hắt hơi là rò rỉ tới (số mảnh song song − 1) mảnh, vĩnh viễn.
    {
        std::vector<tg::ChunkLocation> moCoi;
        for (const auto& loc : uploaded_) {
            bool duocDung = false;
            for (const auto& rec : chunkRecords_) {
                if (rec.documentId == loc.documentId) { duocDung = true; break; }
            }
            if (!duocDung) moCoi.push_back(loc);
        }
        if (!moCoi.empty()) {
            LOG_INFO(kTag, "[%s] Dọn %zu mảnh mồ côi (đẩy xong nhưng nằm sau chỗ hỏng)",
                     id_.c_str(), moCoi.size());
            std::string loiDon;
            manager_.engine().backend().removeChunks(moCoi, loiDon);
            if (!loiDon.empty())
                LOG_WARN(kTag, "[%s] Dọn mảnh mồ côi chưa trọn: %s", id_.c_str(), loiDon.c_str());
        }
    }

    uploaded_.clear();  // đã thuộc về tệp, không rollback nữa
    state_ = UploadState::Completed;
    message_ = "Hoàn tất";
    out = entry;
    LOG_INFO(kTag, "[%s] Hoàn tất '%s' (%s, %d mảnh)", id_.c_str(), entry.name.c_str(),
             formatBytes(entry.size).c_str(), entry.chunkCount);
    return true;
}

void UploadSession::rollback() {
    if (uploaded_.empty()) return;
    LOG_INFO(kTag, "[%s] Dọn %zu mảnh đã tải lên", id_.c_str(), uploaded_.size());
    std::string error;
    manager_.engine().backend().removeChunks(uploaded_, error);
    if (!error.empty())
        LOG_WARN(kTag, "[%s] Dọn dữ liệu chưa trọn vẹn: %s", id_.c_str(), error.c_str());
    uploaded_.clear();
    chunkRecords_.clear();
}

void UploadSession::chotSoTruocKhiNoi() {
    std::lock_guard<std::mutex> lk(mu_);
    if (dangBay_.empty()) return;
    std::string error;
    if (!thuHetManh(error)) {
        LOG_WARN(kTag, "[%s] Có mảnh hỏng khi chốt sổ (%s) — lùi mốc nối lại về %s",
                 id_.c_str(), error.c_str(), formatBytes(committedBytes_).c_str());
        message_ = error;
    }
}

void UploadSession::cancel(const std::string& reason) {
    if (cancelled_.exchange(true)) return;
    std::lock_guard<std::mutex> lk(mu_);
    state_ = UploadState::Cancelled;
    message_ = reason.empty() ? "Đã huỷ theo yêu cầu" : reason;
    // Phải ĐỢI luồng nền dừng hẳn trước khi dọn: cờ cancelled_ ở trên làm chúng
    // bỏ cuộc sớm, nhưng mảnh nào đã gọi finish() rồi thì vẫn kịp nằm lại trên
    // Telegram. Dọn trước rồi mới đợi là để sót đúng những mảnh đó.
    while (!dangBay_.empty()) {
        std::unique_ptr<ManhBay> manh = std::move(dangBay_.front());
        dangBay_.pop_front();
        if (manh->luong.joinable()) manh->luong.join();
        if (manh->ok) uploaded_.push_back(manh->viTri);
    }
    if (writer_) {
        writer_->abort();
        writer_.reset();
    }
    if (buffer_) {
        buffer_->discard();
        buffer_.reset();
    }
    rollback();
    LOG_INFO(kTag, "[%s] Đã huỷ: %s", id_.c_str(), message_.c_str());
}

// ---------------------------------------------------------------------------
//  UploadManager
// ---------------------------------------------------------------------------
UploadManager::UploadManager(StorageEngine& engine, db::Database& database, const Config& config)
    : engine_(engine), db_(database), config_(config) {
    donTepTamBoLai();
}

// Quét tệp tạm sót lại từ lần chạy trước. Máy chủ bị kill giữa lúc đang đệm thì
// tệp .tmp nằm lại vĩnh viễn — không phiên nào biết tới chúng nữa. Trước đây
// chế độ stream không đụng tới thư mục tạm nên chẳng ai để ý; từ khi có đẩy song
// song thì mỗi lần chết là bỏ lại tới (số mảnh song song × cỡ mảnh).
void UploadManager::donTepTamBoLai() {
    std::string thuMuc = config_.resolvePath(config_.storage.spoolDirectory);
    if (!isDirectory(thuMuc)) return;
    uint64_t thuHoi = 0;
    int dem = 0;
    for (const std::string& ten : listDirectory(thuMuc)) {
        if (ten.size() < 4 || ten.compare(ten.size() - 4, 4, ".tmp") != 0) continue;
        std::string duong = joinPath(thuMuc, ten);
        uint64_t co = fileSizeOf(duong);
        if (removeFileIfExists(duong)) {
            thuHoi += co;
            ++dem;
        }
    }
    if (dem > 0)
        LOG_INFO(kTag, "Dọn %d tệp tạm bỏ lại từ lần chạy trước, thu hồi %s", dem,
                 formatBytes(thuHoi).c_str());
}

UploadManager::~UploadManager() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : sessions_) kv.second->cancel("Ứng dụng đang tắt");
    sessions_.clear();
}

std::vector<DuplicateInfo> UploadManager::findDuplicates(const std::string& name,
                                                         int64_t parentId,
                                                         const std::string& quickHash,
                                                         const std::string& sha256,
                                                         uint64_t size) {
    std::vector<DuplicateInfo> out;
    std::string error;

    // 1) Trùng tên trong cùng thư mục.
    db::FileEntry sameName;
    if (db_.findByNameInFolder(parentId, name, sameName, error) && !sameName.isFolder) {
        DuplicateInfo d;
        d.fileId = sameName.id;
        d.name = sameName.name;
        d.path = sameName.path;
        d.size = sameName.size;
        d.modifiedAt = sameName.modifiedAt;
        d.reason = (sameName.size == size && !sha256.empty() && sameName.sha256 == sha256)
                       ? "giống hệt"
                       : "cùng tên";
        out.push_back(d);
    }

    // 2) Trùng nội dung (băm đầy đủ nếu có, ngược lại dùng băm nhanh).
    std::vector<db::FileEntry> sameContent;
    if (!sha256.empty()) {
        db_.findByHash(sha256, sameContent, error);
    } else if (!quickHash.empty()) {
        db_.findByQuickHash(quickHash, size, sameContent, error);
    }
    for (const auto& f : sameContent) {
        bool already = false;
        for (const auto& d : out)
            if (d.fileId == f.id) already = true;
        if (already) continue;
        DuplicateInfo d;
        d.fileId = f.id;
        d.name = f.name;
        d.path = f.path;
        d.size = f.size;
        d.modifiedAt = f.modifiedAt;
        d.reason = sha256.empty() ? "nhiều khả năng cùng nội dung" : "cùng nội dung";
        out.push_back(d);
        if (out.size() >= 10) break;
    }
    return out;
}

bool UploadManager::ensureFolder(const std::string& path, int ownerId, int64_t& folderId,
                                 std::string& error) {
    std::string normalized = normalizeVirtualPath(path);
    if (normalized == "/") {
        folderId = 0;
        return true;
    }
    db::FileEntry existing;
    if (db_.getEntryByPath(normalized, existing, error)) {
        if (!existing.isFolder) {
            error = "Đường dẫn '" + normalized + "' đã là một tệp, không phải thư mục";
            return false;
        }
        folderId = existing.id;
        return true;
    }

    // Tạo đệ quy từ trên xuống.
    int64_t parent = 0;
    std::string current;
    for (const auto& part : split(normalized, '/', false)) {
        current += "/" + part;
        db::FileEntry found;
        std::string findError;
        if (db_.getEntryByPath(current, found, findError)) {
            if (!found.isFolder) {
                error = "Đường dẫn '" + current + "' đã là một tệp";
                return false;
            }
            parent = found.id;
            continue;
        }
        db::FileEntry folder;
        folder.parentId = parent;
        folder.name = part;
        folder.path = current;
        folder.isFolder = true;
        folder.ownerId = ownerId;
        folder.createdAt = nowUnix();
        folder.modifiedAt = folder.createdAt;
        if (!db_.createEntry(folder, error)) return false;
        parent = folder.id;
        LOG_DEBUG(kTag, "Đã tạo thư mục %s", current.c_str());
    }
    folderId = parent;
    return true;
}

UploadInitResult UploadManager::begin(const UploadInitRequest& req) {
    UploadInitResult result;
    result.chunkSize = config_.storage.chunkSize;
    result.browserChunkSize = config_.storage.browserChunkSize;

    std::string cleanName = sanitizeFileName(req.name);
    if (cleanName.empty()) {
        result.error = "Tên tệp không hợp lệ";
        return result;
    }

    std::string backendWhy;
    if (!engine_.backend().ready(backendWhy)) {
        result.error = backendWhy;
        return result;
    }

    int64_t parentId = 0;
    std::string error;
    if (!ensureFolder(req.targetFolderPath, req.ownerId, parentId, error)) {
        result.error = error;
        return result;
    }

    auto duplicates =
        findDuplicates(cleanName, parentId, req.quickHash, req.sha256, req.totalSize);
    result.duplicates = duplicates;

    ConflictPolicy policy = req.policy;
    int64_t replaceId = 0;

    if (!duplicates.empty()) {
        if (policy == ConflictPolicy::Ask) {
            result.needsDecision = true;
            result.ok = true;
            result.message = "Đã tìm thấy tệp trùng — hãy chọn cách xử lý.";
            return result;
        }
        if (policy == ConflictPolicy::Skip) {
            result.ok = true;
            result.skipped = true;
            result.message = "Đã bỏ qua vì tệp đã tồn tại.";
            return result;
        }
        if (policy == ConflictPolicy::LinkExisting) {
            // Tạo mục mới trỏ tới cùng dữ liệu.
            const DuplicateInfo* src = nullptr;
            for (const auto& d : duplicates)
                if (d.reason != "cùng tên") src = &d;
            if (!src) src = &duplicates[0];

            db::FileEntry source;
            if (!db_.getEntry(src->fileId, source, error)) {
                result.error = "Không đọc được tệp gốc: " + error;
                return result;
            }
            std::vector<db::ChunkEntry> chunks;
            if (!db_.listChunks(source.id, chunks, error)) {
                result.error = "Không đọc được danh sách mảnh: " + error;
                return result;
            }

            std::string finalName = cleanName;
            int counter = 1;
            db::FileEntry probe;
            std::string probeError;
            while (db_.findByNameInFolder(parentId, finalName, probe, probeError)) {
                ++counter;
                finalName = makeUniqueName(cleanName, counter);
                if (counter > 500) break;
            }

            db::FileEntry entry = source;
            entry.id = 0;
            entry.parentId = parentId;
            entry.name = finalName;
            entry.path = normalizeVirtualPath(joinPath(req.targetFolderPath, finalName));
            entry.createdAt = nowUnix();
            entry.modifiedAt = entry.createdAt;
            entry.ownerId = req.ownerId;
            entry.shareToken.clear();
            entry.shareExpiresAt = 0;
            entry.trashed = false;
            entry.trashedAt = 0;
            if (!db_.createEntry(entry, error)) {
                result.error = error;
                return result;
            }
            for (auto& c : chunks) {
                c.id = 0;
                c.fileId = entry.id;
                std::string chunkError;
                db_.addChunk(c, chunkError);
            }
            result.ok = true;
            result.linked = true;
            result.linkedFileId = entry.id;
            result.message = "Đã liên kết tới dữ liệu có sẵn, không tốn thêm dung lượng.";
            LOG_INFO(kTag, "Liên kết '%s' tới dữ liệu của '%s'", entry.path.c_str(),
                     source.path.c_str());
            return result;
        }
        if (policy == ConflictPolicy::Replace) {
            for (const auto& d : duplicates)
                if (d.reason == "cùng tên" || d.reason == "giống hệt") replaceId = d.fileId;
            if (replaceId == 0) replaceId = duplicates[0].fileId;
        }
        if (policy == ConflictPolicy::KeepBoth) {
            int counter = 1;
            std::string candidate = cleanName;
            db::FileEntry probe;
            std::string probeError;
            while (db_.findByNameInFolder(parentId, candidate, probe, probeError)) {
                ++counter;
                candidate = makeUniqueName(cleanName, counter);
                if (counter > 500) break;
            }
            cleanName = candidate;
        }
    }

    // Tạo phiên.
    std::string id = crypto::randomHex(12);
    auto session = std::make_shared<UploadSession>(*this, id, req.ownerId);
    {
        std::lock_guard<std::mutex> slk(session->mu_);
        session->name_ = cleanName;
        session->targetFolderPath_ = normalizeVirtualPath(req.targetFolderPath);
        session->parentId_ = parentId;
        session->totalSize_ = req.totalSize;
        session->mimeType_ = req.mimeType;
        session->quickHash_ = req.quickHash;
        session->policy_ = policy;
        session->replaceFileId_ = replaceId;
        session->chunkSize_ = config_.storage.chunkSize;
        session->chunkTotal_ =
            req.totalSize == 0
                ? 1
                : static_cast<int>((req.totalSize + session->chunkSize_ - 1) /
                                   session->chunkSize_);
        session->state_ = UploadState::Preparing;
    }

    // chonCachDay() phải chạy NGOÀI session->mu_: nó cần đếm RAM các phiên khác
    // nên phải xin mu_ của manager, mà claimResumable() lại khoá theo chiều
    // ngược lại (mu_ rồi mới tới session->mu_). Hai chiều gặp nhau là treo cứng.
    // Phiên chưa nằm trong sessions_ nên chưa ai với tới được — không cần khoá.
    chonCachDay(*session);

    {
        std::lock_guard<std::mutex> lk(mu_);
        sessions_[id] = session;
    }

    db::UploadRecord rec;
    rec.id = id;
    rec.ownerId = req.ownerId;
    rec.name = cleanName;
    rec.targetPath = normalizeVirtualPath(req.targetFolderPath);
    rec.totalSize = req.totalSize;
    rec.state = "dang-tai";
    rec.quickHash = req.quickHash;
    rec.createdAt = nowUnix();
    std::string saveError;
    db_.saveUpload(rec, saveError);

    result.ok = true;
    result.uploadId = id;
    result.message = "Sẵn sàng nhận dữ liệu.";
    // Ghi chế độ đệm THẬT SỰ dùng, không phải chế độ ghi trong cấu hình: chạy
    // song song có thể đã đổi stream thành đĩa. Nhật ký nói sai thì lần sau đọc
    // lại chính mình cũng chẩn đoán nhầm.
    LOG_INFO(kTag, "[%s] Bắt đầu tải '%s' (%s) vào %s — mảnh %s, đệm %s, %d mảnh song song",
             id.c_str(), cleanName.c_str(), formatBytes(req.totalSize).c_str(),
             rec.targetPath.c_str(), formatBytes(config_.storage.chunkSize).c_str(),
             bufferModeName(session->cheDoDem_), session->soManhSongSong_);
    return result;
}

std::shared_ptr<UploadSession> UploadManager::find(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(id);
    return it == sessions_.end() ? nullptr : it->second;
}

std::shared_ptr<UploadSession> UploadManager::claimResumable(int ownerId,
                                                             const std::string& folder,
                                                             const std::string& name,
                                                             uint64_t totalSize) {
    if (totalSize == 0) return nullptr;
    std::string key = normalizeVirtualPath(folder + "/" + name);
    std::shared_ptr<UploadSession> chon;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : sessions_) {
            auto& s = kv.second;
            if (!s || s->cancelled()) continue;
            UploadProgress p = s->progress();
            if (p.ownerId != ownerId) continue;
            if (p.state != UploadState::Receiving) continue;   // chỉ phiên đang dở
            if (p.receivedBytes == 0 || p.receivedBytes >= totalSize) continue;
            if (s->totalSize() != totalSize) continue;
            if (s->targetKey() != key) continue;
            if (!s->claim()) continue;   // một lượt PUT khác đang dùng
            chon = s;
            break;
        }
    }
    if (!chon) return nullptr;

    // Chốt sổ NGOÀI khoá của manager. Bước này join luồng nền — có thể mất hàng
    // phút nếu Telegram đang chậm — mà mu_ lại là khoá mọi find()/complete()/
    // cancel() và cả vòng hỏi tiến độ đều phải qua. Chốt sổ trong khoá thì một
    // lượt nối lại làm treo toàn bộ ứng dụng. Phiên đã được claim() ở trên nên
    // không ai giành mất trong lúc ta thả khoá.
    //
    // Phải chốt trước khi trả về: sau đây tầng HTTP đọc ngay receivedBytes() và
    // digestSoFar() làm mốc nối tiếp, mà đẩy song song thì hai giá trị đó chạy
    // trước phần thật sự đã nằm trên Telegram.
    chon->chotSoTruocKhiNoi();
    return chon;
}

bool UploadManager::complete(const std::string& id, db::FileEntry& out, std::string& error) {
    auto session = find(id);
    if (!session) {
        error = "Không tìm thấy phiên tải lên " + id;
        return false;
    }
    bool ok = session->complete(out, error);

    db::UploadRecord rec;
    std::string recError;
    if (db_.getUpload(id, rec, recError)) {
        rec.state = ok ? "hoan-tat" : "loi";
        rec.receivedBytes = session->receivedBytes();
        rec.message = ok ? "" : error;
        db_.saveUpload(rec, recError);
    }

    // CHỈ gỡ phiên khi đã xong hẳn. Gỡ lúc đang hỏng là thả nốt shared_ptr cuối
    // cùng, ~UploadSession() chạy rollback(), và rollback() xoá SẠCH mọi mảnh đã
    // đẩy lên — đúng cái bẫy đã ném đi 36 GB, chỉ khác là nấp ở đường hoàn tất
    // thay vì đường nhận dữ liệu. Mảnh cuối gặp FLOOD_WAIT hay 500 là mất cả tệp.
    //
    // Giữ lại thì không mất gì: bộ quét phiên quá hạn vẫn dọn sau 30 phút nếu
    // thật sự không ai gửi tiếp, mà trong 30 phút đó máy khách còn nối lại được.
    if (ok) {
        std::lock_guard<std::mutex> lk(mu_);
        sessions_.erase(id);
    } else {
        LOG_WARN(kTag, "[%s] Chưa hoàn tất được (%s) — giữ phiên để nối lại", id.c_str(),
                 error.c_str());
    }
    return ok;
}

bool UploadManager::cancel(const std::string& id, const std::string& reason) {
    auto session = find(id);
    if (!session) return false;
    session->cancel(reason);

    db::UploadRecord rec;
    std::string recError;
    if (db_.getUpload(id, rec, recError)) {
        rec.state = "huy";
        rec.message = reason;
        rec.receivedBytes = session->receivedBytes();
        db_.saveUpload(rec, recError);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        sessions_.erase(id);
    }
    return true;
}

std::vector<UploadProgress> UploadManager::activeUploads(int ownerId) const {
    std::vector<std::shared_ptr<UploadSession>> list;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& kv : sessions_) list.push_back(kv.second);
    }
    std::vector<UploadProgress> out;
    for (const auto& s : list) {
        UploadProgress p = s->progress();
        if (ownerId > 0 && p.ownerId != ownerId) continue;
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(),
              [](const UploadProgress& a, const UploadProgress& b) {
                  return a.startedAt < b.startedAt;
              });
    return out;
}

void UploadManager::reapStale() {
    int timeout = config_.storage.uploadIdleTimeoutSeconds;
    if (timeout <= 0) return;
    int64_t cutoff = nowUnix() - timeout;

    std::vector<std::shared_ptr<UploadSession>> stale;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& kv : sessions_) {
            if (kv.second->lastActivity() < cutoff) stale.push_back(kv.second);
        }
    }
    for (const auto& s : stale) {
        LOG_WARN(kTag, "[%s] Không có hoạt động trong %d giây — tự huỷ", s->id().c_str(),
                 timeout);
        cancel(s->id(), "Tự huỷ do không có hoạt động");
    }

    std::string error;
    db_.deleteStaleUploads(nowUnix() - 7 * 86400, error);
}

}  // namespace storage
}  // namespace ttd
