#include "tg/account_pool.h"

#include <algorithm>
#include <limits>
#include <thread>

#include "common/logging.h"
#include "common/timeutil.h"

namespace ttd {
namespace tg {

namespace {
constexpr const char* kTag = "tg.pool";

class PoolChunkWriter : public ChunkWriter {
public:
    PoolChunkWriter(AccountPool& pool, TgAccount* account,
                    std::unique_ptr<TgAccount::StreamUpload> upload, std::string label)
        : pool_(pool), account_(account), upload_(std::move(upload)), label_(std::move(label)) {
        pool_.noteUploadStarted(account_->id());
    }

    ~PoolChunkWriter() override {
        if (!released_) {
            pool_.noteUploadFinished(account_->id());
            released_ = true;
        }
    }

    bool write(const uint8_t* data, size_t len, std::string& error) override {
        if (!upload_) {
            error = "Phiên ghi đã đóng";
            return false;
        }
        return upload_->feed(data, len, error);
    }

    bool finish(ChunkLocation& out, std::string& error) override {
        if (!upload_) {
            error = "Phiên ghi đã đóng";
            return false;
        }
        bool ok = upload_->finish(out, error);
        upload_.reset();
        if (!released_) {
            pool_.noteUploadFinished(account_->id());
            released_ = true;
        }
        return ok;
    }

    void abort() override {
        if (upload_) {
            upload_->abort();
            upload_.reset();
        }
        if (!released_) {
            pool_.noteUploadFinished(account_->id());
            released_ = true;
        }
    }

    uint64_t written() const override { return upload_ ? upload_->bytesSent() : written_; }
    std::string sourceLabel() const override { return label_; }

private:
    AccountPool& pool_;
    TgAccount* account_;
    std::unique_ptr<TgAccount::StreamUpload> upload_;
    std::string label_;
    uint64_t written_ = 0;
    bool released_ = false;
};

}  // namespace

AccountPool::AccountPool(const TlSchema& schema, AppInfo appInfo)
    : schema_(schema), appInfo_(std::move(appInfo)) {}

AccountPool::~AccountPool() { disconnectAll(); }

void AccountPool::setSupergroup(const SupergroupRef& group) {
    std::lock_guard<std::mutex> lk(mu_);
    group_ = group;
}

SupergroupRef AccountPool::supergroup() const {
    std::lock_guard<std::mutex> lk(mu_);
    return group_;
}

void AccountPool::updateAppInfo(const AppInfo& info) {
    std::lock_guard<std::mutex> lk(mu_);
    appInfo_ = info;
    for (auto& a : accounts_) a->updateAppInfo(info);
    LOG_INFO(kTag, "Đã cập nhật thông tin ứng dụng cho %zu tài khoản (api_id %d)",
             accounts_.size(), info.apiId);
}

AppInfo AccountPool::appInfo() const {
    std::lock_guard<std::mutex> lk(mu_);
    return appInfo_;
}

TgAccount* AccountPool::addAccount(const TgAccountConfig& config) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& a : accounts_)
        if (a->id() == config.id) return a.get();
    auto acc = std::make_unique<TgAccount>(schema_, appInfo_, config);
    TgAccount* raw = acc.get();
    accounts_.push_back(std::move(acc));
    enabled_[config.id] = true;
    activeUploads_[config.id] = 0;
    LOG_INFO(kTag, "Đã thêm tài khoản #%d (%s)", config.id, config.label.c_str());
    return raw;
}

TgAccount* AccountPool::findAccount(int id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& a : accounts_)
        if (a->id() == id) return a.get();
    return nullptr;
}

void AccountPool::removeAccount(int id) {
    std::unique_ptr<TgAccount> victim;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (size_t i = 0; i < accounts_.size(); ++i) {
            if (accounts_[i]->id() == id) {
                victim = std::move(accounts_[i]);
                accounts_.erase(accounts_.begin() + static_cast<long>(i));
                break;
            }
        }
        enabled_.erase(id);
        activeUploads_.erase(id);
        displayNames_.erase(id);
    }
    if (victim) {
        victim->disconnect();
        LOG_INFO(kTag, "Đã gỡ tài khoản #%d", id);
    }
}

