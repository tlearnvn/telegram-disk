#include "common/net.h"

#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
using socklen_t = int;
#define TTD_SOCK_ERR WSAGetLastError()
#define TTD_EWOULDBLOCK WSAEWOULDBLOCK
#define TTD_EINPROGRESS WSAEWOULDBLOCK
#define TTD_EINTR WSAEINTR
#else
#include <arpa/inet.h>
#include <csignal>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#define TTD_SOCK_ERR errno
#define TTD_EWOULDBLOCK EWOULDBLOCK
#define TTD_EINPROGRESS EINPROGRESS
#define TTD_EINTR EINTR
#endif

#include "common/dns.h"
#include "common/logging.h"
#include "common/timeutil.h"

namespace ttd {
namespace net {

namespace {
constexpr const char* kTag = "net";

void closeSocketHandle(SocketHandle fd) {
    if (fd == kInvalidSocket) return;
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

bool setNonBlocking(SocketHandle fd, bool on) {
#if defined(_WIN32)
    u_long mode = on ? 1 : 0;
    return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (on) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return ::fcntl(fd, F_SETFL, flags) == 0;
#endif
}

// Chờ socket sẵn sàng đọc/ghi. Trả về 1 = sẵn sàng, 0 = hết giờ, -1 = lỗi.
int waitSocket(SocketHandle fd, bool forWrite, int timeoutMs) {
#if defined(_WIN32)
    fd_set set;
    FD_ZERO(&set);
    FD_SET(static_cast<SOCKET>(fd), &set);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int rc = ::select(0, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr,
                      timeoutMs < 0 ? nullptr : &tv);
    return rc;
#else
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = static_cast<short>(forWrite ? POLLOUT : POLLIN);
    pfd.revents = 0;
    int rc;
    do {
        rc = ::poll(&pfd, 1, timeoutMs);
    } while (rc < 0 && errno == EINTR);
    return rc;
#endif
}

}  // namespace

void initNetworking() {
    static std::once_flag once;
    std::call_once(once, []() {
#if defined(_WIN32)
        WSADATA wsa;
        int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0) {
            LOG_ERROR(kTag, "WSAStartup thất bại: %d", rc);
        }
#else
        // Bỏ qua SIGPIPE để việc ghi vào socket đã đóng trả về lỗi thay vì kết thúc tiến trình.
        ::signal(SIGPIPE, SIG_IGN);
#endif
    });
}

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = kInvalidSocket; }

TcpSocket& TcpSocket::operator=(TcpSocket&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        o.fd_ = kInvalidSocket;
    }
    return *this;
}

void TcpSocket::close() {
    if (fd_ != kInvalidSocket) {
        closeSocketHandle(fd_);
        fd_ = kInvalidSocket;
    }
}

void TcpSocket::shutdownBoth() {
    if (fd_ == kInvalidSocket) return;
#if defined(_WIN32)
    ::shutdown(static_cast<SOCKET>(fd_), SD_BOTH);
#else
    ::shutdown(fd_, SHUT_RDWR);
#endif
}

void TcpSocket::setNoDelay(bool on) {
    if (fd_ == kInvalidSocket) return;
    int flag = on ? 1 : 0;
    ::setsockopt(static_cast<int>(fd_), IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&flag), sizeof(flag));
}

void TcpSocket::setKeepAlive(bool on) {
    if (fd_ == kInvalidSocket) return;
    int flag = on ? 1 : 0;
    ::setsockopt(static_cast<int>(fd_), SOL_SOCKET, SO_KEEPALIVE,
                 reinterpret_cast<const char*>(&flag), sizeof(flag));
}

