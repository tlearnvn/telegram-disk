// Bộ phân tích tệp schema TL (.tl) của Telegram.
//
// Định danh hàm dựng được tính bằng CRC32 của chuỗi khai báo đã chuẩn hoá
// (bỏ "#id", đổi '<' '>' '{' '}' thành khoảng trắng, gộp khoảng trắng, và
// coi kiểu `bytes` như `string`). Nhờ vậy chỉ cần thay tệp schema là ứng dụng
// chạy được với layer mới, không phải sửa mã C++.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ttd {
namespace tg {

// Mô tả kiểu của một tham số.
struct TlType {
    std::string name;         // "int", "long", "string", "bytes", "Vector", "InputFile"...
    bool bare = false;        // kiểu trần (%Type hoặc vector viết thường)
    bool isVector = false;    // Vector<T> / vector<T>
    bool boxedVector = false; // true = Vector<T> (có ID 0x1cb5c415)
    bool isGeneric = false;   // !X — nội dung là một đối tượng bất kỳ
    std::shared_ptr<TlType> item;  // kiểu phần tử khi isVector
};

struct TlArg {
    std::string name;
    TlType type;
    // Trường điều kiện: cờ nằm trong tham số `flagsField`, tại bit `flagBit`.
    bool conditional = false;
    std::string flagsField;
    int flagBit = 0;
    bool isFlagsInt = false;  // chính là trường `flags:#`
    bool isTrueFlag = false;  // kiểu `true` — chỉ đánh dấu, không có dữ liệu
};

struct TlConstructor {
    std::string name;
    uint32_t id = 0;
    std::string resultType;
    std::vector<TlArg> args;
    bool isFunction = false;
    std::string declaration;  // dòng gốc, để tra cứu khi gỡ lỗi
};

class TlSchema {
public:
    // Nạp thêm nội dung một tệp schema. Có thể gọi nhiều lần (mtproto.tl + api.tl).
    // Trả về số hàm dựng đã nạp; ghi cảnh báo vào `warnings`.
    size_t load(const std::string& text, std::vector<std::string>* warnings = nullptr);

    const TlConstructor* byName(const std::string& name) const;
    const TlConstructor* byId(uint32_t id) const;
    // Kiểu chỉ có đúng một hàm dựng (cần cho kiểu trần %Type).
    const TlConstructor* soleConstructorOfType(const std::string& typeName) const;

    size_t size() const { return byName_.size(); }
    std::vector<std::string> constructorNames() const;

    // Tính định danh TL từ một dòng khai báo (đã bỏ phần "#id").
    static uint32_t computeId(const std::string& declaration);
    // Chuẩn hoá dòng khai báo theo quy tắc tính CRC.
    static std::string normalizeDeclaration(const std::string& declaration);

    // Schema mặc định được nhúng sẵn trong tệp thực thi.
    static const char* builtinMtprotoSchema();
    static const char* builtinApiSchema();
    // Số hiệu layer khai báo trong tệp api.tl (dòng "// LAYER n").
    int layer() const { return layer_; }
    void setLayer(int v) { layer_ = v; }

private:
    bool parseLine(const std::string& line, bool isFunction, std::vector<std::string>* warnings);

    std::unordered_map<std::string, std::shared_ptr<TlConstructor>> byName_;
    std::unordered_map<uint32_t, std::shared_ptr<TlConstructor>> byId_;
    std::multimap<std::string, std::shared_ptr<TlConstructor>> byType_;
    int layer_ = 0;
};

}  // namespace tg
}  // namespace ttd
