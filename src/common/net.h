// Lớp bọc socket TCP đa nền tảng (Linux + Windows) có hỗ trợ thời gian chờ.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/strutil.h"

namespace ttd {
namespace net {

// Phải gọi một lần khi khởi động (WSAStartup trên Windows).
void initNetworking();

#if defined(_WIN32)
using SocketHandle = uintptr_t;
constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~0);
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

class TcpSocket {
public:
    TcpSocket() = default;
    explicit TcpSocket(SocketHandle fd) : fd_(fd) {}
    ~TcpSocket();
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& o) noexcept;
    TcpSocket& operator=(TcpSocket&& o) noexcept;

    // Kết nối tới host:port. host có thể là tên miền hoặc địa chỉ IP.
    bool connect(const std::string& host, uint16_t port, int timeoutMs, std::string& error);
    // Kết nối trực tiếp tới một địa chỉ IP đã biết (bỏ qua phân giải tên).
    bool connectIp(const std::string& ip, uint16_t port, int timeoutMs, std::string& error);

    // Gửi toàn bộ dữ liệu; trả về false nếu lỗi hoặc hết giờ.
    bool sendAll(const uint8_t* data, size_t len, int timeoutMs);
    bool sendAll(const Bytes& data, int timeoutMs) {
        return sendAll(data.data(), data.size(), timeoutMs);
    }
    bool sendAll(const std::string& data, int timeoutMs) {
        return sendAll(reinterpret_cast<const uint8_t*>(data.data()), data.size(), timeoutMs);
    }
    // Nhận đúng `len` byte.
    bool recvAll(uint8_t* data, size_t len, int timeoutMs);
    // Nhận tối đa `len` byte, trả về số byte thực nhận (0 = đóng kết nối, -1 = lỗi).
    long recvSome(uint8_t* data, size_t len, int timeoutMs);

    void close();
    void shutdownBoth();
    bool valid() const { return fd_ != kInvalidSocket; }
    SocketHandle handle() const { return fd_; }
    void setNoDelay(bool on);
    void setKeepAlive(bool on);
    std::string peerAddress() const;

    // Chờ tới khi socket có dữ liệu để đọc. Trả về true nếu có.
    bool waitReadable(int timeoutMs) const;

private:
    SocketHandle fd_ = kInvalidSocket;
};

class TcpListener {
public:
    ~TcpListener();
    // bindAddress: "0.0.0.0", "127.0.0.1", "::" ...
    bool listenOn(const std::string& bindAddress, uint16_t port, int backlog, std::string& error);
    // Chấp nhận kết nối mới. Trả về socket không hợp lệ nếu hết giờ hoặc bị đóng.
    TcpSocket accept(int timeoutMs, std::string& peerIp);
    void close();
    uint16_t boundPort() const { return port_; }

private:
    SocketHandle fd_ = kInvalidSocket;
    uint16_t port_ = 0;
};

// Địa chỉ IP cục bộ (dùng để in ra hướng dẫn truy cập).
std::vector<std::string> localIpAddresses();

}  // namespace net
}  // namespace ttd