bool TcpSocket::connectIp(const std::string& ip, uint16_t port, int timeoutMs,
                          std::string& error) {
    initNetworking();
    close();

    sockaddr_storage addr;
    std::memset(&addr, 0, sizeof(addr));
    socklen_t addrLen = 0;
    int family = 0;

    sockaddr_in v4;
    std::memset(&v4, 0, sizeof(v4));
    sockaddr_in6 v6;
    std::memset(&v6, 0, sizeof(v6));

    if (::inet_pton(AF_INET, ip.c_str(), &v4.sin_addr) == 1) {
        v4.sin_family = AF_INET;
        v4.sin_port = htons(port);
        std::memcpy(&addr, &v4, sizeof(v4));
        addrLen = sizeof(v4);
        family = AF_INET;
    } else if (::inet_pton(AF_INET6, ip.c_str(), &v6.sin6_addr) == 1) {
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(port);
        std::memcpy(&addr, &v6, sizeof(v6));
        addrLen = sizeof(v6);
        family = AF_INET6;
    } else {
        error = "Địa chỉ IP không hợp lệ: " + ip;
        return false;
    }

    SocketHandle fd = static_cast<SocketHandle>(::socket(family, SOCK_STREAM, IPPROTO_TCP));
    if (fd == kInvalidSocket) {
        error = "Không tạo được socket";
        return false;
    }
    setNonBlocking(fd, true);

    int rc = ::connect(static_cast<int>(fd), reinterpret_cast<sockaddr*>(&addr), addrLen);
    if (rc != 0) {
        int err = TTD_SOCK_ERR;
#if defined(_WIN32)
        bool pending = (err == WSAEWOULDBLOCK);
#else
        bool pending = (err == EINPROGRESS);
#endif
        if (!pending) {
            closeSocketHandle(fd);
            error = "Kết nối thất bại tới " + ip + " (mã lỗi " + std::to_string(err) + ")";
            return false;
        }
        int w = waitSocket(fd, true, timeoutMs);
        if (w <= 0) {
            closeSocketHandle(fd);
            error = w == 0 ? ("Hết thời gian chờ khi kết nối tới " + ip)
                           : ("Lỗi chờ kết nối tới " + ip);
            return false;
        }
        int soErr = 0;
        socklen_t len = sizeof(soErr);
        ::getsockopt(static_cast<int>(fd), SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&soErr), &len);
        if (soErr != 0) {
            closeSocketHandle(fd);
            error = "Kết nối bị từ chối tới " + ip + " (mã lỗi " + std::to_string(soErr) + ")";
            return false;
        }
    }

    fd_ = fd;
    setNoDelay(true);
    setKeepAlive(true);
    return true;
}

bool TcpSocket::connect(const std::string& host, uint16_t port, int timeoutMs,
                        std::string& error) {
    std::vector<std::string> ips = dns::resolve(host, timeoutMs);
    if (ips.empty()) {
        error = "Không phân giải được tên miền: " + host;
        return false;
    }
    int64_t deadline = monotonicMillis() + timeoutMs;
    for (const auto& ip : ips) {
        int remain = static_cast<int>(deadline - monotonicMillis());
        if (remain < 1000) remain = 1000;
        if (connectIp(ip, port, remain, error)) return true;
        LOG_DEBUG(kTag, "Thử IP %s cho %s thất bại: %s", ip.c_str(), host.c_str(), error.c_str());
    }
    return false;
}

