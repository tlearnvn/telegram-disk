// Máy chủ HTTP/1.1 tự cài đặt: nhiều luồng, giữ kết nối, đọc thân yêu cầu theo
// luồng (cho tải lên tệp lớn) và ghi phản hồi theo luồng (cho tải xuống, SSE).
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "common/net.h"
#include "http/http_types.h"

namespace ttd {
namespace http {

// Cho phép trình xử lý đọc thân yêu cầu theo từng khối.
class BodyReader {
public:
    virtual ~BodyReader() = default;
    // Đọc tối đa `len` byte. Trả về số byte đọc được, 0 = hết, -1 = lỗi.
    virtual long read(uint8_t* buf, size_t len) = 0;
    virtual uint64_t declaredLength() const = 0;
    virtual bool complete() const = 0;
    // Đọc toàn bộ phần còn lại vào chuỗi (có giới hạn an toàn).
    bool readAll(std::string& out, uint64_t maxBytes);
};

// Cho phép trình xử lý ghi phản hồi theo luồng.
using ResponseWriter = std::function<bool(const char*, size_t)>;

using Handler = std::function<void(Request&, BodyReader&, Response&)>;

struct Route {
    std::string method;   // rỗng = mọi phương thức
    std::string prefix;   // khớp theo tiền tố
    bool exact = false;   // khớp chính xác
    Handler handler;
};

struct ServerOptions {
    std::string bindAddress = "0.0.0.0";
    uint16_t port = 8088;
    int workerThreads = 16;
    int backlog = 128;
    int idleTimeoutSeconds = 120;
    uint64_t maxHeaderBytes = 64 * 1024;
    uint64_t maxInlineBodyBytes = 64ull * 1024 * 1024;
    bool logRequests = true;
};

class HttpServer {
public:
    explicit HttpServer(ServerOptions options);
    ~HttpServer();

    // Đăng ký tuyến. Tuyến đăng ký trước được ưu tiên.
    void route(const std::string& method, const std::string& path, Handler handler);
    void routePrefix(const std::string& method, const std::string& prefix, Handler handler);
    void setFallback(Handler handler) { fallback_ = std::move(handler); }
    // Bộ lọc chạy trước mọi tuyến; trả về false để dừng (đã ghi phản hồi).
    void setPreFilter(std::function<bool(Request&, Response&)> filter) {
        preFilter_ = std::move(filter);
    }

    bool start(std::string& error);
    void stop();
    bool running() const { return running_.load(); }
    uint16_t port() const { return options_.port; }

    uint64_t requestsHandled() const { return requestsHandled_.load(); }
    uint64_t bytesSent() const { return bytesSent_.load(); }
    uint64_t bytesReceived() const { return bytesReceived_.load(); }
    int activeConnections() const { return activeConnections_.load(); }

private:
    void acceptLoop();
    void workerLoop();
    void handleConnection(net::TcpSocket socket, const std::string& peerIp);
    const Route* matchRoute(const Request& req) const;

    ServerOptions options_;
    net::TcpListener listener_;
    std::vector<Route> routes_;
    Handler fallback_;
    std::function<bool(Request&, Response&)> preFilter_;

    std::atomic<bool> running_{false};
    std::thread acceptThread_;
    std::vector<std::thread> workers_;

    std::mutex queueMu_;
    std::condition_variable queueCv_;
    std::queue<std::pair<net::TcpSocket, std::string>> pending_;

    std::atomic<uint64_t> requestsHandled_{0};
    std::atomic<uint64_t> bytesSent_{0};
    std::atomic<uint64_t> bytesReceived_{0};
    std::atomic<int> activeConnections_{0};
};

}  // namespace http
}  // namespace ttd
