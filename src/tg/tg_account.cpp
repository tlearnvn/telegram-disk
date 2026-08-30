#include "tg/tg_account.h"

#include <algorithm>
#include <cstring>
#include <thread>

#include "common/logging.h"
#include "common/strutil.h"
#include "common/timeutil.h"
#include "crypto/random.h"
#include "tg/dc_config.h"

namespace ttd {
namespace tg {

namespace {
constexpr const char* kTag = "tg.acc";
constexpr uint32_t kDownloadBlock = 1024 * 1024;  // 1 MB — giới hạn của upload.getFile

bool isMigrationError(const std::string& msg, int& dcOut) {
    static const char* kPrefixes[] = {"FILE_MIGRATE_", "NETWORK_MIGRATE_", "PHONE_MIGRATE_",
                                      "USER_MIGRATE_", "STATS_MIGRATE_"};
    for (const char* p : kPrefixes) {
        if (startsWith(msg, p)) {
            dcOut = std::atoi(msg.c_str() + std::strlen(p));
            return dcOut > 0;
        }
    }
    return false;
}
}  // namespace

TgAccount::TgAccount(const TlSchema& schema, AppInfo appInfo, TgAccountConfig config)
    : schema_(schema), appInfo_(std::move(appInfo)), config_(std::move(config)) {}

TgAccount::~TgAccount() { disconnect(); }

void TgAccount::loadSession(int dcId, const Bytes& authKey, int64_t serverSalt) {
    if (authKey.size() != 256) return;
    std::lock_guard<std::mutex> lk(mu_);
    AuthKey k;
    k.key = authKey;
    k.serverSalt = serverSalt;
    k.computeKeyId();
    loadedKeys_[dcId] = k;
}

std::map<int, AuthKey> TgAccount::exportSessions() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<int, AuthKey> out = loadedKeys_;
    for (const auto& kv : sessions_) {
        AuthKey k = kv.second->authKey();
        if (k.valid()) out[kv.first] = k;
    }
    return out;
}

MtprotoSession* TgAccount::createSession(int dcId, std::string& error) {
    SessionOptions opt;
    opt.dcId = dcId;
    opt.testMode = config_.testMode;
    opt.obfuscated = config_.obfuscated;
    opt.requestTimeoutMs = config_.requestTimeoutMs;
    opt.label = config_.label + "/DC" + std::to_string(dcId);

    auto session = std::make_unique<MtprotoSession>(schema_, appInfo_, opt);
    auto it = loadedKeys_.find(dcId);
    if (it != loadedKeys_.end()) session->setAuthKey(it->second);

    MtprotoSession* raw = session.get();
    sessions_[dcId] = std::move(session);
    if (!raw->ensureConnected(error)) {
        sessions_.erase(dcId);
        return nullptr;
    }
    return raw;
}

MtprotoSession* TgAccount::session(int dcId, std::string& error) {
    if (dcId <= 0) dcId = config_.homeDc;

    MtprotoSession* s = nullptr;
    bool needsAuthorize = false;
    {
        std::unique_lock<std::mutex> lk(mu_);
        auto it = sessions_.find(dcId);
        if (it != sessions_.end()) {
            if (it->second->connected()) return it->second.get();
            if (it->second->ensureConnected(error)) return it->second.get();
            sessions_.erase(it);
        }
        s = createSession(dcId, error);
        if (!s) return nullptr;
        needsAuthorize = (dcId != config_.homeDc) && authorized_.load() && !s->authorized();
    }

    // Uỷ quyền phiên trên DC phụ được làm ngoài vùng khoá vì nó gọi API qua mạng.
    if (needsAuthorize && !authorizeSession(dcId, s, error)) {
        LOG_WARN(kTag, "[%s] Không uỷ quyền được DC%d: %s", config_.label.c_str(), dcId,
                 error.c_str());
        std::lock_guard<std::mutex> lk(mu_);
        sessions_.erase(dcId);
        return nullptr;
    }
    return s;
}

bool TgAccount::authorizeSession(int dcId, MtprotoSession* target, std::string& error) {
    MtprotoSession* home = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(config_.homeDc);
        if (it != sessions_.end()) home = it->second.get();
    }
    if (!home) {
        error = "Chưa có phiên trên DC nhà để xuất uỷ quyền";
        return false;
    }

