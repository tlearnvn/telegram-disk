#include "common/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/strutil.h"

namespace ttd {

const Json Json::kNull;

bool Json::asBool(bool def) const {
    switch (type_) {
        case Type::Bool: return bool_;
        case Type::Number: return num_ != 0;
        case Type::String: return str_ == "true" || str_ == "1" || str_ == "on" || str_ == "yes";
        case Type::Null: return def;
        default: return def;
    }
}

double Json::asDouble(double def) const {
    if (type_ == Type::Number) return isInt_ ? static_cast<double>(int_) : num_;
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    if (type_ == Type::String) {
        try {
            return std::stod(str_);
        } catch (...) {
            return def;
        }
    }
    return def;
}

int64_t Json::asInt64(int64_t def) const {
    if (type_ == Type::Number) {
        if (isInt_) return int_;
        if (std::isnan(num_) || std::isinf(num_)) return def;
        return static_cast<int64_t>(num_);
    }
    if (type_ == Type::Bool) return bool_ ? 1 : 0;
    if (type_ == Type::String) {
        int64_t v;
        if (parseInt64(str_, v)) return v;
        return def;
    }
    return def;
}

uint64_t Json::asUInt64(uint64_t def) const {
    if (type_ == Type::Number) {
        if (isInt_) return int_ < 0 ? def : static_cast<uint64_t>(int_);
        if (num_ < 0 || std::isnan(num_) || std::isinf(num_)) return def;
        return static_cast<uint64_t>(num_);
    }
    if (type_ == Type::String) {
        uint64_t v;
        if (parseUInt64(str_, v)) return v;
        return def;
    }
    return def;
}

std::string Json::asString(const std::string& def) const {
    switch (type_) {
        case Type::String: return str_;
        case Type::Bool: return bool_ ? "true" : "false";
        case Type::Number: {
            if (isInt_) return std::to_string(int_);
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.17g", num_);
            return buf;
        }
        case Type::Null: return def;
        default: return def;
    }
}

size_t Json::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    if (type_ == Type::String) return str_.size();
    return 0;
}

bool Json::has(const std::string& key) const {
    return type_ == Type::Object && obj_.find(key) != obj_.end();
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return kNull;
    auto it = obj_.find(key);
    return it == obj_.end() ? kNull : it->second;
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        obj_.clear();
    }
    return obj_[key];
}

const Json& Json::operator[](size_t index) const {
    if (type_ != Type::Array || index >= arr_.size()) return kNull;
    return arr_[index];
}

void Json::push(Json v) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        arr_.clear();
    }
    arr_.push_back(std::move(v));
}

void Json::set(const std::string& key, Json v) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        obj_.clear();
    }
    obj_[key] = std::move(v);
}

void Json::remove(const std::string& key) {
    if (type_ == Type::Object) obj_.erase(key);
}

