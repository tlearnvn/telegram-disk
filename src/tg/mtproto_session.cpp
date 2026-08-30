#include "tg/mtproto_session.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "common/logging.h"
#include "common/timeutil.h"
#include "compress/inflate.h"
#include "crypto/bigint.h"
#include "crypto/hash.h"
#include "crypto/random.h"

namespace ttd {
namespace tg {

namespace {
constexpr const char* kTag = "tg.mtp";

int64_t readInt64Le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return static_cast<int64_t>(v);
}

// Tách hậu tố số trong các lỗi kiểu FLOOD_WAIT_42, FILE_MIGRATE_4.
int trailingNumber(const std::string& s) {
    size_t i = s.size();
    while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9') --i;
    if (i == s.size()) return 0;
    return std::atoi(s.c_str() + i);
}

}  // namespace

std::string RpcError::toString() const {
    if (empty()) return "";
    return std::to_string(code) + " " + message;
}

std::string InvokeResult::describe() const {
    if (ok) return "OK: " + value.describe(2);
    if (!error.empty()) return "Lỗi máy chủ: " + error.toString();
    return "Lỗi cục bộ: " + localError;
}

MtprotoSession::MtprotoSession(const TlSchema& schema, AppInfo appInfo, SessionOptions options)
    : schema_(schema), codec_(schema), appInfo_(std::move(appInfo)), options_(std::move(options)) {
    sessionId_ = crypto::randomInt64();
}

MtprotoSession::~MtprotoSession() {
    stopping_.store(true);
    disconnect();
    if (reader_.joinable()) reader_.join();
}

void MtprotoSession::setAuthKey(const AuthKey& key) {
    std::lock_guard<std::mutex> lk(mu_);
    authKey_ = key;
    authKey_.computeKeyId();
}

AuthKey MtprotoSession::authKey() const {
    std::lock_guard<std::mutex> lk(mu_);
    return authKey_;
}

bool MtprotoSession::hasAuthKey() const {
    std::lock_guard<std::mutex> lk(mu_);
    return authKey_.valid();
}

size_t MtprotoSession::inFlight() const {
    std::lock_guard<std::mutex> lk(mu_);
    return pending_.size();
}

int32_t MtprotoSession::nextSeqNo(bool contentRelated) {
    // Giả định caller đã giữ mu_.
    int32_t value = seqNo_ * 2 + (contentRelated ? 1 : 0);
    if (contentRelated) ++seqNo_;
    return value;
}

// ---------------------------------------------------------------------------
//  Gói tin dạng rõ (chỉ dùng khi tạo khoá xác thực)
// ---------------------------------------------------------------------------
bool MtprotoSession::sendPlain(const Bytes& payload, std::string& error) {
    Bytes packet;
    packet.reserve(20 + payload.size());
    // auth_key_id = 0
    for (int i = 0; i < 8; ++i) packet.push_back(0);
    int64_t msgId = msgIdGen_.next(timeOffset_.load());
    uint64_t m = static_cast<uint64_t>(msgId);
    for (int i = 0; i < 8; ++i) packet.push_back(static_cast<uint8_t>((m >> (8 * i)) & 0xff));
    uint32_t len = static_cast<uint32_t>(payload.size());
    for (int i = 0; i < 4; ++i) packet.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xff));
    packet.insert(packet.end(), payload.begin(), payload.end());
    bytesSent_.fetch_add(packet.size());
    return transport_.sendPacket(packet, options_.connectTimeoutMs, error);
}

bool MtprotoSession::recvPlain(Bytes& out, int timeoutMs, std::string& error) {
    Bytes packet;
    if (!transport_.recvPacket(packet, timeoutMs, error)) return false;
    bytesReceived_.fetch_add(packet.size());
    if (packet.size() < 20) {
        error = "Gói tin dạng rõ quá ngắn";
        return false;
    }
    int64_t keyId = readInt64Le(packet.data());
    if (keyId != 0) {
        error = "Chờ gói tin dạng rõ nhưng nhận gói đã mã hoá";
        return false;
    }
    uint32_t len = 0;
    for (int i = 0; i < 4; ++i) len |= static_cast<uint32_t>(packet[16 + static_cast<size_t>(i)])
                                      << (8 * i);
    if (len > packet.size() - 20) {
        error = "Độ dài phần thân không hợp lệ";
        return false;
    }
    out.assign(packet.begin() + 20, packet.begin() + 20 + static_cast<long>(len));
    return true;
}