bool TcpSocket::sendAll(const uint8_t* data, size_t len, int timeoutMs) {
    if (fd_ == kInvalidSocket) return false;
    size_t sent = 0;
    int64_t deadline = monotonicMillis() + timeoutMs;
    while (sent < len) {
        int remain = static_cast<int>(deadline - monotonicMillis());
        if (remain <= 0) return false;
#if defined(_WIN32)
        int n = ::send(static_cast<SOCKET>(fd_), reinterpret_cast<const char*>(data + sent),
                       static_cast<int>(len - sent), 0);
#else
        ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
#endif
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        int err = TTD_SOCK_ERR;
        if (err == TTD_EINTR) continue;
        if (err == TTD_EWOULDBLOCK
#if !defined(_WIN32)
            || err == EAGAIN
#endif
        ) {
            if (waitSocket(fd_, true, remain) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

long TcpSocket::recvSome(uint8_t* data, size_t len, int timeoutMs) {
    if (fd_ == kInvalidSocket) return -1;
    int64_t deadline = monotonicMillis() + timeoutMs;
    while (true) {
        int remain = static_cast<int>(deadline - monotonicMillis());
        if (remain <= 0) return -1;
#if defined(_WIN32)
        int n = ::recv(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(data),
                       static_cast<int>(len), 0);
#else
        ssize_t n = ::recv(fd_, data, len, 0);
#endif
        if (n > 0) return static_cast<long>(n);
        if (n == 0) return 0;  // đối phương đóng kết nối
        int err = TTD_SOCK_ERR;
        if (err == TTD_EINTR) continue;
        if (err == TTD_EWOULDBLOCK
#if !defined(_WIN32)
            || err == EAGAIN
#endif
        ) {
            int w = waitSocket(fd_, false, remain);
            if (w <= 0) return -1;
            continue;
        }
        return -1;
    }
}

bool TcpSocket::recvAll(uint8_t* data, size_t len, int timeoutMs) {
    size_t got = 0;
    int64_t deadline = monotonicMillis() + timeoutMs;
    while (got < len) {
        int remain = static_cast<int>(deadline - monotonicMillis());
        if (remain <= 0) return false;
        long n = recvSome(data + got, len - got, remain);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool TcpSocket::waitReadable(int timeoutMs) const {
    if (fd_ == kInvalidSocket) return false;
    return waitSocket(fd_, false, timeoutMs) > 0;
}

std::string TcpSocket::peerAddress() const {
    if (fd_ == kInvalidSocket) return "";
    sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (::getpeername(static_cast<int>(fd_), reinterpret_cast<sockaddr*>(&ss), &len) != 0)
        return "";
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        auto* a = reinterpret_cast<sockaddr_in*>(&ss);
        ::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
    } else {
        auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
        ::inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
    }
    return buf;
}

// ---------------------------------------------------------------------------
//  TcpListener
// ---------------------------------------------------------------------------
TcpListener::~TcpListener() { close(); }

void TcpListener::close() {
    if (fd_ != kInvalidSocket) {
        closeSocketHandle(fd_);
        fd_ = kInvalidSocket;
    }
}

bool TcpListener::listenOn(const std::string& bindAddress, uint16_t port, int backlog,
                           std::string& error) {
    initNetworking();
    close();

    bool useV6 = bindAddress.find(':') != std::string::npos;
    int family = useV6 ? AF_INET6 : AF_INET;
    SocketHandle fd = static_cast<SocketHandle>(::socket(family, SOCK_STREAM, IPPROTO_TCP));
    if (fd == kInvalidSocket) {
        error = "Không tạo được socket lắng nghe";
        return false;
    }

    int yes = 1;
    ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    int rc;
    if (useV6) {
        int off = 0;
        ::setsockopt(static_cast<int>(fd), IPPROTO_IPV6, IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&off), sizeof(off));
        sockaddr_in6 a;
        std::memset(&a, 0, sizeof(a));
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(port);
        if (bindAddress == "::" || bindAddress.empty()) {
            a.sin6_addr = in6addr_any;
        } else if (::inet_pton(AF_INET6, bindAddress.c_str(), &a.sin6_addr) != 1) {
            closeSocketHandle(fd);
            error = "Địa chỉ IPv6 không hợp lệ: " + bindAddress;
            return false;
        }
        rc = ::bind(static_cast<int>(fd), reinterpret_cast<sockaddr*>(&a), sizeof(a));
    } else {
        sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        if (bindAddress.empty() || bindAddress == "0.0.0.0" || bindAddress == "*") {
            a.sin_addr.s_addr = INADDR_ANY;
        } else if (::inet_pton(AF_INET, bindAddress.c_str(), &a.sin_addr) != 1) {
            closeSocketHandle(fd);
            error = "Địa chỉ IPv4 không hợp lệ: " + bindAddress;
            return false;
        }
        rc = ::bind(static_cast<int>(fd), reinterpret_cast<sockaddr*>(&a), sizeof(a));
    }

    if (rc != 0) {
        int err = TTD_SOCK_ERR;
        closeSocketHandle(fd);
        error = "Không gắn được cổng " + std::to_string(port) + " (mã lỗi " +
                std::to_string(err) + "). Cổng có thể đang bị chiếm.";
        return false;
    }
    if (::listen(static_cast<int>(fd), backlog) != 0) {
        closeSocketHandle(fd);
        error = "Không lắng nghe được trên cổng " + std::to_string(port);
        return false;
    }
    setNonBlocking(fd, true);
    fd_ = fd;
    port_ = port;
    return true;
}

TcpSocket TcpListener::accept(int timeoutMs, std::string& peerIp) {
    peerIp.clear();
    if (fd_ == kInvalidSocket) return TcpSocket();
    int w = waitSocket(fd_, false, timeoutMs);
    if (w <= 0) return TcpSocket();

    sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    SocketHandle client = static_cast<SocketHandle>(
        ::accept(static_cast<int>(fd_), reinterpret_cast<sockaddr*>(&ss), &len));
    if (client == kInvalidSocket) return TcpSocket();

    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        auto* a = reinterpret_cast<sockaddr_in*>(&ss);
        ::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
    } else {
        auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
        ::inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
    }
    peerIp = buf;
    // Bỏ tiền tố IPv4-mapped cho dễ đọc.
    if (startsWith(peerIp, "::ffff:")) peerIp = peerIp.substr(7);

    setNonBlocking(client, true);
    TcpSocket s(client);
    s.setNoDelay(true);
    return s;
}

std::vector<std::string> localIpAddresses() {
    std::vector<std::string> out;
    initNetworking();
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                            GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                             &size) == NO_ERROR) {
        auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        for (; aa; aa = aa->Next) {
            if (aa->OperStatus != IfOperStatusUp) continue;
            for (auto* ua = aa->FirstUnicastAddress; ua; ua = ua->Next) {
                char ip[INET6_ADDRSTRLEN] = {0};
                if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                    auto* a = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                    ::inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
                } else {
                    continue;
                }
                std::string s = ip;
                if (s != "127.0.0.1" && !s.empty()) out.push_back(s);
            }
        }
    }
#else
    struct ifaddrs* ifs = nullptr;
    if (::getifaddrs(&ifs) == 0) {
        for (struct ifaddrs* it = ifs; it; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
            char ip[INET6_ADDRSTRLEN] = {0};
            auto* a = reinterpret_cast<sockaddr_in*>(it->ifa_addr);
            ::inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
            std::string s = ip;
            if (s != "127.0.0.1" && !s.empty()) out.push_back(s);
        }
        ::freeifaddrs(ifs);
    }
#endif
    return out;
}

}  // namespace net
}  // namespace ttd
