// JSON tối giản nhưng đầy đủ: phân tích, dựng và xuất chuỗi (UTF-8).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ttd {

class Json;
using JsonArray = std::vector<Json>;
// Dùng map có thứ tự theo khoá để kết quả xuất ra ổn định, dễ so sánh/diff.
using JsonObject = std::map<std::string, Json>;

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() : type_(Type::Null) {}
    Json(std::nullptr_t) : type_(Type::Null) {}
    Json(bool v) : type_(Type::Bool), bool_(v) {}
    Json(int v) : type_(Type::Number), num_(v) {}
    Json(int64_t v) : type_(Type::Number), num_(static_cast<double>(v)), int_(v), isInt_(true) {}
    Json(uint64_t v) : type_(Type::Number), num_(static_cast<double>(v)),
                       int_(static_cast<int64_t>(v)), isInt_(true) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char* v) : type_(Type::String), str_(v ? v : "") {}
    Json(std::string v) : type_(Type::String), str_(std::move(v)) {}
    Json(JsonArray v) : type_(Type::Array), arr_(std::move(v)) {}
    Json(JsonObject v) : type_(Type::Object), obj_(std::move(v)) {}

    static Json array() { return Json(JsonArray{}); }
    static Json object() { return Json(JsonObject{}); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const;
    double asDouble(double def = 0) const;
    int64_t asInt64(int64_t def = 0) const;
    uint64_t asUInt64(uint64_t def = 0) const;
    int asInt(int def = 0) const { return static_cast<int>(asInt64(def)); }
    std::string asString(const std::string& def = "") const;

    const JsonArray& arr() const { return arr_; }
    JsonArray& arr() { return arr_; }
    const JsonObject& obj() const { return obj_; }
    JsonObject& obj() { return obj_; }

    size_t size() const;
    bool has(const std::string& key) const;
    const Json& operator[](const std::string& key) const;
    Json& operator[](const std::string& key);
    const Json& operator[](size_t index) const;
    void push(Json v);
    void set(const std::string& key, Json v);
    void remove(const std::string& key);

    // Truy cập theo đường dẫn "a.b.c" (không hỗ trợ chỉ số mảng).
    const Json& at(const std::string& dottedPath) const;

    std::string dump(int indent = -1) const;
    static Json parse(const std::string& text, std::string* error = nullptr);

private:
    void dumpTo(std::string& out, int indent, int depth) const;

    Type type_;
    bool bool_ = false;
    double num_ = 0;
    int64_t int_ = 0;
    bool isInt_ = false;
    std::string str_;
    JsonArray arr_;
    JsonObject obj_;

    static const Json kNull;
};

}  // namespace ttd
