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

    writer_ = manager_.engine().backend().beginChunk(thisChunkSize, chunkName, error);
    if (!writer_) return false;
    currentAccount_ = writer_->sourceLabel();

    BufferMode mode = parseBufferMode(manager_.config().storage.bufferMode);
    std::string spool = manager_.config().resolvePath(
        joinPath(manager_.config().storage.spoolDirectory, chunkName + ".tmp"));

    tg::ChunkWriter* w = writer_.get();
    UploadSession* self = this;
    buffer_.reset(new ChunkBuffer(
        mode, thisChunkSize, spool,
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
    return true;
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

    size_t offset = 0;
    while (offset < len) {
        if (cancelled_.load()) {
            error = "Phiên tải lên đã bị huỷ";
            return false;
        }
        if (!writer_) {
            if (!openChunk(error)) {
                state_ = UploadState::Failed;
                message_ = error;
                return false;
            }
        }
        uint64_t remainingInChunk = chunkSize_ - chunkWritten_;
        size_t take = static_cast<size_t>(
            std::min<uint64_t>(remainingInChunk, static_cast<uint64_t>(len - offset)));
        if (take == 0) {
            if (!closeChunk(error)) {
                state_ = UploadState::Failed;
                message_ = error;
                return false;
            }
            continue;
        }
        if (!buffer_->append(data + offset, take, error)) {
            state_ = UploadState::Failed;
            message_ = error;
            return false;
        }
        hasher_.update(data + offset, take);
        chunkHasher_.update(data + offset, take);
        chunkWritten_ += take;
        offset += take;
        receivedBytes_.fetch_add(take);

        if (chunkWritten_ >= chunkSize_) {
            if (!closeChunk(error)) {
                state_ = UploadState::Failed;
                message_ = error;
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

    if (writer_ && chunkWritten_ > 0) {
        if (!closeChunk(error)) {
            state_ = UploadState::Failed;
            message_ = error;
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

void UploadSession::cancel(const std::string& reason) {
    if (cancelled_.exchange(true)) return;
    std::lock_guard<std::mutex> lk(mu_);
    state_ = UploadState::Cancelled;
    message_ = reason.empty() ? "Đã huỷ theo yêu cầu" : reason;
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
    : engine_(engine), db_(database), config_(config) {}

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
    LOG_INFO(kTag, "[%s] Bắt đầu tải '%s' (%s) vào %s — mảnh %s, chế độ đệm %s", id.c_str(),
             cleanName.c_str(), formatBytes(req.totalSize).c_str(),
             rec.targetPath.c_str(), formatBytes(config_.storage.chunkSize).c_str(),
             config_.storage.bufferMode.c_str());
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
        return s;
    }
    return nullptr;
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

    {
        std::lock_guard<std::mutex> lk(mu_);
        sessions_.erase(id);
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