    TlValue req = TlValue::makeObject("auth.exportAuthorization");
    req.setInt("dc_id", dcId);
    InvokeResult res = home->invoke(req);
    if (!res.ok) {
        error = res.describe();
        return false;
    }
    if (!res.value.is("auth.exportedAuthorization")) {
        error = "Kết quả exportAuthorization không mong đợi: " + res.value.ctorName();
        return false;
    }

    TlValue imp = TlValue::makeObject("auth.importAuthorization");
    imp.setLong("id", res.value["id"].asLong());
    imp.setBytes("bytes", res.value["bytes"].asBytes());
    InvokeResult res2 = target->invoke(imp);
    if (!res2.ok && !res2.partial) {
        error = res2.describe();
        return false;
    }
    target->markAuthorized(true);
    LOG_INFO(kTag, "[%s] Đã uỷ quyền phiên trên DC%d", config_.label.c_str(), dcId);
    return true;
}

bool TgAccount::connect(std::string& error) {
    DcConfig::instance().loadDefaults();
    MtprotoSession* s = session(config_.homeDc, error);
    if (!s) {
        setLastError(error);
        return false;
    }
    return true;
}

void TgAccount::disconnect() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : sessions_) {
        AuthKey k = kv.second->authKey();
        if (k.valid()) loadedKeys_[kv.first] = k;
        kv.second->disconnect();
    }
    sessions_.clear();
}

bool TgAccount::connected() const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = sessions_.find(config_.homeDc);
    return it != sessions_.end() && it->second->connected();
}

void TgAccount::setLastError(const std::string& e) {
    std::lock_guard<std::mutex> lk(mu_);
    lastError_ = e;
}

std::string TgAccount::lastError() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lastError_;
}

std::string TgAccount::statusText() const {
    int64_t wait = floodWaitUntil_.load();
    if (wait > nowUnix()) return "Đang chờ giới hạn tần suất";
    if (!authorized_.load()) return "Chưa đăng nhập";
    if (!connected()) return "Mất kết nối";
    return "Sẵn sàng";
}

bool TgAccount::handleMigration(const RpcError& err, int& dcIdInOut) {
    int dc = 0;
    if (isMigrationError(err.message, dc)) {
        LOG_INFO(kTag, "[%s] Máy chủ yêu cầu chuyển sang DC%d", config_.label.c_str(), dc);
        dcIdInOut = dc;
        return true;
    }
    return false;
}

InvokeResult TgAccount::invoke(const TlValue& request, int dcId, int timeoutMs) {
    if (dcId <= 0) dcId = config_.homeDc;

    int64_t wait = floodWaitUntil_.load();
    if (wait > nowUnix()) {
        InvokeResult r;
        r.localError = "Tài khoản đang bị Telegram giới hạn tần suất tới " +
                       formatDateTime(wait);
        return r;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string error;
        MtprotoSession* s = session(dcId, error);
        if (!s) {
            InvokeResult r;
            r.localError = error;
            setLastError(error);
            return r;
        }
        InvokeResult res = s->invoke(request, timeoutMs);
        if (res.ok || res.partial) return res;

        if (!res.error.empty()) {
            int newDc = dcId;
            if (handleMigration(res.error, newDc)) {
                dcId = newDc;
                if (attempt == 0) config_.homeDc = newDc;  // PHONE_MIGRATE/USER_MIGRATE
                continue;
            }
            if (startsWith(res.error.message, "FLOOD_WAIT_")) {
                int seconds = res.error.value;
                floodWaitUntil_.store(nowUnix() + seconds);
                LOG_WARN(kTag, "[%s] Telegram giới hạn tần suất %d giây", config_.label.c_str(),
                         seconds);
                setLastError("Bị giới hạn tần suất " + std::to_string(seconds) + " giây");
                return res;
            }
            if (res.error.message == "AUTH_KEY_UNREGISTERED" ||
                res.error.message == "SESSION_REVOKED" ||
                res.error.message == "USER_DEACTIVATED") {
                authorized_.store(false);
                setLastError("Phiên đăng nhập không còn hiệu lực: " + res.error.message);
                return res;
            }
            return res;
        }
        // Lỗi mạng: thử kết nối lại một lần.
        LOG_DEBUG(kTag, "[%s] %s thất bại (%s), thử lại", config_.label.c_str(),
                  request.ctorName().c_str(), res.localError.c_str());
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = sessions_.find(dcId);
            if (it != sessions_.end()) it->second->disconnect();
        }
        if (attempt + 1 < 3) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        else {
            setLastError(res.localError);
            return res;
        }
    }
    InvokeResult r;
    r.localError = "Không gọi được API sau nhiều lần thử";
    return r;
}

