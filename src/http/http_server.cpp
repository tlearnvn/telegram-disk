#include "http/http_server.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "common/logging.h"
#include "common/timeutil.h"

namespace ttd {
namespace http {

namespace {
constexpr const char* kTag = "http";
constexpr size_t kReadBufferSize = 64 * 1024;

// Bộ đệm đọc có khả năng "trả lại" dữ liệu thừa đã đọc trước.
class SocketBuffer {
public:
    SocketBuffer(net::TcpSocket& socket, int timeoutMs)
        : socket_(socket), timeoutMs_(timeoutMs) {}

    // Đọc thêm dữ liệu vào bộ đệm. Trả về false nếu kết nối đóng hoặc lỗi.
    bool fill() {
        if (pos_ > 0 && pos_ == size_) {
            pos_ = 0;
            size_ = 0;
        }
        if (size_ == buffer_.size()) {
            if (pos_ > 0) {
                std::memmove(buffer_.data(), buffer_.data() + pos_, size_ - pos_);
                size_ -= pos_;
                pos_ = 0;
            } else {
                return false;  // đầy mà vẫn chưa đủ
            }
        }
        long n = socket_.recvSome(buffer_.data() + size_, buffer_.size() - size_, timeoutMs_);
        if (n <= 0) return false;
        size_ += static_cast<size_t>(n);
        received_ += static_cast<uint64_t>(n);
        return true;
    }

    // Đọc một dòng kết thúc bằng CRLF (không gồm CRLF). Trả về false nếu lỗi.
    bool readLine(std::string& out, size_t maxLen) {
        out.clear();
        while (true) {
            for (size_t i = pos_; i < size_; ++i) {
                if (buffer_[i] == '\n') {
                    size_t end = i;
                    if (end > pos_ && buffer_[end - 1] == '\r') --end;
                    out.assign(reinterpret_cast<const char*>(buffer_.data() + pos_), end - pos_);
                    pos_ = i + 1;
                    return true;
                }
            }
            if (size_ - pos_ > maxLen) return false;
            if (!fill()) return false;
        }
    }

    // Lấy dữ liệu đang có sẵn trong bộ đệm.
    size_t available() const { return size_ - pos_; }
    const uint8_t* data() const { return buffer_.data() + pos_; }
    void consume(size_t n) { pos_ += std::min(n, size_ - pos_); }

    // Đọc trực tiếp từ socket khi bộ đệm rỗng.
    long readRaw(uint8_t* out, size_t len) {
        if (available() > 0) {
            size_t take = std::min(len, available());
            std::memcpy(out, data(), take);
            consume(take);
            return static_cast<long>(take);
        }
        long n = socket_.recvSome(out, len, timeoutMs_);
        if (n > 0) received_ += static_cast<uint64_t>(n);
        return n;
    }

    uint64_t bytesReceived() const { return received_; }
    void setTimeout(int ms) { timeoutMs_ = ms; }

private:
    net::TcpSocket& socket_;
    int timeoutMs_;
    std::vector<uint8_t> buffer_ = std::vector<uint8_t>(kReadBufferSize);
    size_t pos_ = 0;
    size_t size_ = 0;
    uint64_t received_ = 0;
};

// Đọc thân yêu cầu với độ dài xác định.
class LengthBodyReader : public BodyReader {
public:
    LengthBodyReader(SocketBuffer& buf, uint64_t length) : buf_(buf), remaining_(length),
                                                           total_(length) {}
    long read(uint8_t* out, size_t len) override {
        if (remaining_ == 0) return 0;
        size_t want = static_cast<size_t>(std::min<uint64_t>(len, remaining_));
        long n = buf_.readRaw(out, want);
        if (n > 0) remaining_ -= static_cast<uint64_t>(n);
        return n;
    }
    uint64_t declaredLength() const override { return total_; }
    bool complete() const override { return remaining_ == 0; }

private:
    SocketBuffer& buf_;
    uint64_t remaining_;
    uint64_t total_;
};

// Đọc thân yêu cầu dạng chunked.
class ChunkedBodyReader : public BodyReader {
public:
    explicit ChunkedBodyReader(SocketBuffer& buf) : buf_(buf) {}

