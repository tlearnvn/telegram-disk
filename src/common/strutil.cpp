#include "common/strutil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace ttd {

namespace {
inline char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
inline char upperAscii(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}
inline int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string toLower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = lowerAscii(c);
    return r;
}

std::string toUpper(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = upperAscii(c);
    return r;
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && std::memcmp(s.data(), prefix.data(), prefix.size()) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           std::memcmp(s.data() + s.size() - suffix.size(), suffix.data(), suffix.size()) == 0;
}

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    return true;
}

std::vector<std::string> split(const std::string& s, char sep, bool keepEmpty) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (keepEmpty || !cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (keepEmpty || !cur.empty()) out.push_back(cur);
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

bool globMatch(const std::string& pattern, const std::string& text) {
    // Thuật toán hai con trỏ có quay lui, O(n*m) xấu nhất nhưng thực tế tuyến tính.
    size_t p = 0, t = 0, starP = std::string::npos, starT = 0;
    while (t < text.size()) {
        if (p < pattern.size() &&
            (pattern[p] == '?' || lowerAscii(pattern[p]) == lowerAscii(text[t]))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            starP = p++;
            starT = t;
        } else if (starP != std::string::npos) {
            p = starP + 1;
            t = ++starT;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string toHex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
    }
    return out;
}

std::string toHex(const Bytes& b) { return toHex(b.data(), b.size()); }

Bytes fromHex(const std::string& hex) {
    Bytes out;
    out.reserve(hex.size() / 2);
    int hi = -1;
    for (char c : hex) {
        int v = hexVal(c);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(static_cast<uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    return out;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
        i += 3;
    }
    if (i + 1 == len) {
        uint32_t v = uint32_t(data[i]) << 16;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == len) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

std::string base64Encode(const Bytes& b) { return base64Encode(b.data(), b.size()); }

Bytes base64Decode(const std::string& s) {
    static int8_t table[256];
    static bool inited = false;
    if (!inited) {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        for (int i = 0; i < 64; ++i) table[static_cast<uint8_t>(kB64[i])] = static_cast<int8_t>(i);
        table[static_cast<uint8_t>('-')] = 62;  // biến thể url-safe
        table[static_cast<uint8_t>('_')] = 63;
        inited = true;
    }
    Bytes out;
    out.reserve(s.size() * 3 / 4 + 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int8_t v = table[static_cast<uint8_t>(c)];
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
        }
    }
    return out;
}

std::string base64UrlEncode(const Bytes& b) {
    std::string s = base64Encode(b);
    for (auto& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

std::string urlEncode(const std::string& s, bool keepSlash) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~';
        if (keepSlash && c == '/') safe = true;
        if (safe) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0f]);
        }
    }
    return out;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hexVal(s[i + 1]), l = hexVal(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>((h << 4) | l));
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // Bỏ ký tự điều khiển không hợp lệ trong XML 1.0.
                if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') break;
                out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string normalizeVirtualPath(const std::string& path) {
    std::vector<std::string> stack;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty() || cur == ".") {
            cur.clear();
            return;
        }
        if (cur == "..") {
            if (!stack.empty()) stack.pop_back();
        } else {
            stack.push_back(cur);
        }
        cur.clear();
    };
    for (char c : path) {
        if (c == '/' || c == '\\') {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    if (stack.empty()) return "/";
    std::string out;
    for (const auto& p : stack) {
        out.push_back('/');
        out += p;
    }
    return out;
}

std::string parentPath(const std::string& path) {
    std::string p = normalizeVirtualPath(path);
    if (p == "/") return "/";
    size_t pos = p.rfind('/');
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

std::string baseName(const std::string& path) {
    std::string p = normalizeVirtualPath(path);
    if (p == "/") return "";
    size_t pos = p.rfind('/');
    return p.substr(pos + 1);
}

std::string fileExtension(const std::string& name) {
    size_t pos = name.rfind('.');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= name.size()) return "";
    std::string ext = name.substr(pos + 1);
    if (ext.size() > 12) return "";
    return toLower(ext);
}

std::string stripExtension(const std::string& name) {
    size_t pos = name.rfind('.');
    if (pos == std::string::npos || pos == 0) return name;
    return name.substr(0, pos);
}

std::string sanitizeFileName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c < 0x20) continue;
        switch (c) {
            case '/': case '\\': case ':': case '*': case '?':
            case '"': case '<': case '>': case '|':
                out.push_back('_');
                break;
            default:
                out.push_back(static_cast<char>(c));
        }
    }
    out = trim(out);
    while (!out.empty() && out.back() == '.') out.pop_back();
    if (out.empty()) out = "khong-ten";
    return utf8TruncateBytes(out, 240);
}