// ---------------------------------------------------------------------------
//  Tạo khoá xác thực (Diffie-Hellman theo MTProto 2.0)
// ---------------------------------------------------------------------------
bool MtprotoSession::createAuthKey(std::string& error) {
    LOG_INFO(kTag, "[%s] Bắt đầu tạo khoá xác thực với DC%d", options_.label.c_str(),
             options_.dcId);

    Bytes nonce = crypto::randomBytes(16);

    // --- Bước 1: req_pq_multi ---
    TlValue reqPq = TlValue::makeObject("req_pq_multi");
    reqPq.setBytes("nonce", nonce);
    TlWriter w1;
    if (!codec_.serialize(reqPq, w1, error)) return false;
    if (!sendPlain(w1.buffer(), error)) return false;

    Bytes resp;
    if (!recvPlain(resp, options_.connectTimeoutMs, error)) return false;
    TlReader r1(resp);
    TlValue resPq;
    if (!codec_.deserialize(r1, resPq, error)) {
        error = "Không đọc được resPQ: " + error;
        return false;
    }
    if (!resPq.is("resPQ")) {
        error = "Chờ resPQ nhưng nhận " + resPq.ctorName();
        return false;
    }
    Bytes serverNonce = resPq["server_nonce"].asBytes();
    const Bytes& pqBytes = resPq["pq"].asBytes();
    if (pqBytes.empty() || pqBytes.size() > 8) {
        error = "Giá trị pq không hợp lệ";
        return false;
    }
    uint64_t pq = 0;
    for (uint8_t b : pqBytes) pq = (pq << 8) | b;

    uint64_t p = 0, q = 0;
    if (!factorizePq(pq, p, q)) {
        error = "Không phân tích được pq = " + std::to_string(pq);
        return false;
    }
    LOG_DEBUG(kTag, "[%s] pq=%llu -> p=%llu q=%llu", options_.label.c_str(),
              static_cast<unsigned long long>(pq), static_cast<unsigned long long>(p),
              static_cast<unsigned long long>(q));

    std::vector<int64_t> fingerprints;
    for (const auto& fp : resPq["server_public_key_fingerprints"].asVector())
        fingerprints.push_back(fp.asLong());
    int64_t chosenFp = 0;
    const crypto::RsaPublicKey* key = DcConfig::instance().selectKey(fingerprints, chosenFp);
    if (!key) {
        error =
            "Máy chủ yêu cầu khoá công khai mà ứng dụng chưa có. Hãy bổ sung khoá mới qua cấu "
            "hình telegram.rsa_public_keys.";
        return false;
    }

    // --- Bước 2: req_DH_params ---
    Bytes newNonce = crypto::randomBytes(32);
    auto toBigEndianBytes = [](uint64_t v) {
        Bytes b;
        bool started = false;
        for (int i = 7; i >= 0; --i) {
            uint8_t byte = static_cast<uint8_t>((v >> (8 * i)) & 0xff);
            if (byte || started) {
                b.push_back(byte);
                started = true;
            }
        }
        if (b.empty()) b.push_back(0);
        return b;
    };

    TlValue inner = TlValue::makeObject("p_q_inner_data_dc");
    inner.setBytes("pq", pqBytes);
    inner.setBytes("p", toBigEndianBytes(p));
    inner.setBytes("q", toBigEndianBytes(q));
    inner.setBytes("nonce", nonce);
    inner.setBytes("server_nonce", serverNonce);
    inner.setBytes("new_nonce", newNonce);
    inner.setInt("dc", options_.testMode ? (10000 + options_.dcId) : options_.dcId);

    TlWriter wInner;
    if (!codec_.serialize(inner, wInner, error)) return false;

    Bytes encryptedData;
    if (!rsaPadEncrypt(wInner.buffer(), *key, encryptedData)) {
        error = "Mã hoá RSA cho p_q_inner_data thất bại";
        return false;
    }

    TlValue reqDh = TlValue::makeObject("req_DH_params");
    reqDh.setBytes("nonce", nonce);
    reqDh.setBytes("server_nonce", serverNonce);
    reqDh.setBytes("p", toBigEndianBytes(p));
    reqDh.setBytes("q", toBigEndianBytes(q));
    reqDh.setLong("public_key_fingerprint", chosenFp);
    reqDh.setBytes("encrypted_data", encryptedData);

    TlWriter w2;
    if (!codec_.serialize(reqDh, w2, error)) return false;
    if (!sendPlain(w2.buffer(), error)) return false;
    if (!recvPlain(resp, options_.connectTimeoutMs, error)) return false;

    TlReader r2(resp);
    TlValue dhParams;
    if (!codec_.deserialize(r2, dhParams, error)) {
        error = "Không đọc được Server_DH_Params: " + error;
        return false;
    }
    if (dhParams.is("server_DH_params_fail")) {
        error = "Máy chủ từ chối tham số DH (server_DH_params_fail)";
        return false;
    }
    if (!dhParams.is("server_DH_params_ok")) {
        error = "Phản hồi DH không mong đợi: " + dhParams.ctorName();
        return false;
    }

    Bytes aesKey, aesIv;
    tmpAesKeyIv(newNonce, serverNonce, aesKey, aesIv);
    Bytes answerPlain;
    if (!crypto::aesIgeDecrypt(dhParams["encrypted_answer"].asBytes(), aesKey, aesIv,
                               answerPlain)) {
        error = "Giải mã encrypted_answer thất bại";
        return false;
    }
    if (answerPlain.size() < 20) {
        error = "encrypted_answer quá ngắn";
        return false;
    }
    // 20 byte đầu là SHA1 của phần dữ liệu.
    Bytes answerHash(answerPlain.begin(), answerPlain.begin() + 20);
    Bytes answerBody(answerPlain.begin() + 20, answerPlain.end());

    TlReader r3(answerBody);
    TlValue dhInner;
    if (!codec_.deserialize(r3, dhInner, error)) {
        error = "Không đọc được server_DH_inner_data: " + error;
        return false;
    }
    if (!dhInner.is("server_DH_inner_data")) {
        error = "Chờ server_DH_inner_data nhưng nhận " + dhInner.ctorName();
        return false;
    }
    // Kiểm tra SHA1 trên đúng phần dữ liệu đã đọc.
    Bytes usedBody(answerBody.begin(), answerBody.begin() + static_cast<long>(r3.position()));
    if (!crypto::constantTimeEquals(crypto::Sha1::hash(usedBody), answerHash)) {
        error = "SHA1 của server_DH_inner_data không khớp";
        return false;
    }
    if (dhInner["nonce"].asBytes() != nonce ||
        dhInner["server_nonce"].asBytes() != serverNonce) {
        error = "nonce trong server_DH_inner_data không khớp";
        return false;
    }

    Bytes dhPrimeBytes = dhInner["dh_prime"].asBytes();
    Bytes gaBytes = dhInner["g_a"].asBytes();
    int32_t g = dhInner["g"].asInt();
    int32_t serverTime = dhInner["server_time"].asInt();
    timeOffset_.store(static_cast<int64_t>(serverTime) - nowUnix());

    if (dhPrimeBytes.size() != 256) {
        error = "dh_prime phải dài 2048 bit";
        return false;
    }
    crypto::BigInt dhPrime = crypto::BigInt::fromBytes(dhPrimeBytes);
    crypto::BigInt ga = crypto::BigInt::fromBytes(gaBytes);
    crypto::BigInt one(1);
    crypto::BigInt primeMinusOne = crypto::BigInt::sub(dhPrime, one);

    // Kiểm tra an toàn theo khuyến nghị của Telegram.
    if (crypto::BigInt::compare(ga, one) <= 0 ||
        crypto::BigInt::compare(ga, primeMinusOne) >= 0) {
        error = "g_a nằm ngoài khoảng cho phép";
        return false;
    }
    if (g < 2 || g > 7) {
        error = "Giá trị g không hợp lệ";
        return false;
    }
    if ((dhPrimeBytes[0] & 0x80) == 0) {
        error = "dh_prime không đủ 2048 bit";
        return false;
    }

    // --- Bước 3: set_client_DH_params ---
    crypto::BigInt gBig(static_cast<uint64_t>(g));
    Bytes authKeyBytes;
    Bytes gbBytes;
    for (int attempt = 0; attempt < 8; ++attempt) {
        crypto::BigInt b = crypto::BigInt::fromBytes(crypto::randomBytes(256));
        crypto::BigInt gb = crypto::BigInt::powMod(gBig, b, dhPrime);
        if (crypto::BigInt::compare(gb, one) <= 0 ||
            crypto::BigInt::compare(gb, primeMinusOne) >= 0)
            continue;
        crypto::BigInt shared = crypto::BigInt::powMod(ga, b, dhPrime);
        authKeyBytes = shared.toBytes(256);
        gbBytes = gb.toBytes(256);
        break;
    }
    if (authKeyBytes.size() != 256) {
        error = "Không tính được khoá chia sẻ";
        return false;
    }

    TlValue clientInner = TlValue::makeObject("client_DH_inner_data");
    clientInner.setBytes("nonce", nonce);
    clientInner.setBytes("server_nonce", serverNonce);
    clientInner.setLong("retry_id", 0);
    clientInner.setBytes("g_b", gbBytes);

    TlWriter wClient;
    if (!codec_.serialize(clientInner, wClient, error)) return false;
    Bytes toEncrypt = crypto::Sha1::hash(wClient.buffer());
    toEncrypt.insert(toEncrypt.end(), wClient.buffer().begin(), wClient.buffer().end());
    while (toEncrypt.size() % 16 != 0) {
        Bytes pad = crypto::randomBytes(1);
        toEncrypt.push_back(pad[0]);
    }
    Bytes clientEncrypted;
    if (!crypto::aesIgeEncrypt(toEncrypt, aesKey, aesIv, clientEncrypted)) {
        error = "Mã hoá client_DH_inner_data thất bại";
        return false;
    }

    TlValue setParams = TlValue::makeObject("set_client_DH_params");
    setParams.setBytes("nonce", nonce);
    setParams.setBytes("server_nonce", serverNonce);
    setParams.setBytes("encrypted_data", clientEncrypted);

    TlWriter w4;
    if (!codec_.serialize(setParams, w4, error)) return false;
    if (!sendPlain(w4.buffer(), error)) return false;
    if (!recvPlain(resp, options_.connectTimeoutMs, error)) return false;

    TlReader r4(resp);
    TlValue answer;
    if (!codec_.deserialize(r4, answer, error)) {
        error = "Không đọc được kết quả DH: " + error;
        return false;
    }

    AuthKey newKey;
    newKey.key = authKeyBytes;
    newKey.computeKeyId();
    Bytes aux = newKey.auxHash();

    if (answer.is("dh_gen_ok")) {
        Bytes expect = newNonceHash(newNonce, 1, aux);
        if (answer["new_nonce_hash1"].asBytes() != expect) {
            error = "new_nonce_hash1 không khớp";
            return false;
        }
        // server_salt = new_nonce[0:8] XOR server_nonce[0:8]
        int64_t salt = 0;
        for (int i = 7; i >= 0; --i) {
            uint8_t v = static_cast<uint8_t>(newNonce[static_cast<size_t>(i)] ^
                                             serverNonce[static_cast<size_t>(i)]);
            salt = (salt << 8) | v;
        }
        newKey.serverSalt = salt;
        {
            std::lock_guard<std::mutex> lk(mu_);
            authKey_ = newKey;
            seqNo_ = 0;
            sessionId_ = crypto::randomInt64();
        }
        LOG_INFO(kTag, "[%s] Đã tạo khoá xác thực cho DC%d (key_id=0x%016llx)",
                 options_.label.c_str(), options_.dcId,
                 static_cast<unsigned long long>(newKey.keyId));
        return true;
    }
    if (answer.is("dh_gen_retry")) {
        error = "Máy chủ yêu cầu thử lại quá trình tạo khoá (dh_gen_retry)";
        return false;
    }
    error = "Tạo khoá thất bại: " + answer.ctorName();
    return false;
}