const Json& Json::at(const std::string& dottedPath) const {
    const Json* cur = this;
    size_t start = 0;
    while (start <= dottedPath.size()) {
        size_t dot = dottedPath.find('.', start);
        std::string part = dottedPath.substr(
            start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!cur->isObject()) return kNull;
        auto it = cur->obj_.find(part);
        if (it == cur->obj_.end()) return kNull;
        cur = &it->second;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return *cur;
}

// ---------------------------------------------------------------------------
//  Xuất chuỗi
// ---------------------------------------------------------------------------
static void escapeJsonString(const std::string& s, std::string& out) {
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent >= 0) {
            out.push_back('\n');
            out.append(static_cast<size_t>(indent * d), ' ');
        }
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (isInt_) {
                out += std::to_string(int_);
            } else if (std::isnan(num_) || std::isinf(num_)) {
                out += "null";
            } else {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%.17g", num_);
                // Rút gọn nếu là số nguyên.
                double rt = std::strtod(buf, nullptr);
                if (rt == num_ && num_ == std::floor(num_) && std::fabs(num_) < 1e15) {
                    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(num_));
                }
                out += buf;
            }
            break;
        }
        case Type::String: escapeJsonString(str_, out); break;
        case Type::Array: {
            if (arr_.empty()) { out += "[]"; break; }
            out.push_back('[');
            bool first = true;
            for (const auto& v : arr_) {
                if (!first) out.push_back(',');
                first = false;
                newline(depth + 1);
                v.dumpTo(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back(']');
            break;
        }
        case Type::Object: {
            if (obj_.empty()) { out += "{}"; break; }
            out.push_back('{');
            bool first = true;
            for (const auto& kv : obj_) {
                if (!first) out.push_back(',');
                first = false;
                newline(depth + 1);
                escapeJsonString(kv.first, out);
                out.push_back(':');
                if (indent >= 0) out.push_back(' ');
                kv.second.dumpTo(out, indent, depth + 1);
            }
            newline(depth);
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    out.reserve(256);
    dumpTo(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------------------
//  Phân tích
// ---------------------------------------------------------------------------
namespace {

class Parser {
public:
    Parser(const std::string& t) : s_(t) {}

    bool parse(Json& out) {
        skipWs();
        if (!parseValue(out, 0)) return false;
        skipWs();
        if (pos_ != s_.size()) {
            // Cho phép rác trắng ở cuối; ký tự khác là lỗi.
            error_ = "Còn dữ liệu thừa sau giá trị JSON";
            return false;
        }
        return true;
    }

    std::string error() const { return error_; }

private:
    static constexpr int kMaxDepth = 200;

    void skipWs() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '/') {
                // Chấp nhận chú thích kiểu // để tệp cấu hình dễ đọc.
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;
            } else if (c == '/' && pos_ + 1 < s_.size() && s_[pos_ + 1] == '*') {
                pos_ += 2;
                while (pos_ + 1 < s_.size() && !(s_[pos_] == '*' && s_[pos_ + 1] == '/')) ++pos_;
                pos_ = pos_ + 2 <= s_.size() ? pos_ + 2 : s_.size();
            } else {
                break;
            }
        }
    }

    bool fail(const char* msg) {
        if (error_.empty())
            error_ = std::string(msg) + " tại vị trí " + std::to_string(pos_);
        return false;
    }

    bool parseValue(Json& out, int depth) {
        if (depth > kMaxDepth) return fail("JSON lồng quá sâu");
        skipWs();
        if (pos_ >= s_.size()) return fail("Kết thúc bất ngờ");
        char c = s_[pos_];
        switch (c) {
            case '{': return parseObject(out, depth);
            case '[': return parseArray(out, depth);
            case '"': {
                std::string str;
                if (!parseString(str)) return false;
                out = Json(std::move(str));
                return true;
            }
            case 't':
                if (s_.compare(pos_, 4, "true") == 0) { pos_ += 4; out = Json(true); return true; }
                return fail("Giá trị không hợp lệ");
            case 'f':
                if (s_.compare(pos_, 5, "false") == 0) { pos_ += 5; out = Json(false); return true; }
                return fail("Giá trị không hợp lệ");
            case 'n':
                if (s_.compare(pos_, 4, "null") == 0) { pos_ += 4; out = Json(); return true; }
                return fail("Giá trị không hợp lệ");
            default:
                return parseNumber(out);
        }
    }

    bool parseObject(Json& out, int depth) {
        ++pos_;  // '{'
        JsonObject obj;
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; out = Json(std::move(obj)); return true; }
        while (true) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail("Cần tên khoá dạng chuỗi");
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("Thiếu dấu ':'");
            ++pos_;
            Json v;
            if (!parseValue(v, depth + 1)) return false;
            obj[std::move(key)] = std::move(v);
            skipWs();
            if (pos_ >= s_.size()) return fail("Thiếu '}'");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; break; }
            return fail("Cần ',' hoặc '}'");
        }
        out = Json(std::move(obj));
        return true;
    }

    bool parseArray(Json& out, int depth) {
        ++pos_;  // '['
        JsonArray arr;
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; out = Json(std::move(arr)); return true; }
        while (true) {
            Json v;
            if (!parseValue(v, depth + 1)) return false;
            arr.push_back(std::move(v));
            skipWs();
            if (pos_ >= s_.size()) return fail("Thiếu ']'");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; break; }
            return fail("Cần ',' hoặc ']'");
        }
        out = Json(std::move(arr));
        return true;
    }

    void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool readHex4(uint32_t& v) {
        if (pos_ + 4 > s_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_ + static_cast<size_t>(i)];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return false;
            v = (v << 4) | static_cast<uint32_t>(d);
        }
        pos_ += 4;
        return true;
    }

    bool parseString(std::string& out) {
        ++pos_;  // '"'
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == '"') { ++pos_; return true; }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= s_.size()) return fail("Chuỗi chưa đóng");
                char e = s_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp;
                        if (!readHex4(cp)) return fail("Escape \\u sai");
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() &&
                            s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            size_t save = pos_;
                            pos_ += 2;
                            uint32_t lo;
                            if (readHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else {
                                pos_ = save;
                            }
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: return fail("Escape không hợp lệ");
                }
            } else {
                out.push_back(c);
                ++pos_;
            }
        }
        return fail("Chuỗi chưa đóng");
    }

    bool parseNumber(Json& out) {
        size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
        bool isInt = true;
        bool any = false;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') { ++pos_; any = true; }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            isInt = false;
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') { ++pos_; any = true; }
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            isInt = false;
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        }
        if (!any) return fail("Số không hợp lệ");
        std::string num = s_.substr(start, pos_ - start);
        if (isInt) {
            errno = 0;
            char* end = nullptr;
            long long v = std::strtoll(num.c_str(), &end, 10);
            if (errno == 0 && end && *end == '\0') {
                out = Json(static_cast<int64_t>(v));
                return true;
            }
        }
        out = Json(std::strtod(num.c_str(), nullptr));
        return true;
    }

    const std::string& s_;
    size_t pos_ = 0;
    std::string error_;
};

}  // namespace

Json Json::parse(const std::string& text, std::string* error) {
    Json out;
    Parser p(text);
    if (!p.parse(out)) {
        if (error) *error = p.error();
        return Json();
    }
    if (error) error->clear();
    return out;
}

}  // namespace ttd
