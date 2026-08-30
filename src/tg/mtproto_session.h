// Một phiên MTProto tới một trung tâm dữ liệu: tạo khoá xác thực (Diffie-Hellman),
// gửi yêu cầu, nhận trả lời, xử lý lỗi giao thức và tự phục hồi kết nối.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tg/dc_config.h"
#include "tg/mtproto_crypto.h"
#include "tg/mtproto_transport.h"
#include "tg/tl_codec.h"
#include "tg/tl_schema.h"
#include "tg/tl_value.h"

namespace ttd {
namespace tg {

struct RpcError {
    int code = 0;
    std::string message;
    // Với lỗi FLOOD_WAIT_x / *_MIGRATE_x, đây là giá trị x.
    int value = 0;
    bool empty() const { return code == 0 && message.empty(); }
    std::string toString() const;
};

struct InvokeResult {
    bool ok = false;
    TlValue value;
    RpcError error;         // lỗi do máy chủ trả về
    std::string localError; // lỗi phía ứng dụng (mạng, giải mã…)
    bool partial = false;   // giải mã thành công một phần
    std::string describe() const;
};

// Thông tin nhận dạng ứng dụng gửi trong initConnection.
struct AppInfo {
    int32_t apiId = 0;
    std::string apiHash;
    std::string deviceModel = "Tuan Telegram Disk";
    std::string systemVersion = "1.0";
    std::string appVersion = "1.0";
    std::string langCode = "vi";
    std::string systemLangCode = "vi";
    int layer = 158;
};

struct SessionOptions {
    int dcId = 2;
    bool testMode = false;
    bool obfuscated = false;
    int connectTimeoutMs = 15000;
    int requestTimeoutMs = 60000;
    int pingIntervalMs = 30000;
    std::string label;  // dùng trong nhật ký
};

class MtprotoSession {
public:
    MtprotoSession(const TlSchema& schema, AppInfo appInfo, SessionOptions options);
    ~MtprotoSession();

    MtprotoSession(const MtprotoSession&) = delete;
    MtprotoSession& operator=(const MtprotoSession&) = delete;

    // Đặt khoá xác thực đã có (từ phiên lưu trước). Nếu chưa có sẽ tự tạo khi kết nối.
    void setAuthKey(const AuthKey& key);
    AuthKey authKey() const;
    bool hasAuthKey() const;

    // Kết nối (và tạo khoá nếu cần). An toàn khi gọi nhiều lần.
    bool ensureConnected(std::string& error);
    void disconnect();
    bool connected() const { return connected_.load(); }

    // Gửi một yêu cầu và chờ kết quả.
    //   wrapInit = true cho yêu cầu đầu tiên của phiên (bọc invokeWithLayer/initConnection).
    InvokeResult invoke(const TlValue& request, int timeoutMs = 0);

    // Đánh dấu phiên đã được uỷ quyền (đăng nhập xong) để không bọc initConnection nữa.
    void markAuthorized(bool v) { authorized_.store(v); }
    bool authorized() const { return authorized_.load(); }

    int dcId() const { return options_.dcId; }
    const SessionOptions& options() const { return options_; }
    void setDcId(int dc) { options_.dcId = dc; }
    int64_t timeOffset() const { return timeOffset_.load(); }

    // Số yêu cầu đang chờ trả lời (dùng để cân bằng tải).
    size_t inFlight() const;
    // Thống kê.
    uint64_t bytesSent() const { return bytesSent_.load(); }
    uint64_t bytesReceived() const { return bytesReceived_.load(); }

private:
    struct Pending {
        int64_t msgId = 0;
        TlValue request;
        bool done = false;
        InvokeResult result;
        std::condition_variable cv;
        bool needsResend = false;
    };

    bool createAuthKey(std::string& error);
    bool sendPlain(const Bytes& payload, std::string& error);
    bool recvPlain(Bytes& out, int timeoutMs, std::string& error);

    bool sendEncrypted(int64_t msgId, int32_t seqNo, const Bytes& payload, std::string& error);
    void readerLoop();
    void handleIncoming(const DecryptedMessage& msg);
    void processBody(int64_t msgId, const Bytes& body, int depth);
    void completePending(int64_t reqMsgId, InvokeResult result);
    void failAllPending(const std::string& error);
    int32_t nextSeqNo(bool contentRelated);
    void queueAck(int64_t msgId);
    void flushAcks();

    const TlSchema& schema_;
    TlCodec codec_;
    AppInfo appInfo_;
    SessionOptions options_;

    mutable std::mutex mu_;
    MtprotoTransport transport_;
    AuthKey authKey_;
    int64_t sessionId_ = 0;
    int32_t seqNo_ = 0;
    MsgIdGenerator msgIdGen_;
    std::atomic<int64_t> timeOffset_{0};
    std::atomic<bool> connected_{false};
    std::atomic<bool> authorized_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> initSent_{false};
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint64_t> bytesReceived_{0};

    std::map<int64_t, std::shared_ptr<Pending>> pending_;
    std::vector<int64_t> pendingAcks_;

    std::thread reader_;
    std::mutex connectMu_;
    int64_t lastPingMs_ = 0;
    int64_t lastActivityMs_ = 0;
};

}  // namespace tg
}  // namespace ttd
