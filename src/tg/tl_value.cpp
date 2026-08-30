#include "tg/tl_value.h"

#include <cstdio>

namespace ttd {
namespace tg {

const TlValue TlValue::kNull;

TlValue TlValue::makeInt(int32_t v) {
    TlValue t;
    t.kind_ = Kind::Int;
    t.num_ = v;
    return t;
}

TlValue TlValue::makeLong(int64_t v) {
    TlValue t;
    t.kind_ = Kind::Long;
    t.num_ = v;
    return t;
}

TlValue TlValue::makeDouble(double v) {
    TlValue t;
    t.kind_ = Kind::Double;
    t.dbl_ = v;
    return t;
}

TlValue TlValue::makeBytes(Bytes v) {
    TlValue t;
    t.kind_ = Kind::Bytes;
    t.bytes_ = std::move(v);
    return t;
}

TlValue TlValue::makeBytes(const std::string& v) {
    return makeBytes(Bytes(v.begin(), v.end()));
}

TlValue TlValue::makeBool(bool v) {
    TlValue t;
    t.kind_ = Kind::Bool;
    t.num_ = v ? 1 : 0;
    return t;
}

TlValue TlValue::makeFlagTrue() { return makeBool(true); }

TlValue TlValue::makeVector(TlVector v) {
    TlValue t;
    t.kind_ = Kind::Vector;
    t.vec_ = std::move(v);
    return t;
}

TlValue TlValue::makeObject(std::string ctorName) {
    TlValue t;
    t.kind_ = Kind::Object;
    t.ctor_ = std::move(ctorName);
    t.fields_ = std::make_shared<std::vector<TlField>>();
    return t;
}

int32_t TlValue::asInt(int32_t def) const {
    switch (kind_) {
        case Kind::Int:
        case Kind::Long:
        case Kind::Bool: return static_cast<int32_t>(num_);
        case Kind::Double: return static_cast<int32_t>(dbl_);
        default: return def;
    }
}

int64_t TlValue::asLong(int64_t def) const {
    switch (kind_) {
        case Kind::Int:
        case Kind::Long:
        case Kind::Bool: return num_;
        case Kind::Double: return static_cast<int64_t>(dbl_);
        default: return def;
    }
}

double TlValue::asDouble(double def) const {
    if (kind_ == Kind::Double) return dbl_;
    if (kind_ == Kind::Int || kind_ == Kind::Long) return static_cast<double>(num_);
    return def;
}

bool TlValue::asBool(bool def) const {
    switch (kind_) {
        case Kind::Bool: return num_ != 0;
        case Kind::Int:
        case Kind::Long: return num_ != 0;
        case Kind::Object: return ctor_ == "boolTrue";
        case Kind::Null: return def;
        default: return def;
    }
}

const Bytes& TlValue::asBytes() const {
    static const Bytes kEmpty;
    return kind_ == Kind::Bytes ? bytes_ : kEmpty;
}

std::string TlValue::asString() const {
    if (kind_ == Kind::Bytes) return std::string(bytes_.begin(), bytes_.end());
    if (kind_ == Kind::Int || kind_ == Kind::Long) return std::to_string(num_);
    return "";
}

void TlValue::ensureFields() {
    if (!fields_) fields_ = std::make_shared<std::vector<TlField>>();
    kind_ = Kind::Object;
}

bool TlValue::has(const std::string& field) const {
    if (kind_ != Kind::Object || !fields_) return false;
    for (const auto& f : *fields_)
        if (f.name == field) return !f.value.isNull();
    return false;
}

const TlValue& TlValue::operator[](const std::string& field) const {
    if (kind_ != Kind::Object || !fields_) return kNull;
    for (const auto& f : *fields_)
        if (f.name == field) return f.value;
    return kNull;
}

const TlValue& TlValue::operator[](size_t index) const {
    if (kind_ != Kind::Vector || index >= vec_.size()) return kNull;
    return vec_[index];
}

void TlValue::set(const std::string& field, TlValue v) {
    ensureFields();
    for (auto& f : *fields_) {
        if (f.name == field) {
            f.value = std::move(v);
            return;
        }
    }
    fields_->push_back(TlField{field, std::move(v)});
}

void TlValue::push(TlValue v) {
    if (kind_ != Kind::Vector) {
        kind_ = Kind::Vector;
        vec_.clear();
    }
    vec_.push_back(std::move(v));
}

std::string TlValue::describe(int maxDepth) const {
    switch (kind_) {
        case Kind::Null: return "null";
        case Kind::Int: return std::to_string(static_cast<int32_t>(num_));
        case Kind::Long: return std::to_string(num_) + "L";
        case Kind::Double: {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%g", dbl_);
            return buf;
        }
        case Kind::Bool: return num_ ? "true" : "false";
        case Kind::Bytes: {
            // In dạng văn bản nếu là chuỗi UTF-8 ngắn, ngược lại in hex rút gọn.
            std::string s(bytes_.begin(), bytes_.end());
            if (bytes_.size() <= 64 && isValidUtf8(s)) {
                bool printable = true;
                for (uint8_t c : bytes_)
                    if (c < 0x20 && c != '\n' && c != '\t') printable = false;
                if (printable) return "\"" + s + "\"";
            }
            std::string hex = toHex(bytes_);
            if (hex.size() > 48) hex = hex.substr(0, 48) + "…";
            return "<" + std::to_string(bytes_.size()) + "B " + hex + ">";
        }
        case Kind::Vector: {
            if (maxDepth <= 0) return "[…" + std::to_string(vec_.size()) + "]";
            std::string out = "[";
            for (size_t i = 0; i < vec_.size() && i < 8; ++i) {
                if (i) out += ", ";
                out += vec_[i].describe(maxDepth - 1);
            }
            if (vec_.size() > 8) out += ", …(" + std::to_string(vec_.size()) + ")";
            out += "]";
            return out;
        }
        case Kind::Object: {
            if (maxDepth <= 0) return ctor_ + "{…}";
            std::string out = ctor_ + "{";
            bool first = true;
            if (fields_) {
                for (const auto& f : *fields_) {
                    if (f.value.isNull()) continue;
                    if (!first) out += ", ";
                    first = false;
                    out += f.name + "=" + f.value.describe(maxDepth - 1);
                }
            }
            out += "}";
            return out;
        }
    }
    return "?";
}

}  // namespace tg
}  // namespace ttd
