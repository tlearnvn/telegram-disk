// Tiện ích xử lý chuỗi, mã hoá hex/base64, URL, đường dẫn.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ttd {

using Bytes = std::vector<uint8_t>;

// ---- chuỗi cơ bản ---------------------------------------------------------
std::string trim(const std::string& s);
std::string toLower(const std::string& s);
std::string toUpper(const std::string& s);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
bool iequals(const std::string& a, const std::string& b);
std::vector<std::string> split(const std::string& s, char sep, bool keepEmpty = true);
std::string join(const std::vector<std::string>& parts, const std::string& sep);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
// So khớp mẫu đơn giản có '*' và '?' (không phân biệt hoa thường).
bool globMatch(const std::string& pattern, const std::string& text);

// ---- hex / base64 ---------------------------------------------------------
std::string toHex(const uint8_t* data, size_t len);
std::string toHex(const Bytes& b);
Bytes fromHex(const std::string& hex);
std::string base64Encode(const uint8_t* data, size_t len);
std::string base64Encode(const Bytes& b);
Bytes base64Decode(const std::string& s);
std::string base64UrlEncode(const Bytes& b);

// ---- URL ------------------------------------------------------------------
std::string urlEncode(const std::string& s, bool keepSlash = false);
std::string urlDecode(const std::string& s);

// ---- HTML / XML -----------------------------------------------------------
std::string htmlEscape(const std::string& s);
std::string xmlEscape(const std::string& s);

// ---- đường dẫn ảo ---------------------------------------------------------
// Chuẩn hoá đường dẫn ảo: luôn bắt đầu bằng '/', bỏ '..', bỏ '/' thừa và cuối.
std::string normalizeVirtualPath(const std::string& path);
std::string parentPath(const std::string& path);
std::string baseName(const std::string& path);
std::string fileExtension(const std::string& name);
std::string stripExtension(const std::string& name);
// Làm sạch tên tệp: bỏ ký tự điều khiển và ký tự cấm trên Windows.
std::string sanitizeFileName(const std::string& name);
// Sinh tên không trùng: "a.txt" -> "a (2).txt"
std::string makeUniqueName(const std::string& name, int counter);

// ---- số / dung lượng ------------------------------------------------------
std::string formatBytes(uint64_t bytes);          // 1,5 GB
std::string formatNumber(uint64_t n);             // 1.234.567
uint64_t parseSizeString(const std::string& s, uint64_t fallback);  // "500MB" -> byte
bool parseInt64(const std::string& s, int64_t& out);
bool parseUInt64(const std::string& s, uint64_t& out);
std::string toString(int64_t v);
std::string toString(uint64_t v);

// ---- UTF-8 ----------------------------------------------------------------
bool isValidUtf8(const std::string& s);
size_t utf8Length(const std::string& s);
// Cắt chuỗi UTF-8 theo số byte tối đa mà không làm vỡ ký tự.
std::string utf8TruncateBytes(const std::string& s, size_t maxBytes);

// ---- byte <-> string ------------------------------------------------------
inline Bytes toBytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
inline std::string bytesToString(const Bytes& b) { return std::string(b.begin(), b.end()); }

}  // namespace ttd