void AccountPool::setAccountEnabled(int id, bool enabled) {
    std::lock_guard<std::mutex> lk(mu_);
    enabled_[id] = enabled;
}

size_t AccountPool::accountCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return accounts_.size();
}

std::vector<AccountStatus> AccountPool::statuses() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<AccountStatus> out;
    out.reserve(accounts_.size());
    for (const auto& a : accounts_) {
        AccountStatus s;
        s.id = a->id();
        s.label = a->label();
        s.phone = a->config().phone;
        auto itName = displayNames_.find(a->id());
        s.displayName = itName == displayNames_.end() ? "" : itName->second;
        auto itEn = enabled_.find(a->id());
        s.enabled = itEn == enabled_.end() ? true : itEn->second;
        s.authorized = a->authorized();
        s.connected = a->connected();
        s.homeDc = a->config().homeDc;
        auto itUp = activeUploads_.find(a->id());
        s.activeUploads = itUp == activeUploads_.end() ? 0 : itUp->second;
        s.bytesUploaded = a->bytesUploaded();
        s.bytesDownloaded = a->bytesDownloaded();
        s.status = s.enabled ? a->statusText() : "Đã tắt";
        s.lastError = a->lastError();
        out.push_back(std::move(s));
    }
    return out;
}

void AccountPool::connectAll() {
    std::vector<TgAccount*> list;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& a : accounts_) {
            auto it = enabled_.find(a->id());
            if (it != enabled_.end() && !it->second) continue;
            list.push_back(a.get());
        }
    }
    for (TgAccount* a : list) {
        std::string error;
        if (!a->connect(error)) {
            LOG_WARN(kTag, "Tài khoản %s chưa kết nối được: %s", a->label().c_str(),
                     error.c_str());
            continue;
        }
        std::string displayName;
        int64_t userId = 0;
        if (a->fetchSelf(error, displayName, userId)) {
            std::lock_guard<std::mutex> lk(mu_);
            displayNames_[a->id()] = displayName;
        } else {
            LOG_WARN(kTag, "Tài khoản %s chưa đăng nhập: %s", a->label().c_str(),
                     error.c_str());
        }
    }
    persistAll();
}

void AccountPool::disconnectAll() {
    persistAll();
    std::vector<TgAccount*> list;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& a : accounts_) list.push_back(a.get());
    }
    for (TgAccount* a : list) a->disconnect();
}

void AccountPool::persistAll() {
    if (!persistFn_) return;
    std::vector<std::pair<int, std::map<int, AuthKey>>> snapshot;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& a : accounts_) snapshot.emplace_back(a->id(), a->exportSessions());
    }
    for (auto& kv : snapshot) persistFn_(kv.first, kv.second);
}

bool AccountPool::ready(std::string& why) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (accounts_.empty()) {
        why = "Chưa có tài khoản Telegram nào. Hãy thêm tài khoản trong phần Cài đặt.";
        return false;
    }
    if (!group_.valid()) {
        why = "Chưa chọn siêu nhóm lưu trữ. Hãy cấu hình trong phần Cài đặt.";
        return false;
    }
    int64_t now = nowUnix();
    for (const auto& a : accounts_) {
        auto it = enabled_.find(a->id());
        if (it != enabled_.end() && !it->second) continue;
        if (!a->authorized()) continue;
        (void)now;
        return true;
    }
    why = "Không có tài khoản Telegram nào đang đăng nhập.";
    return false;
}

