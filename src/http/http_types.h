// Kiểu dữ liệu cho máy chủ HTTP: yêu cầu, phản hồi, phân tích tiêu đề.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "common/json.h"
#include "common/strutil.h"

namespace ttd {
namespace http {

// So sánh tên tiêu đề không phân biệt hoa thường.
struct CaseInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const;
};

using Headers = std::map<std::string, std::string, CaseInsensitiveLess>;

struct Request {
    std::string method;
    std::string target;        // nguyên bản, còn nguyên chuỗi truy vấn
    std::string path;          // đã giải mã URL
    std::string rawPath;       // chưa giải mã
    std::string query;
    std::string version = "HTTP/1.1";
    Headers headers;
    std::map<std::string, std::string> params;   // tham số truy vấn
    std::map<std::string, std::string> cookies;
    std::string body;          // với yêu cầu nhỏ đã đọc sẵn
    std::string clientIp;
    bool keepAlive = true;
    uint64_t contentLength = 0;
    bool chunked = false;
    bool bodyStreamed = false;  // thân yêu cầu được xử lý theo luồng, không nằm trong `body`

    std::string header(const std::string& name, const std::string& def = "") const;
    std::string param(const std::string& name, const std::string& def = "") const;
    int64_t paramInt(const std::string& name, int64_t def = 0) const;
    bool paramBool(const std::string& name, bool def = false) const;
    std::string cookie(const std::string& name, const std::string& def = "") const;
    Json json() const;
};

struct Response {
    int status = 200;
    std::string statusText;
    Headers headers;
    std::string body;
    // Khi cần gửi nội dung lớn theo luồng, dùng hàm này thay cho `body`.
    // Trả về false để dừng sớm.
    std::function<bool(const std::function<bool(const char*, size_t)>&)> streamBody;
    uint64_t streamLength = 0;   // 0 = dùng chunked encoding
    bool closeConnection = false;

    void setHeader(const std::string& name, const std::string& value) {
        headers[name] = value;
    }
    void setJson(const Json& value, int statusCode = 200);
    void setText(const std::string& text, int statusCode = 200,
                 const std::string& contentType = "text/plain; charset=utf-8");
    void setHtml(const std::string& html, int statusCode = 200);
    void setError(int statusCode, const std::string& message);
    void redirect(const std::string& location, int statusCode = 302);
};

const char* statusText(int code);
std::string urlDecodePath(const std::string& path);
std::map<std::string, std::string> parseQueryString(const std::string& query);
std::map<std::string, std::string> parseCookies(const std::string& header);
// Tạo giá trị Content-Disposition an toàn cho tên tệp tiếng Việt.
std::string contentDisposition(const std::string& fileName, bool inlineDisplay);

}  // namespace http
}  // namespace ttd