// ---------------------------------------------------------------------------
//  Kết nối
// ---------------------------------------------------------------------------
bool MtprotoSession::ensureConnected(std::string& error) {
    std::lock_guard<std::mutex> lk(connectMu_);
    if (connected_.load() && transport_.connected()) return true;
    if (stopping_.load()) {
        error = "Phiên đang dừng";
        return false;
    }

    // Dọn luồng đọc cũ nếu có.
    connected_.store(false);
    if (reader_.joinable()) reader_.join();
    {
        std::lock_guard<std::mutex> lk2(mu_);
        transport_.close();
    }

    DcEndpoint ep;
    if (!DcConfig::instance().endpointFor(options_.dcId, options_.testMode, ep)) {
        error = "Không có địa chỉ cho DC" + std::to_string(options_.dcId);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk2(mu_);
        if (!transport_.connect(ep.ip, ep.port, options_.dcId, options_.obfuscated,
                                options_.connectTimeoutMs, error))
            return false;
    }

    bool needKey;
    {
        std::lock_guard<std::mutex> lk2(mu_);
        needKey = !authKey_.valid();
        // Mỗi lần kết nối lại dùng session_id mới để tránh lặp msg_id.
        sessionId_ = crypto::randomInt64();
        seqNo_ = 0;
        initSent_.store(false);
    }
    if (needKey) {
        if (!createAuthKey(error)) {
            std::lock_guard<std::mutex> lk2(mu_);
            transport_.close();
            return false;
        }
    }

    connected_.store(true);
    lastActivityMs_ = monotonicMillis();
    lastPingMs_ = lastActivityMs_;
    reader_ = std::thread([this]() { readerLoop(); });
    LOG_INFO(kTag, "[%s] Sẵn sàng trên DC%d qua %s", options_.label.c_str(), options_.dcId,
             ep.ip.c_str());
    return true;
}

