#include "common/dns.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "common/fsutil.h"
#include "common/logging.h"
#include "common/strutil.h"
#include "common/timeutil.h"
#include "crypto/random.h"

namespace ttd {
namespace dns {

namespace {

constexpr const char* kTag = "dns";

struct CacheEntry {
    std::vector<std::string> ips;
    int64_t expiresAt = 0;
};

std::mutex& cacheMutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, CacheEntry>& cache() {
    static std::map<std::string, CacheEntry> c;
    return c;
}

bool looksLikeIp(const std::string& host) {
    if (host.find(':') != std::string::npos) return true;  // IPv6
    int dots = 0;
    for (char c : host) {
        if (c == '.') ++dots;
        else if (c < '0' || c > '9') return false;
    }
    return dots == 3;
}

std::vector<std::string> resolveSystem(const std::string& host) {
    std::vector<std::string> out;
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) return out;
    for (struct addrinfo* it = res; it; it = it->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {0};
        if (it->ai_family == AF_INET) {
            auto* a = reinterpret_cast<sockaddr_in*>(it->ai_addr);
            ::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
        } else if (it->ai_family == AF_INET6) {
            auto* a = reinterpret_cast<sockaddr_in6*>(it->ai_addr);
            ::inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
        } else {
            continue;
        }
        std::string s = buf;
        if (!s.empty()) out.push_back(s);
    }
    ::freeaddrinfo(res);
    // Ưu tiên IPv4 vì đường ra IPv6 hay bị chặn ở nhiều mạng.
    std::stable_sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        bool a6 = a.find(':') != std::string::npos;
        bool b6 = b.find(':') != std::string::npos;
        return !a6 && b6;
    });
    return out;
}

// Tìm trong tệp hosts của hệ thống.
std::vector<std::string> lookupHostsFile(const std::string& host) {
    std::vector<std::string> out;
#if defined(_WIN32)
    const char* path = "C:/Windows/System32/drivers/etc/hosts";
#else
    const char* path = "/etc/hosts";
#endif
    std::string content;
    if (!readWholeFile(path, content)) return out;
    for (const auto& rawLine : split(content, '\n')) {
        std::string line = rawLine;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;
        std::vector<std::string> parts;
        std::string cur;
        for (char c : line) {
            if (c == ' ' || c == '\t') {
                if (!cur.empty()) parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) parts.push_back(cur);
        if (parts.size() < 2) continue;
        for (size_t i = 1; i < parts.size(); ++i) {
            if (iequals(parts[i], host)) {
                out.push_back(parts[0]);
                break;
            }
        }
    }
    return out;
}

void encodeDnsName(const std::string& host, std::string& out) {
    for (const auto& label : split(host, '.', false)) {
        if (label.empty() || label.size() > 63) continue;
        out.push_back(static_cast<char>(label.size()));
        out += label;
    }
    out.push_back('\0');
}

// Bỏ qua một tên trong thông điệp DNS (có thể là con trỏ nén).
bool skipDnsName(const uint8_t* buf, size_t len, size_t& pos) {
    int guard = 0;
    while (pos < len && ++guard < 128) {
        uint8_t l = buf[pos];
        if (l == 0) {
            ++pos;
            return true;
        }
        if ((l & 0xc0) == 0xc0) {
            pos += 2;
            return pos <= len;
        }
        pos += 1 + l;
    }
    return false;
}

std::vector<std::string> parseDnsResponse(const uint8_t* buf, size_t len, uint16_t wantId) {
    std::vector<std::string> out;
    if (len < 12) return out;
    uint16_t id = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
    if (id != wantId) return out;
    uint16_t flags = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
    if ((flags & 0x000f) != 0) return out;  // RCODE khác 0
    uint16_t qdCount = static_cast<uint16_t>((buf[4] << 8) | buf[5]);
    uint16_t anCount = static_cast<uint16_t>((buf[6] << 8) | buf[7]);

    size_t pos = 12;
    for (uint16_t i = 0; i < qdCount; ++i) {
        if (!skipDnsName(buf, len, pos)) return out;
        pos += 4;
    }
    for (uint16_t i = 0; i < anCount && pos + 10 <= len; ++i) {
        if (!skipDnsName(buf, len, pos)) return out;
        if (pos + 10 > len) return out;
        uint16_t type = static_cast<uint16_t>((buf[pos] << 8) | buf[pos + 1]);
        uint16_t rdLen = static_cast<uint16_t>((buf[pos + 8] << 8) | buf[pos + 9]);
        pos += 10;
        if (pos + rdLen > len) return out;
        if (type == 1 && rdLen == 4) {
            char ip[INET_ADDRSTRLEN] = {0};
            ::inet_ntop(AF_INET, buf + pos, ip, sizeof(ip));
            out.push_back(ip);
        } else if (type == 28 && rdLen == 16) {
            char ip[INET6_ADDRSTRLEN] = {0};
            ::inet_ntop(AF_INET6, buf + pos, ip, sizeof(ip));
            out.push_back(ip);
        }
        pos += rdLen;
    }
    return out;
}

std::vector<std::string> queryServer(const std::string& server, const std::string& host,
                                     uint16_t qtype, int timeoutMs) {
    std::vector<std::string> out;
    uint16_t id = static_cast<uint16_t>(crypto::randomUInt32() & 0xffff);

    std::string query;
    query.push_back(static_cast<char>(id >> 8));
    query.push_back(static_cast<char>(id & 0xff));
    query.push_back(0x01);  // RD = 1
    query.push_back(0x00);
    query.append("\x00\x01", 2);  // QDCOUNT
    query.append("\x00\x00", 2);  // ANCOUNT
    query.append("\x00\x00", 2);  // NSCOUNT
    query.append("\x00\x00", 2);  // ARCOUNT
    encodeDnsName(host, query);
    query.push_back(static_cast<char>(qtype >> 8));
    query.push_back(static_cast<char>(qtype & 0xff));
    query.append("\x00\x01", 2);  // class IN

#if defined(_WIN32)
    SOCKET fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == INVALID_SOCKET) return out;
#else
    int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return out;
#endif
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    if (::inet_pton(AF_INET, server.c_str(), &addr.sin_addr) != 1) {
#if defined(_WIN32)
        ::closesocket(fd);
#else
        ::close(fd);
#endif
        return out;
    }

    ::sendto(static_cast<int>(fd), query.data(), static_cast<int>(query.size()), 0,
             reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

#if defined(_WIN32)
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int ready = ::select(0, &set, nullptr, nullptr, &tv);
#else
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ready = ::poll(&pfd, 1, timeoutMs);
#endif
    if (ready > 0) {
        uint8_t buf[4096];
#if defined(_WIN32)
        int n = ::recv(fd, reinterpret_cast<char*>(buf), static_cast<int>(sizeof(buf)), 0);
#else
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
#endif
        if (n > 0) out = parseDnsResponse(buf, static_cast<size_t>(n), id);
    }
#if defined(_WIN32)
    ::closesocket(fd);
#else
    ::close(fd);
#endif
    return out;
}

}  // namespace