TgAccount* AccountPool::pickAccount(std::string& error, const std::vector<int>& boQua) {
    std::lock_guard<std::mutex> lk(mu_);
    if (accounts_.empty()) {
        error = "Chưa có tài khoản Telegram nào";
        return nullptr;
    }
    auto biBoQua = [&boQua](int id) {
        return std::find(boQua.begin(), boQua.end(), id) != boQua.end();
    };
    TgAccount* best = nullptr;
    int bestLoad = std::numeric_limits<int>::max();
    int64_t now = nowUnix();

    // Xoay vòng điểm bắt đầu để phân bổ đều khi các tài khoản cùng tải.
    size_t start = roundRobin_.fetch_add(1) % accounts_.size();
    for (size_t i = 0; i < accounts_.size(); ++i) {
        TgAccount* a = accounts_[(start + i) % accounts_.size()].get();
        auto itEn = enabled_.find(a->id());
        if (itEn != enabled_.end() && !itEn->second) continue;
        if (!a->authorized()) continue;
        if (a->statusText() == "Đang chờ giới hạn tần suất") continue;
        if (biBoQua(a->id())) continue;
        (void)now;
        int load = activeUploads_.count(a->id()) ? activeUploads_[a->id()] : 0;
        if (load < bestLoad) {
            bestLoad = load;
            best = a;
        }
    }
    if (!best) error = "Không có tài khoản Telegram nào sẵn sàng";
    return best;
}

TgAccount* AccountPool::pickForRead(const ChunkLocation& loc, std::string& error) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& a : accounts_) {
            if (a->id() != loc.accountId) continue;
            auto itEn = enabled_.find(a->id());
            if (itEn != enabled_.end() && !itEn->second) break;
            if (a->authorized()) return a.get();
            break;
        }
    }
    return pickAccount(error);
}

void AccountPool::noteUploadStarted(int accountId) {
    std::lock_guard<std::mutex> lk(mu_);
    activeUploads_[accountId]++;
}

void AccountPool::noteUploadFinished(int accountId) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = activeUploads_.find(accountId);
    if (it != activeUploads_.end() && it->second > 0) it->second--;
}

std::unique_ptr<ChunkWriter> AccountPool::beginChunk(uint64_t totalSize,
                                                     const std::string& chunkName,
                                                     std::string& error) {
    SupergroupRef group = supergroup();
    if (!group.valid()) {
        error = "Chưa cấu hình siêu nhóm lưu trữ";
        return nullptr;
    }
    // Thử tài khoản rảnh nhất trước; tài khoản nào không vào được siêu nhóm thì
    // bỏ qua và thử tài khoản kế tiếp, thay vì làm hỏng cả phiên tải lên.
    std::vector<int> daThu;
    std::string lyDoCuoi;
    for (size_t vong = 0; vong < accountCount() + 1; ++vong) {
        TgAccount* account = pickAccount(error, daThu);
        if (!account) break;
        daThu.push_back(account->id());

        std::string connectError;
        if (!account->connected() && !account->connect(connectError)) {
            lyDoCuoi = "Tài khoản " + account->label() + " không kết nối được: " + connectError;
            LOG_WARN(kTag, "%s", lyDoCuoi.c_str());
            continue;
        }

        auto upload = account->beginStreamUpload(group, totalSize, chunkName);
        if (!upload) {
            lyDoCuoi = account->lastError().empty()
                           ? ("Tài khoản " + account->label() + " không gửi được vào siêu nhóm")
                           : account->lastError();
            continue;
        }
        return std::unique_ptr<ChunkWriter>(
            new PoolChunkWriter(*this, account, std::move(upload), account->label()));
    }

    if (!lyDoCuoi.empty()) error = lyDoCuoi;
    else if (error.empty()) error = "Không có tài khoản Telegram nào dùng được";
    return nullptr;
}