void MtprotoSession::disconnect() {
    connected_.store(false);
    {
        std::lock_guard<std::mutex> lk(mu_);
        transport_.close();
    }
    failAllPending("Kết nối đã đóng");
    if (reader_.joinable() && std::this_thread::get_id() != reader_.get_id()) reader_.join();
}

// ---------------------------------------------------------------------------
//  Gửi / nhận gói tin đã mã hoá
// ---------------------------------------------------------------------------
bool MtprotoSession::sendEncrypted(int64_t msgId, int32_t seqNo, const Bytes& payload,
                                   std::string& error) {
    Bytes packet;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!authKey_.valid()) {
            error = "Chưa có khoá xác thực";
            return false;
        }
        packet = encryptMessage(authKey_, sessionId_, msgId, seqNo, payload);
    }
    if (packet.empty()) {
        error = "Mã hoá gói tin thất bại";
        return false;
    }
    bytesSent_.fetch_add(packet.size());
    std::lock_guard<std::mutex> lk(mu_);
    return transport_.sendPacket(packet, options_.requestTimeoutMs, error);
}

void MtprotoSession::queueAck(int64_t msgId) {
    std::lock_guard<std::mutex> lk(mu_);
    pendingAcks_.push_back(msgId);
}

void MtprotoSession::flushAcks() {
    std::vector<int64_t> acks;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pendingAcks_.empty()) return;
        acks.swap(pendingAcks_);
    }
    TlValue ack = TlValue::makeObject("msgs_ack");
    TlVector ids;
    for (int64_t id : acks) ids.push_back(TlValue::makeLong(id));
    ack.setVector("msg_ids", std::move(ids));

    TlWriter w;
    std::string err;
    if (!codec_.serialize(ack, w, err)) return;

    int64_t msgId;
    int32_t seq;
    {
        std::lock_guard<std::mutex> lk(mu_);
        msgId = msgIdGen_.next(timeOffset_.load());
        seq = nextSeqNo(false);
    }
    sendEncrypted(msgId, seq, w.buffer(), err);
}