bool TgAccount::refreshDcConfig(std::string& error) {
    TlValue req = TlValue::makeObject("help.getConfig");
    InvokeResult res = invoke(req);
    if (!res.ok && !res.partial) {
        error = res.describe();
        return false;
    }
    const TlValue& cfg = res.value;
    std::vector<DcEndpoint> endpoints;
    for (const auto& item : cfg["dc_options"].asVector()) {
        if (!item.is("dcOption")) continue;
        DcEndpoint e;
        e.dcId = item["id"].asInt();
        e.ip = item["ip_address"].asString();
        e.port = static_cast<uint16_t>(item["port"].asInt());
        e.ipv6 = item["ipv6"].asBool();
        e.mediaOnly = item["media_only"].asBool();
        e.cdn = item["cdn"].asBool();
        e.testMode = config_.testMode;
        if (!e.ip.empty() && e.dcId > 0) endpoints.push_back(e);
    }
    if (!endpoints.empty()) DcConfig::instance().updateFromConfig(endpoints);
    return true;
}

bool TgAccount::fetchSelf(std::string& error, std::string& displayName, int64_t& userId) {
    TlValue req = TlValue::makeObject("users.getUsers");
    TlVector ids;
    ids.push_back(TlValue::makeObject("inputUserSelf"));
    req.setVector("id", std::move(ids));

    InvokeResult res = invoke(req);
    if (!res.ok && !res.partial) {
        error = res.describe();
        return false;
    }
    const TlValue* user = findFirstObject(res.value, "user");
    if (!user) {
        error = "Không đọc được thông tin tài khoản";
        return false;
    }
    userId = (*user)["id"].asLong();
    std::string first = (*user)["first_name"].asString();
    std::string last = (*user)["last_name"].asString();
    std::string uname = (*user)["username"].asString();
    displayName = trim(first + " " + last);
    if (displayName.empty()) displayName = uname.empty() ? ("ID " + std::to_string(userId))
                                                         : ("@" + uname);
    else if (!uname.empty()) displayName += " (@" + uname + ")";
    authorized_.store(true);
    return true;
}

