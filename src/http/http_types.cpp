#include "http/http_types.h"

#include <cstring>

namespace ttd {
namespace http {

bool CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return ca < cb;
    }
    return a.size() < b.size();
}

std::string Request::header(const std::string& name, const std::string& def) const {
    auto it = headers.find(name);
    return it == headers.end() ? def : it->second;
}

std::string Request::param(const std::string& name, const std::string& def) const {
    auto it = params.find(name);
    return it == params.end() ? def : it->second;
}

int64_t Request::paramInt(const std::string& name, int64_t def) const {
    auto it = params.find(name);
    if (it == params.end()) return def;
    int64_t v;
    return parseInt64(it->second, v) ? v : def;
}

bool Request::paramBool(const std::string& name, bool def) const {
    auto it = params.find(name);
    if (it == params.end()) return def;
    std::string v = toLower(it->second);
    if (v == "1" || v == "true" || v == "yes" || v == "on" || v.empty()) return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

std::string Request::cookie(const std::string& name, const std::string& def) const {
    auto it = cookies.find(name);
    return it == cookies.end() ? def : it->second;
}

Json Request::json() const {
    std::string error;
    Json value = Json::parse(body, &error);
    return value;
}

void Response::setJson(const Json& value, int statusCode) {
    status = statusCode;
    body = value.dump();
    headers["Content-Type"] = "application/json; charset=utf-8";
}

void Response::setText(const std::string& text, int statusCode,
                       const std::string& contentType) {
    status = statusCode;
    body = text;
    headers["Content-Type"] = contentType;
}

void Response::setHtml(const std::string& html, int statusCode) {
    setText(html, statusCode, "text/html; charset=utf-8");
}

void Response::setError(int statusCode, const std::string& message) {
    Json j = Json::object();
    j.set("ok", false);
    j.set("error", message);
    setJson(j, statusCode);
}

void Response::redirect(const std::string& location, int statusCode) {
    status = statusCode;
    headers["Location"] = location;
    body.clear();
}

const char* statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 412: return "Precondition Failed";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 422: return "Unprocessable Entity";
        case 423: return "Locked";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 507: return "Insufficient Storage";
        default: return "Unknown";
    }
}

std::string urlDecodePath(const std::string& path) { return urlDecode(path); }

std::map<std::string, std::string> parseQueryString(const std::string& query) {
    std::map<std::string, std::string> out;
    for (const auto& pair : split(query, '&', false)) {
        if (pair.empty()) continue;
        size_t eq = pair.find('=');
        if (eq == std::string::npos) {
            out[urlDecode(pair)] = "";
        } else {
            out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
        }
    }
    return out;
}

std::map<std::string, std::string> parseCookies(const std::string& header) {
    std::map<std::string, std::string> out;
    for (const auto& part : split(header, ';', false)) {
        std::string p = trim(part);
        if (p.empty()) continue;
        size_t eq = p.find('=');
        if (eq == std::string::npos) continue;
        out[trim(p.substr(0, eq))] = trim(p.substr(eq + 1));
    }
    return out;
}

std::string contentDisposition(const std::string& fileName, bool inlineDisplay) {
    // Bản ASCII dự phòng cho trình duyệt cũ + bản UTF-8 theo RFC 5987.
    std::string ascii;
    for (unsigned char c : fileName) {
        if (c < 0x20 || c > 0x7e || c == '"' || c == '\\')
            ascii.push_back('_');
        else
            ascii.push_back(static_cast<char>(c));
    }
    if (ascii.empty()) ascii = "tep";
    std::string out = inlineDisplay ? "inline" : "attachment";
    out += "; filename=\"" + ascii + "\"";
    out += "; filename*=UTF-8''" + urlEncode(fileName);
    return out;
}

}  // namespace http
}  // namespace ttd