// ---------------------------------------------------------------------------
//  Vòng lặp đọc
// ---------------------------------------------------------------------------
void MtprotoSession::readerLoop() {
    LOG_DEBUG(kTag, "[%s] Luồng đọc bắt đầu", options_.label.c_str());
    while (connected_.load() && !stopping_.load()) {
        Bytes packet;
        std::string error;
        bool got;
        {
            // recvPacket tự chờ có dữ liệu nên không giữ khoá lâu.
            got = transport_.recvPacket(packet, 1000, error);
        }
        if (!got) {
            if (!connected_.load() || stopping_.load()) break;
            // Hết giờ đọc là bình thường — nhân lúc rảnh thì gửi ack và ping.
            if (error.find("Không đọc được độ dài") != std::string::npos ||
                error.find("Hết thời gian") != std::string::npos) {
                flushAcks();
                int64_t now = monotonicMillis();
                if (now - lastPingMs_ > options_.pingIntervalMs) {
                    lastPingMs_ = now;
                    TlValue ping = TlValue::makeObject("ping_delay_disconnect");
                    ping.setLong("ping_id", crypto::randomInt64());
                    ping.setInt("disconnect_delay", 75);
                    TlWriter w;
                    std::string err;
                    if (codec_.serialize(ping, w, err)) {
                        int64_t msgId;
                        int32_t seq;
                        {
                            std::lock_guard<std::mutex> lk(mu_);
                            msgId = msgIdGen_.next(timeOffset_.load());
                            seq = nextSeqNo(false);
                        }
                        sendEncrypted(msgId, seq, w.buffer(), err);
                    }
                }
                continue;
            }
            LOG_WARN(kTag, "[%s] Mất kết nối: %s", options_.label.c_str(), error.c_str());
            connected_.store(false);
            failAllPending("Mất kết nối tới máy chủ Telegram: " + error);
            break;
        }

        bytesReceived_.fetch_add(packet.size());
        lastActivityMs_ = monotonicMillis();

        AuthKey key;
        {
            std::lock_guard<std::mutex> lk(mu_);
            key = authKey_;
        }
        DecryptedMessage msg = decryptMessage(key, packet);
        if (!msg.ok) {
            LOG_WARN(kTag, "[%s] Bỏ qua gói tin không giải mã được: %s", options_.label.c_str(),
                     msg.error.c_str());
            continue;
        }
        handleIncoming(msg);
    }
    LOG_DEBUG(kTag, "[%s] Luồng đọc kết thúc", options_.label.c_str());
}