std::string makeUniqueName(const std::string& name, int counter) {
    if (counter <= 1) return name;
    std::string ext = fileExtension(name);
    std::string base = ext.empty() ? name : name.substr(0, name.size() - ext.size() - 1);
    std::string suffix = " (" + std::to_string(counter) + ")";
    if (ext.empty()) return base + suffix;
    return base + suffix + "." + ext;
}

std::string formatBytes(uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        ++u;
    }
    char buf[64];
    if (u == 0) {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[u]);
        // Dấu thập phân kiểu Việt Nam.
        for (char* p = buf; *p; ++p)
            if (*p == '.') *p = ',';
    }
    return buf;
}

std::string formatNumber(uint64_t n) {
    std::string s = std::to_string(n);
    std::string out;
    int cnt = 0;
    for (int i = static_cast<int>(s.size()) - 1; i >= 0; --i) {
        out.push_back(s[static_cast<size_t>(i)]);
        if (++cnt % 3 == 0 && i > 0) out.push_back('.');
    }
    std::reverse(out.begin(), out.end());
    return out;
}

uint64_t parseSizeString(const std::string& s, uint64_t fallback) {
    std::string t = trim(toUpper(s));
    if (t.empty()) return fallback;
    size_t i = 0;
    long double num = 0;
    bool seenDigit = false;
    while (i < t.size() && ((t[i] >= '0' && t[i] <= '9'))) {
        num = num * 10 + (t[i] - '0');
        seenDigit = true;
        ++i;
    }
    if (i < t.size() && (t[i] == '.' || t[i] == ',')) {
        ++i;
        long double frac = 0.1L;
        while (i < t.size() && t[i] >= '0' && t[i] <= '9') {
            num += (t[i] - '0') * frac;
            frac /= 10;
            seenDigit = true;
            ++i;
        }
    }
    if (!seenDigit) return fallback;
    while (i < t.size() && t[i] == ' ') ++i;
    std::string unit = t.substr(i);
    long double mult = 1;
    if (unit.empty() || unit == "B") mult = 1;
    else if (unit == "K" || unit == "KB" || unit == "KIB") mult = 1024.0L;
    else if (unit == "M" || unit == "MB" || unit == "MIB") mult = 1024.0L * 1024;
    else if (unit == "G" || unit == "GB" || unit == "GIB") mult = 1024.0L * 1024 * 1024;
    else if (unit == "T" || unit == "TB" || unit == "TIB") mult = 1024.0L * 1024 * 1024 * 1024;
    else return fallback;
    long double total = num * mult;
    if (total < 0) return fallback;
    return static_cast<uint64_t>(total);
}

bool parseInt64(const std::string& s, int64_t& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (t[0] == '-' || t[0] == '+') {
        neg = (t[0] == '-');
        i = 1;
    }
    if (i >= t.size()) return false;
    unsigned long long v = 0;
    for (; i < t.size(); ++i) {
        if (t[i] < '0' || t[i] > '9') return false;
        unsigned long long nv = v * 10 + static_cast<unsigned long long>(t[i] - '0');
        if (nv < v) return false;
        v = nv;
    }
    out = neg ? -static_cast<int64_t>(v) : static_cast<int64_t>(v);
    return true;
}

bool parseUInt64(const std::string& s, uint64_t& out) {
    std::string t = trim(s);
    if (t.empty()) return false;
    unsigned long long v = 0;
    for (char c : t) {
        if (c < '0' || c > '9') return false;
        unsigned long long nv = v * 10 + static_cast<unsigned long long>(c - '0');
        if (nv < v) return false;
        v = nv;
    }
    out = v;
    return true;
}

std::string toString(int64_t v) { return std::to_string(v); }
std::string toString(uint64_t v) { return std::to_string(v); }

bool isValidUtf8(const std::string& s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra;
        uint32_t cp;
        if (c < 0x80) { ++i; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else return false;
        if (i + extra >= n) return false;
        for (size_t k = 1; k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += extra + 1;
    }
    return true;
}

size_t utf8Length(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string utf8TruncateBytes(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

}  // namespace ttd