std::vector<std::string> nameServers() {
    std::vector<std::string> out;
#if !defined(_WIN32)
    std::string content;
    if (readWholeFile("/etc/resolv.conf", content)) {
        for (const auto& rawLine : split(content, '\n')) {
            std::string line = trim(rawLine);
            if (!startsWith(line, "nameserver")) continue;
            std::string rest = trim(line.substr(10));
            size_t sp = rest.find_first_of(" \t");
            if (sp != std::string::npos) rest = rest.substr(0, sp);
            if (!rest.empty() && rest.find(':') == std::string::npos) out.push_back(rest);
        }
    }
#endif
    // Máy chủ công cộng làm phương án dự phòng cuối cùng.
    if (out.empty()) {
        out.push_back("1.1.1.1");
        out.push_back("8.8.8.8");
        out.push_back("9.9.9.9");
    }
    return out;
}

std::vector<std::string> resolveBuiltin(const std::string& host, int timeoutMs) {
    std::vector<std::string> out = lookupHostsFile(host);
    if (!out.empty()) return out;

    int perServer = timeoutMs / 2;
    if (perServer < 1500) perServer = 1500;
    for (const auto& server : nameServers()) {
        out = queryServer(server, host, 1 /* A */, perServer);
        if (!out.empty()) return out;
        out = queryServer(server, host, 28 /* AAAA */, perServer);
        if (!out.empty()) return out;
    }
    return out;
}

std::vector<std::string> resolve(const std::string& host, int timeoutMs) {
    if (host.empty()) return {};
    if (looksLikeIp(host)) return {host};

    int64_t now = nowUnix();
    {
        std::lock_guard<std::mutex> lk(cacheMutex());
        auto it = cache().find(host);
        if (it != cache().end() && it->second.expiresAt > now) return it->second.ips;
    }

    std::vector<std::string> ips = resolveSystem(host);
    if (ips.empty()) {
        LOG_DEBUG(kTag, "getaddrinfo không phân giải được '%s', dùng bộ phân giải nội bộ",
                  host.c_str());
        ips = resolveBuiltin(host, timeoutMs);
    }
    if (!ips.empty()) {
        std::lock_guard<std::mutex> lk(cacheMutex());
        cache()[host] = CacheEntry{ips, now + 300};
        LOG_TRACE(kTag, "Phân giải %s -> %s", host.c_str(), join(ips, ", ").c_str());
    } else {
        LOG_WARN(kTag, "Không phân giải được tên miền: %s", host.c_str());
    }
    return ips;
}

void clearCache() {
    std::lock_guard<std::mutex> lk(cacheMutex());
    cache().clear();
}

}  // namespace dns
}  // namespace ttd