void MtprotoSession::handleIncoming(const DecryptedMessage& msg) {
    // Chỉ ack các thông điệp có nội dung (seq_no lẻ).
    if (msg.seqNo & 1) queueAck(msg.msgId);
    processBody(msg.msgId, msg.body, 0);
    flushAcks();
}

void MtprotoSession::processBody(int64_t msgId, const Bytes& body, int depth) {
    if (depth > 8 || body.size() < 4) return;
    uint32_t ctorId = static_cast<uint32_t>(body[0]) | (static_cast<uint32_t>(body[1]) << 8) |
                      (static_cast<uint32_t>(body[2]) << 16) |
                      (static_cast<uint32_t>(body[3]) << 24);

    // --- Bộ chứa nhiều thông điệp ---
    if (ctorId == ctor::kMsgContainer) {
        TlReader r(body);
        uint32_t id;
        uint32_t count;
        if (!r.readUInt(id) || !r.readUInt(count)) return;
        for (uint32_t i = 0; i < count; ++i) {
            int64_t innerMsgId;
            int32_t seqNo, len;
            if (!r.readLong(innerMsgId) || !r.readInt(seqNo) || !r.readInt(len)) return;
            if (len < 0 || static_cast<size_t>(len) > r.remaining()) return;
            Bytes inner;
            if (!r.readRaw(static_cast<size_t>(len), inner)) return;
            if (seqNo & 1) queueAck(innerMsgId);
            processBody(innerMsgId, inner, depth + 1);
        }
        return;
    }

    // --- Dữ liệu nén gzip ---
    if (ctorId == ctor::kGzipPacked) {
        TlReader r(body);
        uint32_t id;
        Bytes packed;
        if (!r.readUInt(id) || !r.readBytes(packed)) return;
        Bytes unpacked;
        if (!compress::inflateAuto(packed, unpacked, 256u * 1024 * 1024)) {
            LOG_WARN(kTag, "[%s] Giải nén gzip_packed thất bại", options_.label.c_str());
            return;
        }
        processBody(msgId, unpacked, depth + 1);
        return;
    }

    // --- Kết quả của một yêu cầu ---
    if (ctorId == ctor::kRpcResult) {
        TlReader r(body);
        uint32_t id;
        int64_t reqMsgId;
        if (!r.readUInt(id) || !r.readLong(reqMsgId)) return;

        Bytes rest(body.begin() + static_cast<long>(r.position()), body.end());
        if (rest.size() >= 4) {
            uint32_t innerId = static_cast<uint32_t>(rest[0]) |
                               (static_cast<uint32_t>(rest[1]) << 8) |
                               (static_cast<uint32_t>(rest[2]) << 16) |
                               (static_cast<uint32_t>(rest[3]) << 24);
            if (innerId == ctor::kGzipPacked) {
                TlReader rg(rest);
                uint32_t gid;
                Bytes packed;
                if (rg.readUInt(gid) && rg.readBytes(packed)) {
                    Bytes unpacked;
                    if (compress::inflateAuto(packed, unpacked, 256u * 1024 * 1024)) {
                        rest = std::move(unpacked);
                    }
                }
            }
        }

        InvokeResult result;
        TlReader rr(rest);
        TlValue value;
        std::string err;
        if (codec_.deserialize(rr, value, err)) {
            if (value.is("rpc_error")) {
                result.ok = false;
                result.error.code = value["error_code"].asInt();
                result.error.message = value["error_message"].asString();
                result.error.value = trailingNumber(result.error.message);
            } else {
                result.ok = true;
                result.value = std::move(value);
            }
        } else {
            // Giải mã một phần vẫn có ích: các trường đọc được nằm trong value.
            result.ok = false;
            result.partial = true;
            result.value = std::move(value);
            result.localError = err;
            LOG_WARN(kTag, "[%s] Giải mã kết quả chưa trọn vẹn: %s", options_.label.c_str(),
                     err.c_str());
        }
        completePending(reqMsgId, std::move(result));
        return;
    }

    // --- Thông điệp dịch vụ ---
    TlReader r(body);
    TlValue obj;
    std::string err;
    if (!codec_.deserialize(r, obj, err)) {
        LOG_DEBUG(kTag, "[%s] Bỏ qua thông điệp không hiểu: %s", options_.label.c_str(),
                  err.c_str());
        return;
    }

    if (obj.is("new_session_created")) {
        std::lock_guard<std::mutex> lk(mu_);
        authKey_.serverSalt = obj["server_salt"].asLong();
        LOG_DEBUG(kTag, "[%s] Máy chủ tạo phiên mới, cập nhật server_salt",
                  options_.label.c_str());
        return;
    }
    if (obj.is("bad_server_salt")) {
        int64_t newSalt = obj["new_server_salt"].asLong();
        int64_t badMsgId = obj["bad_msg_id"].asLong();
        {
            std::lock_guard<std::mutex> lk(mu_);
            authKey_.serverSalt = newSalt;
        }
        LOG_DEBUG(kTag, "[%s] bad_server_salt — đã cập nhật muối, gửi lại yêu cầu",
                  options_.label.c_str());
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = pending_.find(badMsgId);
            if (it != pending_.end()) pending = it->second;
        }
        if (pending) {
            pending->needsResend = true;
            std::lock_guard<std::mutex> lk(mu_);
            pending->cv.notify_all();
        }
        return;
    }
    if (obj.is("bad_msg_notification")) {
        int errorCode = obj["error_code"].asInt();
        int64_t badMsgId = obj["bad_msg_id"].asLong();
        LOG_WARN(kTag, "[%s] bad_msg_notification mã %d", options_.label.c_str(), errorCode);
        if (errorCode == 16 || errorCode == 17) {
            // Lệch đồng hồ: dùng msg_id của máy chủ để hiệu chỉnh.
            int64_t serverTime = msgId >> 32;
            timeOffset_.store(serverTime - nowUnix());
            LOG_INFO(kTag, "[%s] Hiệu chỉnh lệch giờ: %lld giây", options_.label.c_str(),
                     static_cast<long long>(timeOffset_.load()));
            std::shared_ptr<Pending> pending;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto it = pending_.find(badMsgId);
                if (it != pending_.end()) pending = it->second;
            }
            if (pending) {
                pending->needsResend = true;
                std::lock_guard<std::mutex> lk(mu_);
                pending->cv.notify_all();
            }
            return;
        }
        InvokeResult res;
        res.ok = false;
        res.localError = "Lỗi giao thức bad_msg_notification mã " + std::to_string(errorCode);
        completePending(badMsgId, std::move(res));
        return;
    }
    if (obj.is("pong") || obj.is("msgs_ack") || obj.is("msg_detailed_info") ||
        obj.is("msg_new_detailed_info") || obj.is("msgs_state_info")) {
        return;
    }
    LOG_TRACE(kTag, "[%s] Thông điệp dịch vụ: %s", options_.label.c_str(),
              obj.describe(2).c_str());
}