    long read(uint8_t* out, size_t len) override {
        if (done_) return 0;
        if (chunkRemaining_ == 0) {
            std::string line;
            if (!buf_.readLine(line, 1024)) return -1;
            if (line.empty()) {
                if (!buf_.readLine(line, 1024)) return -1;
            }
            size_t semi = line.find(';');
            if (semi != std::string::npos) line = line.substr(0, semi);
            line = trim(line);
            uint64_t size = 0;
            for (char c : line) {
                int v;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
                else return -1;
                size = size * 16 + static_cast<uint64_t>(v);
            }
            if (size == 0) {
                // Đọc nốt phần trailer.
                std::string trailer;
                while (buf_.readLine(trailer, 4096) && !trailer.empty()) {
                }
                done_ = true;
                return 0;
            }
            chunkRemaining_ = size;
        }
        size_t want = static_cast<size_t>(std::min<uint64_t>(len, chunkRemaining_));
        long n = buf_.readRaw(out, want);
        if (n > 0) {
            chunkRemaining_ -= static_cast<uint64_t>(n);
            total_ += static_cast<uint64_t>(n);
            if (chunkRemaining_ == 0) {
                // Bỏ CRLF sau mỗi khối.
                std::string crlf;
                buf_.readLine(crlf, 8);
            }
        }
        return n;
    }
    uint64_t declaredLength() const override { return 0; }
    bool complete() const override { return done_; }

private:
    SocketBuffer& buf_;
    uint64_t chunkRemaining_ = 0;
    uint64_t total_ = 0;
    bool done_ = false;
};

// Không có thân yêu cầu.
class EmptyBodyReader : public BodyReader {
public:
    long read(uint8_t*, size_t) override { return 0; }
    uint64_t declaredLength() const override { return 0; }
    bool complete() const override { return true; }
};

}  // namespace

bool BodyReader::readAll(std::string& out, uint64_t maxBytes) {
    out.clear();
    uint8_t buf[32768];
    while (true) {
        long n = read(buf, sizeof(buf));
        if (n < 0) return false;
        if (n == 0) break;
        if (out.size() + static_cast<size_t>(n) > maxBytes) return false;
        out.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
    }
    return true;
}

HttpServer::HttpServer(ServerOptions options) : options_(std::move(options)) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(const std::string& method, const std::string& path, Handler handler) {
    Route r;
    r.method = method;
    r.prefix = path;
    r.exact = true;
    r.handler = std::move(handler);
    routes_.push_back(std::move(r));
}

void HttpServer::routePrefix(const std::string& method, const std::string& prefix,
                             Handler handler) {
    Route r;
    r.method = method;
    r.prefix = prefix;
    r.exact = false;
    r.handler = std::move(handler);
    routes_.push_back(std::move(r));
}

const Route* HttpServer::matchRoute(const Request& req) const {
    for (const auto& r : routes_) {
        if (!r.method.empty() && r.method != req.method) continue;
        if (r.exact) {
            if (req.path == r.prefix) return &r;
        } else {
            if (startsWith(req.path, r.prefix)) return &r;
        }
    }
    return nullptr;
}

bool HttpServer::start(std::string& error) {
    net::initNetworking();
    if (!listener_.listenOn(options_.bindAddress, options_.port, options_.backlog, error))
        return false;
    running_.store(true);
    for (int i = 0; i < options_.workerThreads; ++i)
        workers_.emplace_back([this]() { workerLoop(); });
    acceptThread_ = std::thread([this]() { acceptLoop(); });
    LOG_INFO(kTag, "Máy chủ web đang lắng nghe tại %s:%u (%d luồng xử lý)",
             options_.bindAddress.c_str(), static_cast<unsigned>(options_.port),
             options_.workerThreads);
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    listener_.close();
    queueCv_.notify_all();
    if (acceptThread_.joinable()) acceptThread_.join();
    for (auto& t : workers_)
        if (t.joinable()) t.join();
    workers_.clear();
    {
        std::lock_guard<std::mutex> lk(queueMu_);
        while (!pending_.empty()) pending_.pop();
    }
    LOG_INFO(kTag, "Máy chủ web đã dừng");
}

void HttpServer::acceptLoop() {
    while (running_.load()) {
        std::string peerIp;
        net::TcpSocket client = listener_.accept(500, peerIp);
        if (!client.valid()) continue;
        {
            std::lock_guard<std::mutex> lk(queueMu_);
            if (pending_.size() > 4096) {
                LOG_WARN(kTag, "Hàng đợi kết nối đầy, từ chối %s", peerIp.c_str());
                continue;
            }
            pending_.emplace(std::move(client), peerIp);
        }
        queueCv_.notify_one();
    }
}

void HttpServer::workerLoop() {
    while (running_.load()) {
        std::pair<net::TcpSocket, std::string> item;
        {
            std::unique_lock<std::mutex> lk(queueMu_);
            queueCv_.wait_for(lk, std::chrono::milliseconds(500),
                              [this]() { return !pending_.empty() || !running_.load(); });
            if (pending_.empty()) continue;
            item = std::move(pending_.front());
            pending_.pop();
        }
        activeConnections_.fetch_add(1);
        handleConnection(std::move(item.first), item.second);
        activeConnections_.fetch_sub(1);
    }
}

void HttpServer::handleConnection(net::TcpSocket socket, const std::string& peerIp) {
    int timeoutMs = options_.idleTimeoutSeconds * 1000;
    SocketBuffer buf(socket, timeoutMs);

    while (running_.load() && socket.valid()) {
        std::string line;
        if (!buf.readLine(line, options_.maxHeaderBytes)) break;
        if (line.empty()) continue;  // dòng trống giữa các yêu cầu

        Request req;
        req.clientIp = peerIp;

        // Dòng yêu cầu: METHOD TARGET VERSION
        size_t sp1 = line.find(' ');
        if (sp1 == std::string::npos) break;
        size_t sp2 = line.find(' ', sp1 + 1);
        req.method = line.substr(0, sp1);
        req.target = sp2 == std::string::npos ? line.substr(sp1 + 1)
                                              : line.substr(sp1 + 1, sp2 - sp1 - 1);
        req.version = sp2 == std::string::npos ? "HTTP/1.0" : line.substr(sp2 + 1);

        size_t qm = req.target.find('?');
        if (qm == std::string::npos) {
            req.rawPath = req.target;
        } else {
            req.rawPath = req.target.substr(0, qm);
            req.query = req.target.substr(qm + 1);
            req.params = parseQueryString(req.query);
        }
        req.path = urlDecodePath(req.rawPath);

        // Tiêu đề
        uint64_t headerBytes = line.size();
        while (true) {
            if (!buf.readLine(line, options_.maxHeaderBytes)) return;
            if (line.empty()) break;
            headerBytes += line.size();
            if (headerBytes > options_.maxHeaderBytes) return;
            size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string name = trim(line.substr(0, colon));
            std::string value = trim(line.substr(colon + 1));
            auto it = req.headers.find(name);
            if (it != req.headers.end()) {
                it->second += ", " + value;
            } else {
                req.headers[name] = value;
            }
        }

        std::string connection = toLower(req.header("Connection"));
        req.keepAlive = (req.version != "HTTP/1.0" && connection != "close") ||
                        connection == "keep-alive";
        std::string cookieHeader = req.header("Cookie");
        if (!cookieHeader.empty()) req.cookies = parseCookies(cookieHeader);

        std::string te = toLower(req.header("Transfer-Encoding"));
        req.chunked = te.find("chunked") != std::string::npos;
        std::string cl = req.header("Content-Length");
        if (!cl.empty()) parseUInt64(cl, req.contentLength);

        std::unique_ptr<BodyReader> reader;
        if (req.chunked) {
            reader.reset(new ChunkedBodyReader(buf));
        } else if (req.contentLength > 0) {
            reader.reset(new LengthBodyReader(buf, req.contentLength));
        } else {
            reader.reset(new EmptyBodyReader());
        }

        Response res;
        res.headers["Server"] = "Tuan-Telegram-Disk";
        res.headers["Date"] = formatHttpDate(nowUnix());

        int64_t startMs = monotonicMillis();
        bool handled = false;

        if (preFilter_ && !preFilter_(req, res)) {
            handled = true;
        } else {
            const Route* r = matchRoute(req);
            Handler handler = r ? r->handler : fallback_;
            if (handler) {
                try {
                    handler(req, *reader, res);
                } catch (const std::exception& e) {
                    LOG_ERROR(kTag, "Lỗi khi xử lý %s %s: %s", req.method.c_str(),
                              req.path.c_str(), e.what());
                    res = Response();
                    res.setError(500, std::string("Lỗi máy chủ: ") + e.what());
                } catch (...) {
                    LOG_ERROR(kTag, "Lỗi không xác định khi xử lý %s %s", req.method.c_str(),
                              req.path.c_str());
                    res = Response();
                    res.setError(500, "Lỗi máy chủ không xác định");
                }
                handled = true;
            }
        }
        if (!handled) res.setError(404, "Không tìm thấy đường dẫn " + req.path);

        // Đọc nốt phần thân chưa dùng để giữ được kết nối.
        if (!reader->complete()) {
            uint8_t discard[16384];
            int guard = 0;
            while (!reader->complete() && ++guard < 100000) {
                long n = reader->read(discard, sizeof(discard));
                if (n <= 0) break;
            }
            if (!reader->complete()) req.keepAlive = false;
        }

        bool keepAlive = req.keepAlive && !res.closeConnection;

        // Ghi phản hồi.
        std::string head;
        head.reserve(512);
        head += "HTTP/1.1 " + std::to_string(res.status) + " " +
                (res.statusText.empty() ? statusText(res.status) : res.statusText) + "\r\n";

        bool useChunked = false;
        if (res.streamBody) {
            if (res.streamLength > 0) {
                res.headers["Content-Length"] = std::to_string(res.streamLength);
            } else {
                res.headers["Transfer-Encoding"] = "chunked";
                useChunked = true;
            }
        } else {
            res.headers["Content-Length"] = std::to_string(res.body.size());
        }
        if (res.headers.find("Content-Type") == res.headers.end() &&
            (!res.body.empty() || res.streamBody))
            res.headers["Content-Type"] = "application/octet-stream";
        res.headers["Connection"] = keepAlive ? "keep-alive" : "close";

        for (const auto& kv : res.headers) head += kv.first + ": " + kv.second + "\r\n";
        head += "\r\n";

        if (!socket.sendAll(head, timeoutMs)) break;
        bytesSent_.fetch_add(head.size());

        bool sendOk = true;
        if (req.method == "HEAD") {
            // Không gửi thân phản hồi.
        } else if (res.streamBody) {
            ResponseWriter writer = [&](const char* data, size_t len) -> bool {
                if (len == 0) return true;
                if (useChunked) {
                    char sizeBuf[32];
                    int n = std::snprintf(sizeBuf, sizeof(sizeBuf), "%zx\r\n", len);
                    if (!socket.sendAll(reinterpret_cast<const uint8_t*>(sizeBuf),
                                        static_cast<size_t>(n), timeoutMs))
                        return false;
                }
                if (!socket.sendAll(reinterpret_cast<const uint8_t*>(data), len, timeoutMs))
                    return false;
                if (useChunked) {
                    if (!socket.sendAll(reinterpret_cast<const uint8_t*>("\r\n"), 2, timeoutMs))
                        return false;
                }
                bytesSent_.fetch_add(len);
                return true;
            };
            sendOk = res.streamBody(writer);
            if (useChunked && sendOk)
                sendOk = socket.sendAll(reinterpret_cast<const uint8_t*>("0\r\n\r\n"), 5,
                                        timeoutMs);
        } else if (!res.body.empty()) {
            sendOk = socket.sendAll(res.body, timeoutMs);
            if (sendOk) bytesSent_.fetch_add(res.body.size());
        }

        requestsHandled_.fetch_add(1);
        if (options_.logRequests) {
            int64_t took = monotonicMillis() - startMs;
            LOG_DEBUG(kTag, "%s %s -> %d (%lld ms, %s)", req.method.c_str(), req.target.c_str(),
                      res.status, static_cast<long long>(took), peerIp.c_str());
        }

        if (!sendOk || !keepAlive) break;
    }

    bytesReceived_.fetch_add(buf.bytesReceived());
    socket.close();
}

}  // namespace http
}  // namespace ttd
