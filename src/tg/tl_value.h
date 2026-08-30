// Giá trị TL động — biểu diễn mọi kiểu dữ liệu trong giao thức MTProto.
// Cách làm này giúp toàn bộ tầng Telegram chạy theo tệp schema (.tl) thay vì
// mã nguồn sinh sẵn, nên khi Telegram nâng layer chỉ cần thay tệp schema.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/strutil.h"

namespace ttd {
namespace tg {

class TlValue;
using TlVector = std::vector<TlValue>;

struct TlField;

class TlValue {
public:
    enum class Kind {
        Null,     // trường vắng mặt / giá trị rỗng
        Int,      // int (32-bit)
        Long,     // long (64-bit)
        Double,   // double
        Bytes,    // string / bytes / int128 / int256
        Bool,     // Bool hoặc cờ "true"
        Vector,   // Vector<T> hoặc vector<T>
        Object,   // hàm dựng có tên + danh sách trường
    };

    TlValue() = default;

    static TlValue makeInt(int32_t v);
    static TlValue makeLong(int64_t v);
    static TlValue makeDouble(double v);
    static TlValue makeBytes(Bytes v);
    static TlValue makeBytes(const std::string& v);
    static TlValue makeBool(bool v);
    static TlValue makeVector(TlVector v);
    static TlValue makeObject(std::string ctorName);
    static TlValue makeFlagTrue();  // giá trị cho trường kiểu `flags.N?true`

    Kind kind() const { return kind_; }
    bool isNull() const { return kind_ == Kind::Null; }
    bool isObject() const { return kind_ == Kind::Object; }
    bool isVector() const { return kind_ == Kind::Vector; }

    int32_t asInt(int32_t def = 0) const;
    int64_t asLong(int64_t def = 0) const;
    double asDouble(double def = 0) const;
    bool asBool(bool def = false) const;
    const Bytes& asBytes() const;
    std::string asString() const;
    const TlVector& asVector() const { return vec_; }
    TlVector& vectorRef() { return vec_; }

    const std::string& ctorName() const { return ctor_; }
    void setCtorName(std::string name) { ctor_ = std::move(name); kind_ = Kind::Object; }
    bool is(const std::string& name) const { return kind_ == Kind::Object && ctor_ == name; }

    // Truy cập trường của đối tượng.
    bool has(const std::string& field) const;
    const TlValue& operator[](const std::string& field) const;
    const TlValue& operator[](size_t index) const;
    void set(const std::string& field, TlValue v);
    void setInt(const std::string& field, int32_t v) { set(field, makeInt(v)); }
    void setLong(const std::string& field, int64_t v) { set(field, makeLong(v)); }
    void setBytes(const std::string& field, Bytes v) { set(field, makeBytes(std::move(v))); }
    void setBytes(const std::string& field, const std::string& v) { set(field, makeBytes(v)); }
    void setBool(const std::string& field, bool v) { set(field, makeBool(v)); }
    void setFlag(const std::string& field) { set(field, makeFlagTrue()); }
    void setVector(const std::string& field, TlVector v) { set(field, makeVector(std::move(v))); }

    const std::vector<TlField>& fields() const { return *fields_; }
    void push(TlValue v);

    // Mô tả ngắn gọn để ghi nhật ký (cắt bớt dữ liệu nhị phân dài).
    std::string describe(int maxDepth = 3) const;

private:
    void ensureFields();

    Kind kind_ = Kind::Null;
    int64_t num_ = 0;
    double dbl_ = 0;
    Bytes bytes_;
    TlVector vec_;
    std::string ctor_;
    std::shared_ptr<std::vector<TlField>> fields_;

    static const TlValue kNull;
};

struct TlField {
    std::string name;
    TlValue value;
};

}  // namespace tg
}  // namespace ttd