void MtprotoSession::completePending(int64_t reqMsgId, InvokeResult result) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = pending_.find(reqMsgId);
    if (it == pending_.end()) return;
    it->second->result = std::move(result);
    it->second->done = true;
    it->second->cv.notify_all();
}

void MtprotoSession::failAllPending(const std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : pending_) {
        if (kv.second->done) continue;
        kv.second->result.ok = false;
        kv.second->result.localError = error;
        kv.second->done = true;
        kv.second->cv.notify_all();
    }
}

// ---------------------------------------------------------------------------
//  Gửi yêu cầu
// ---------------------------------------------------------------------------
InvokeResult MtprotoSession::invoke(const TlValue& request, int timeoutMs) {
    InvokeResult result;
    if (timeoutMs <= 0) timeoutMs = options_.requestTimeoutMs;

    std::string error;
    if (!ensureConnected(error)) {
        result.localError = error;
        return result;
    }

    // Yêu cầu đầu tiên trên mỗi kết nối phải bọc invokeWithLayer + initConnection.
    TlValue outer = request;
    bool needInit = !initSent_.load();
    if (needInit) {
        TlValue init = TlValue::makeObject("initConnection");
        init.setInt("api_id", appInfo_.apiId);
        init.setBytes("device_model", appInfo_.deviceModel);
        init.setBytes("system_version", appInfo_.systemVersion);
        init.setBytes("app_version", appInfo_.appVersion);
        init.setBytes("system_lang_code", appInfo_.systemLangCode);
        init.setBytes("lang_pack", std::string());
        init.setBytes("lang_code", appInfo_.langCode);
        init.set("query", request);

        TlValue wrapper = TlValue::makeObject("invokeWithLayer");
        wrapper.setInt("layer", appInfo_.layer);
        wrapper.set("query", init);
        outer = wrapper;
    }

    TlWriter writer;
    if (!codec_.serialize(outer, writer, error)) {
        result.localError = "Không tuần tự hoá được yêu cầu: " + error;
        return result;
    }

    auto pending = std::make_shared<Pending>();
    pending->request = outer;

    const int kMaxAttempts = 3;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        int64_t msgId;
        int32_t seq;
        {
            std::lock_guard<std::mutex> lk(mu_);
            msgId = msgIdGen_.next(timeOffset_.load());
            seq = nextSeqNo(true);
            pending->msgId = msgId;
            pending->done = false;
            pending->needsResend = false;
            pending->result = InvokeResult();
            pending_[msgId] = pending;
        }

        if (!sendEncrypted(msgId, seq, writer.buffer(), error)) {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.erase(msgId);
            result.localError = "Gửi yêu cầu thất bại: " + error;
            connected_.store(false);
            return result;
        }
        if (needInit) initSent_.store(true);

        LOG_TRACE(kTag, "[%s] Gửi %s (msg_id=%lld)", options_.label.c_str(),
                  request.ctorName().c_str(), static_cast<long long>(msgId));

        {
            std::unique_lock<std::mutex> lk(mu_);
            pending->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                                 [&]() { return pending->done || pending->needsResend; });
            pending_.erase(msgId);
            if (pending->done) {
                result = pending->result;
                break;
            }
            if (pending->needsResend) {
                LOG_DEBUG(kTag, "[%s] Gửi lại %s", options_.label.c_str(),
                          request.ctorName().c_str());
                continue;
            }
        }
        result.localError = "Hết thời gian chờ trả lời từ Telegram (" +
                            std::to_string(timeoutMs / 1000) + " giây)";
        break;
    }

    if (!result.ok && !result.error.empty()) {
        LOG_DEBUG(kTag, "[%s] %s -> lỗi %s", options_.label.c_str(),
                  request.ctorName().c_str(), result.error.toString().c_str());
    }
    return result;
}

}  // namespace tg
}  // namespace ttd