bool TgAccount::resolveSupergroupByUsername(const std::string& username, SupergroupRef& out,
                                            std::string& error) {
    std::string clean = username;
    if (startsWith(clean, "https://t.me/")) clean = clean.substr(13);
    else if (startsWith(clean, "http://t.me/")) clean = clean.substr(12);
    else if (startsWith(clean, "t.me/")) clean = clean.substr(5);
    if (!clean.empty() && clean[0] == '@') clean = clean.substr(1);
    size_t slash = clean.find('/');
    if (slash != std::string::npos) clean = clean.substr(0, slash);
    if (clean.empty()) {
        error = "Tên siêu nhóm rỗng";
        return false;
    }

    TlValue req = TlValue::makeObject("contacts.resolveUsername");
    req.setBytes("username", clean);
    InvokeResult res = invoke(req);
    if (!res.ok && !res.partial) {
        error = res.describe();
        return false;
    }
    const TlValue* ch = findFirstObject(res.value, "channel");
    if (!ch) {
        error = "Không tìm thấy siêu nhóm '" + clean + "' (hoặc đây không phải siêu nhóm)";
        return false;
    }
    out.channelId = (*ch)["id"].asLong();
    out.accessHash = (*ch)["access_hash"].asLong();
    out.title = (*ch)["title"].asString();
    if (!out.valid()) {
        error = "Siêu nhóm trả về thiếu thông tin";
        return false;
    }
    return true;
}

bool TgAccount::resolveSupergroupById(int64_t channelId, int64_t accessHashHint,
                                      SupergroupRef& out, std::string& error) {
    TlValue inputChannel = TlValue::makeObject("inputChannel");
    inputChannel.setLong("channel_id", channelId);
    inputChannel.setLong("access_hash", accessHashHint);

    TlValue req = TlValue::makeObject("channels.getChannels");
    TlVector ids;
    ids.push_back(inputChannel);
    req.setVector("id", std::move(ids));

    InvokeResult res = invoke(req);
    if (!res.ok && !res.partial) {
        error = res.describe();
        return false;
    }
    const TlValue* ch = findFirstObject(res.value, "channel");
    if (!ch) ch = findFirstObject(res.value, "channelForbidden");
    if (!ch) {
        error = "Không truy cập được siêu nhóm " + std::to_string(channelId);
        return false;
    }
    out.channelId = (*ch)["id"].asLong();
    out.accessHash = (*ch)["access_hash"].asLong();
    out.title = (*ch)["title"].asString();
    return out.valid();
}