bool AccountPool::readRange(ChunkLocation& loc, uint64_t offset, uint32_t limit,
                            Bytes& out, std::string& error) {
    TgAccount* account = pickForRead(loc, error);
    if (!account) return false;

    const SupergroupRef group = supergroup();
    ChunkLocation working = loc;

    // Dữ liệu nằm trong siêu nhóm chứ không nằm "trong" tài khoản nào, nên tài
    // khoản nào còn trong nhóm cũng đọc được. Nhưng access_hash của tài liệu do
    // Telegram cấp RIÊNG cho từng tài khoản, y như access_hash của kênh: tài
    // khoản thay thế phải hỏi lại Telegram theo message_id trước, dùng lại
    // access_hash của người đã tải lên thì bị từ chối ngay.
    if (account->id() != working.accountId && group.valid()) {
        std::string refreshError;
        if (account->refreshFileReference(group, working, refreshError)) {
            working.accountId = account->id();
            LOG_INFO(kTag, "Mảnh do tài khoản #%d tải lên nay đọc bằng %s — đã hỏi lại tham chiếu",
                     loc.accountId, account->label().c_str());
        } else if (!refreshError.empty()) {
            LOG_WARN(kTag, "Không hỏi lại được tham chiếu bằng %s: %s", account->label().c_str(),
                     refreshError.c_str());
        }
    }

    if (account->downloadRange(working, offset, limit, out, error)) {
        loc = working;
        return true;
    }

    // Hỏng vì lý do gì cũng thử hỏi lại tham chiếu đúng một lần rồi đọc lại:
    // tham chiếu tệp thì có hạn dùng, còn access_hash thì có thể đang là của
    // tài khoản khác. Chỉ bắt riêng lỗi FILE_REFERENCE là bỏ sót trường hợp
    // sau — và khi trong nhóm chỉ còn đúng một tài khoản thì không còn ai khác
    // để thử tiếp.
    if (group.valid()) {
        std::string refreshError;
        if (account->refreshFileReference(group, working, refreshError)) {
            working.accountId = account->id();
            if (account->downloadRange(working, offset, limit, out, error)) {
                LOG_DEBUG(kTag, "Đã làm mới tham chiếu và đọc lại thành công");
                loc = working;
                return true;
            }
        } else if (!refreshError.empty()) {
            error += " | Làm mới tham chiếu thất bại: " + refreshError;
        }
    }

    // Thử tài khoản khác nếu tài khoản hiện tại gặp trục trặc.
    std::vector<TgAccount*> others;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& a : accounts_) {
            if (a.get() == account) continue;
            auto itEn = enabled_.find(a->id());
            if (itEn != enabled_.end() && !itEn->second) continue;
            if (!a->authorized()) continue;
            others.push_back(a.get());
        }
    }
    for (TgAccount* a : others) {
        ChunkLocation alt = loc;
        std::string err2;
        if (group.valid() && a->refreshFileReference(group, alt, err2)) alt.accountId = a->id();
        if (a->downloadRange(alt, offset, limit, out, err2)) {
            LOG_INFO(kTag, "Đọc lại bằng tài khoản %s thành công", a->label().c_str());
            loc = alt;
            return true;
        }
    }
    if (!others.empty())
        error += " | Đã thử thêm " + std::to_string(others.size()) + " tài khoản khác";
    return false;
}

bool AccountPool::removeChunks(const std::vector<ChunkLocation>& locations,
                               std::string& error) {
    if (locations.empty()) return true;
    SupergroupRef group = supergroup();
    if (!group.valid()) {
        error = "Chưa cấu hình siêu nhóm lưu trữ";
        return false;
    }
    // Gom theo tài khoản đã tải lên; nếu tài khoản đó không dùng được thì
    // dùng bất kỳ tài khoản nào còn hoạt động (quyền xoá thuộc về nhóm).
    std::map<int, std::vector<int64_t>> byAccount;
    for (const auto& loc : locations) byAccount[loc.accountId].push_back(loc.messageId);

    bool anyFailure = false;
    for (auto& kv : byAccount) {
        TgAccount* a = findAccount(kv.first);
        std::string pickError;
        if (!a || !a->authorized()) a = pickAccount(pickError);
        if (!a) {
            error = pickError.empty() ? "Không có tài khoản để xoá dữ liệu" : pickError;
            anyFailure = true;
            continue;
        }
        std::string err;
        if (!a->deleteMessages(group, kv.second, err)) {
            LOG_WARN(kTag, "Xoá %zu thông điệp thất bại: %s", kv.second.size(), err.c_str());
            error = err;
            anyFailure = true;
        }
    }
    return !anyFailure;
}

BackendStats AccountPool::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    BackendStats s;
    s.totalAccounts = static_cast<int>(accounts_.size());
    for (const auto& a : accounts_) {
        auto it = enabled_.find(a->id());
        bool en = it == enabled_.end() ? true : it->second;
        if (en && a->authorized()) s.readyAccounts++;
        s.bytesUploaded += a->bytesUploaded();
        s.bytesDownloaded += a->bytesDownloaded();
    }
    return s;
}

}  // namespace tg
}  // namespace ttd