bool TgAccount::listSupergroups(std::vector<SupergroupRef>& out, std::string& error) {
    TlValue req = TlValue::makeObject("messages.getDialogs");
    req.setInt("offset_date", 0);
    req.setInt("offset_id", 0);
    req.set("offset_peer", TlValue::makeObject("inputPeerEmpty"));
    req.setInt("limit", 100);
    req.setLong("hash", 0);

    InvokeResult res = invoke(req);
    if (!res.ok && !res.partial) {
        error = res.describe();
        return false;
    }
    std::vector<const TlValue*> channels;
    collectObjects(res.value, "channel", channels);
    for (const TlValue* ch : channels) {
        // Chỉ nhận siêu nhóm (megagroup), bỏ qua kênh phát sóng.
        if (!(*ch)["megagroup"].asBool()) continue;
        SupergroupRef ref;
        ref.channelId = (*ch)["id"].asLong();
        ref.accessHash = (*ch)["access_hash"].asLong();
        ref.title = (*ch)["title"].asString();
        if (ref.valid()) out.push_back(ref);
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Tải lên
// ---------------------------------------------------------------------------
namespace {

TlValue makeInputPeer(const SupergroupRef& group) {
    TlValue peer = TlValue::makeObject("inputPeerChannel");
    peer.setLong("channel_id", group.channelId);
    peer.setLong("access_hash", group.accessHash);
    return peer;
}

TlValue makeInputChannel(const SupergroupRef& group) {
    TlValue ch = TlValue::makeObject("inputChannel");
    ch.setLong("channel_id", group.channelId);
    ch.setLong("access_hash", group.accessHash);
    return ch;
}

int pickPartSize(uint64_t totalSize) {
    // Telegram cho phép tối đa 4000 phần; chọn kích thước phần sao cho đủ chỗ.
    uint64_t size = 512 * 1024;
    while (totalSize > size * 4000 && size < 2u * 1024 * 1024) size *= 2;
    return static_cast<int>(size);
}

}  // namespace

TgAccount::StreamUpload::StreamUpload(TgAccount& account, SupergroupRef group,
                                      uint64_t totalSize, std::string fileName)
    : account_(account), group_(std::move(group)), totalSize_(totalSize),
      fileName_(std::move(fileName)) {
    fileId_ = crypto::randomInt64();
    partSize_ = pickPartSize(totalSize_);
    totalParts_ = static_cast<int>((totalSize_ + static_cast<uint64_t>(partSize_) - 1) /
                                   static_cast<uint64_t>(partSize_));
    if (totalParts_ < 1) totalParts_ = 1;
    buffer_.reserve(static_cast<size_t>(partSize_));
}

bool TgAccount::StreamUpload::flushPart(std::string& error, bool last) {
    if (buffer_.empty() && !last) return true;
    if (buffer_.empty() && last) return true;
    if (nextPart_ >= totalParts_) {
        error = "Số phần vượt quá dự kiến — kích thước tệp không khớp";
        return false;
    }

    TlValue req = TlValue::makeObject("upload.saveBigFilePart");
    req.setLong("file_id", fileId_);
    req.setInt("file_part", nextPart_);
    req.setInt("file_total_parts", totalParts_);
    req.setBytes("bytes", buffer_);

    InvokeResult res = account_.invoke(req);
    if (!res.ok) {
        error = "Tải phần " + std::to_string(nextPart_ + 1) + "/" +
                std::to_string(totalParts_) + " thất bại: " + res.describe();
        return false;
    }
    if (!res.value.asBool(true)) {
        error = "Telegram từ chối phần " + std::to_string(nextPart_ + 1);
        return false;
    }
    account_.bytesUploaded_.fetch_add(buffer_.size());
    sent_ += buffer_.size();
    ++nextPart_;
    buffer_.clear();
    return true;
}

bool TgAccount::StreamUpload::feed(const uint8_t* data, size_t len, std::string& error) {
    if (aborted_) {
        error = "Phiên tải lên đã bị huỷ";
        return false;
    }
    while (len > 0) {
        size_t space = static_cast<size_t>(partSize_) - buffer_.size();
        size_t take = std::min(space, len);
        buffer_.insert(buffer_.end(), data, data + take);
        data += take;
        len -= take;
        if (buffer_.size() == static_cast<size_t>(partSize_)) {
            if (!flushPart(error, false)) return false;
        }
    }
    return true;
}

bool TgAccount::StreamUpload::finish(ChunkLocation& out, std::string& error) {
    if (aborted_) {
        error = "Phiên tải lên đã bị huỷ";
        return false;
    }
    if (!buffer_.empty()) {
        if (!flushPart(error, true)) return false;
    }
    if (nextPart_ != totalParts_) {
        // Người dùng gửi ít dữ liệu hơn khai báo — điều chỉnh lại tổng số phần.
        LOG_WARN(kTag, "Số phần thực tế (%d) khác dự kiến (%d)", nextPart_, totalParts_);
        totalParts_ = nextPart_;
    }

    TlValue inputFile = TlValue::makeObject("inputFileBig");
    inputFile.setLong("id", fileId_);
    inputFile.setInt("parts", totalParts_);
    inputFile.setBytes("name", fileName_);

    TlValue attrName = TlValue::makeObject("documentAttributeFilename");
    attrName.setBytes("file_name", fileName_);
    TlVector attrs;
    attrs.push_back(attrName);

    TlValue media = TlValue::makeObject("inputMediaUploadedDocument");
    media.setFlag("force_file");
    media.set("file", inputFile);
    media.setBytes("mime_type", std::string("application/octet-stream"));
    media.setVector("attributes", std::move(attrs));

    TlValue send = TlValue::makeObject("messages.sendMedia");
    send.set("peer", makeInputPeer(group_));
    send.set("media", media);
    send.setBytes("message", fileName_);
    send.setLong("random_id", crypto::randomInt64());

    InvokeResult res = account_.invoke(send);
    if (!res.ok && !res.partial) {
        error = "Không gửi được tệp vào siêu nhóm: " + res.describe();
        return false;
    }

    const TlValue* doc = findFirstObject(res.value, "document");
    if (!doc) {
        error = "Telegram không trả về thông tin tài liệu sau khi gửi";
        return false;
    }
    out.documentId = (*doc)["id"].asLong();
    out.accessHash = (*doc)["access_hash"].asLong();
    out.fileReference = (*doc)["file_reference"].asBytes();
    out.dcId = (*doc)["dc_id"].asInt();
    out.size = static_cast<uint64_t>((*doc)["size"].asLong());
    out.accountId = account_.id();
    out.fileName = fileName_;

    // Mã thông điệp: ưu tiên updateMessageID, sau đó tới message.id.
    const TlValue* upd = findFirstObject(res.value, "updateMessageID");
    if (upd) {
        out.messageId = (*upd)["id"].asInt();
    } else {
        const TlValue* msg = findFirstObject(res.value, "message");
        if (msg) out.messageId = (*msg)["id"].asInt();
    }
    if (out.messageId == 0) {
        error = "Không lấy được mã thông điệp của mảnh dữ liệu";
        return false;
    }
    if (out.size == 0) out.size = sent_;
    finished_ = true;
    LOG_DEBUG(kTag, "[%s] Đã lưu mảnh %s (%s) -> msg=%lld doc=%lld dc=%d",
              account_.label().c_str(), fileName_.c_str(), formatBytes(out.size).c_str(),
              static_cast<long long>(out.messageId), static_cast<long long>(out.documentId),
              out.dcId);
    return true;
}

void TgAccount::StreamUpload::abort() {
    aborted_ = true;
    buffer_.clear();
}

std::unique_ptr<TgAccount::StreamUpload> TgAccount::beginStreamUpload(
    const SupergroupRef& group, uint64_t totalSize, const std::string& fileName) {
    return std::unique_ptr<StreamUpload>(new StreamUpload(*this, group, totalSize, fileName));
}

bool TgAccount::uploadChunk(const SupergroupRef& group, const Bytes& data,
                            const std::string& fileName, ChunkLocation& out,
                            const ProgressCallback& progress, const CancelCheck& cancelled,
                            std::string& error) {
    auto up = beginStreamUpload(group, data.size(), fileName);
    size_t offset = 0;
    const size_t kStep = 512 * 1024;
    while (offset < data.size()) {
        if (cancelled && cancelled()) {
            up->abort();
            error = "Đã huỷ theo yêu cầu";
            return false;
        }
        size_t take = std::min(kStep, data.size() - offset);
        if (!up->feed(data.data() + offset, take, error)) {
            up->abort();
            return false;
        }
        offset += take;
        if (progress) progress(up->bytesSent());
    }
    if (cancelled && cancelled()) {
        up->abort();
        error = "Đã huỷ theo yêu cầu";
        return false;
    }
    return up->finish(out, error);
}

// ---------------------------------------------------------------------------
//  Tải xuống
// ---------------------------------------------------------------------------
bool TgAccount::downloadRange(const ChunkLocation& loc, uint64_t offset, uint32_t limit,
                              Bytes& out, std::string& error) {
    if (!loc.valid()) {
        error = "Thông tin mảnh dữ liệu không hợp lệ";
        return false;
    }
    // Telegram yêu cầu offset chia hết cho 4096 và limit chia hết cho 4096,
    // đồng thời một lần đọc không vượt quá 1 MB và không vắt qua ranh giới 1 MB.
    if (offset % 4096 != 0) {
        error = "offset phải chia hết cho 4096";
        return false;
    }
    if (limit == 0 || limit > kDownloadBlock) limit = kDownloadBlock;
    if (limit % 4096 != 0) limit = ((limit + 4095) / 4096) * 4096;
    uint64_t blockStart = offset / kDownloadBlock;
    uint64_t blockEnd = (offset + limit - 1) / kDownloadBlock;
    if (blockStart != blockEnd) limit = static_cast<uint32_t>((blockStart + 1) * kDownloadBlock - offset);

    TlValue location = TlValue::makeObject("inputDocumentFileLocation");
    location.setLong("id", loc.documentId);
    location.setLong("access_hash", loc.accessHash);
    location.setBytes("file_reference", loc.fileReference);
    location.setBytes("thumb_size", std::string());

    TlValue req = TlValue::makeObject("upload.getFile");
    req.setFlag("precise");
    req.set("location", location);
    req.setLong("offset", static_cast<int64_t>(offset));
    req.setInt("limit", static_cast<int32_t>(limit));

    int dc = loc.dcId > 0 ? loc.dcId : config_.homeDc;
    InvokeResult res = invoke(req, dc);
    if (!res.ok) {
        if (!res.error.empty()) {
            error = "Tải dữ liệu thất bại: " + res.error.toString();
            if (res.error.message == "FILE_REFERENCE_EXPIRED" ||
                startsWith(res.error.message, "FILE_REFERENCE_"))
                error += " (tham chiếu tệp đã hết hạn — cần làm mới)";
        } else {
            error = res.localError;
        }
        return false;
    }
    if (!res.value.is("upload.file")) {
        if (res.value.is("upload.fileCdnRedirect")) {
            error = "Telegram chuyển hướng sang CDN — chưa hỗ trợ (hãy dùng tài khoản khác)";
        } else {
            error = "Phản hồi tải xuống không mong đợi: " + res.value.ctorName();
        }
        return false;
    }
    out = res.value["bytes"].asBytes();
    bytesDownloaded_.fetch_add(out.size());
    return true;
}

bool TgAccount::deleteMessages(const SupergroupRef& group,
                               const std::vector<int64_t>& messageIds, std::string& error) {
    if (messageIds.empty()) return true;
    // Xoá theo lô 100 thông điệp mỗi lần.
    for (size_t i = 0; i < messageIds.size(); i += 100) {
        TlVector ids;
        for (size_t j = i; j < messageIds.size() && j < i + 100; ++j)
            ids.push_back(TlValue::makeInt(static_cast<int32_t>(messageIds[j])));

        TlValue req = TlValue::makeObject("channels.deleteMessages");
        req.set("channel", makeInputChannel(group));
        req.setVector("id", std::move(ids));

        InvokeResult res = invoke(req);
        if (!res.ok && !res.partial) {
            error = res.describe();
            return false;
        }
    }
    return true;
}

bool TgAccount::refreshFileReference(const SupergroupRef& group, ChunkLocation& loc,
                                     std::string& error) {
    TlValue msgId = TlValue::makeObject("inputMessageID");
    msgId.setInt("id", static_cast<int32_t>(loc.messageId));
    TlVector ids;
    ids.push_back(msgId);

    TlValue req = TlValue::makeObject("channels.getMessages");
    req.set("channel", makeInputChannel(group));
    req.setVector("id", std::move(ids));

    InvokeResult res = invoke(req);
    if (!res.ok && !res.partial) {
        error = res.describe();
        return false;
    }
    const TlValue* doc = findFirstObject(res.value, "document");
    if (!doc) {
        error = "Không tìm thấy tài liệu trong thông điệp " + std::to_string(loc.messageId) +
                " (có thể đã bị xoá khỏi siêu nhóm)";
        return false;
    }
    loc.documentId = (*doc)["id"].asLong();
    loc.accessHash = (*doc)["access_hash"].asLong();
    loc.fileReference = (*doc)["file_reference"].asBytes();
    loc.dcId = (*doc)["dc_id"].asInt();
    uint64_t size = static_cast<uint64_t>((*doc)["size"].asLong());
    if (size) loc.size = size;
    LOG_DEBUG(kTag, "[%s] Đã làm mới tham chiếu cho thông điệp %lld", config_.label.c_str(),
              static_cast<long long>(loc.messageId));
    return true;
}

}  // namespace tg
}  // namespace ttd
