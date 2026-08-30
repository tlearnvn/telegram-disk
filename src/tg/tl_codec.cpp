#include "tg/tl_codec.h"

#include <cstring>

#include "common/logging.h"

namespace ttd {
namespace tg {

// ---------------------------------------------------------------------------
//  TlWriter
// ---------------------------------------------------------------------------
void TlWriter::writeInt(int32_t v) { writeUInt(static_cast<uint32_t>(v)); }

void TlWriter::writeUInt(uint32_t v) {
    buf_.push_back(static_cast<uint8_t>(v & 0xff));
    buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    buf_.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    buf_.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

void TlWriter::writeLong(int64_t v) {
    uint64_t u = static_cast<uint64_t>(v);
    for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xff));
}

void TlWriter::writeDouble(double v) {
    uint64_t u;
    std::memcpy(&u, &v, 8);
    for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xff));
}

void TlWriter::writeRaw(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
}

void TlWriter::writeBytes(const uint8_t* data, size_t len) {
    size_t start = buf_.size();
    if (len < 254) {
        buf_.push_back(static_cast<uint8_t>(len));
    } else {
        buf_.push_back(254);
        buf_.push_back(static_cast<uint8_t>(len & 0xff));
        buf_.push_back(static_cast<uint8_t>((len >> 8) & 0xff));
        buf_.push_back(static_cast<uint8_t>((len >> 16) & 0xff));
    }
    buf_.insert(buf_.end(), data, data + len);
    while ((buf_.size() - start) % 4 != 0) buf_.push_back(0);
}

void TlWriter::writeString(const std::string& s) {
    writeBytes(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ---------------------------------------------------------------------------
//  TlReader
// ---------------------------------------------------------------------------
bool TlReader::readUInt(uint32_t& out) {
    if (remaining() < 4) {
        fail("Hết dữ liệu khi đọc int");
        return false;
    }
    out = static_cast<uint32_t>(data_[pos_]) | (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
          (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
          (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return true;
}

bool TlReader::readInt(int32_t& out) {
    uint32_t u;
    if (!readUInt(u)) return false;
    out = static_cast<int32_t>(u);
    return true;
}

bool TlReader::readLong(int64_t& out) {
    if (remaining() < 8) {
        fail("Hết dữ liệu khi đọc long");
        return false;
    }
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u |= static_cast<uint64_t>(data_[pos_ + static_cast<size_t>(i)])
                                     << (8 * i);
    pos_ += 8;
    out = static_cast<int64_t>(u);
    return true;
}

bool TlReader::readDouble(double& out) {
    int64_t v;
    if (!readLong(v)) return false;
    uint64_t u = static_cast<uint64_t>(v);
    std::memcpy(&out, &u, 8);
    return true;
}

bool TlReader::readRaw(size_t n, Bytes& out) {
    if (remaining() < n) {
        fail("Hết dữ liệu khi đọc " + std::to_string(n) + " byte");
        return false;
    }
    out.assign(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return true;
}

bool TlReader::skip(size_t n) {
    if (remaining() < n) {
        fail("Hết dữ liệu khi bỏ qua");
        return false;
    }
    pos_ += n;
    return true;
}

bool TlReader::readBytes(Bytes& out) {
    if (remaining() < 1) {
        fail("Hết dữ liệu khi đọc chuỗi");
        return false;
    }
    size_t start = pos_;
    uint8_t first = data_[pos_++];
    size_t len;
    if (first < 254) {
        len = first;
    } else {
        if (remaining() < 3) {
            fail("Hết dữ liệu khi đọc độ dài chuỗi dài");
            return false;
        }
        len = static_cast<size_t>(data_[pos_]) | (static_cast<size_t>(data_[pos_ + 1]) << 8) |
              (static_cast<size_t>(data_[pos_ + 2]) << 16);
        pos_ += 3;
    }
    if (remaining() < len) {
        fail("Chuỗi dài hơn dữ liệu còn lại");
        return false;
    }
    out.assign(data_ + pos_, data_ + pos_ + len);
    pos_ += len;
    while ((pos_ - start) % 4 != 0) {
        if (pos_ >= len_) break;
        ++pos_;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  TlCodec — ghi
// ---------------------------------------------------------------------------
bool TlCodec::writeTyped(const TlType& type, const TlValue& v, TlWriter& out,
                         std::string& error) const {
    if (type.isVector) {
        if (type.boxedVector) out.writeUInt(ctor::kVector);
        const TlVector& items = v.asVector();
        out.writeUInt(static_cast<uint32_t>(items.size()));
        for (const auto& item : items) {
            if (!writeTyped(*type.item, item, out, error)) return false;
        }
        return true;
    }

    const std::string& n = type.name;
    if (n == "int") {
        out.writeInt(v.asInt());
        return true;
    }
    if (n == "long") {
        out.writeLong(v.asLong());
        return true;
    }
    if (n == "double") {
        out.writeDouble(v.asDouble());
        return true;
    }
    if (n == "string" || n == "bytes") {
        out.writeBytes(v.asBytes());
        return true;
    }
    if (n == "int128") {
        const Bytes& b = v.asBytes();
        if (b.size() != 16) {
            error = "int128 phải đúng 16 byte";
            return false;
        }
        out.writeRaw(b);
        return true;
    }
    if (n == "int256") {
        const Bytes& b = v.asBytes();
        if (b.size() != 32) {
            error = "int256 phải đúng 32 byte";
            return false;
        }
        out.writeRaw(b);
        return true;
    }
    if (n == "Bool") {
        out.writeUInt(v.asBool() ? ctor::kBoolTrue : ctor::kBoolFalse);
        return true;
    }
    if (n == "true") {
        return true;  // không có dữ liệu
    }

    // Kiểu trần: bỏ ID hàm dựng.
    if (type.bare) {
        const TlConstructor* c = nullptr;
        if (v.isObject() && !v.ctorName().empty()) c = schema_.byName(v.ctorName());
        if (!c) c = schema_.soleConstructorOfType(n);
        if (!c) {
            error = "Không xác định được hàm dựng trần cho kiểu " + n;
            return false;
        }
        TlValue tmp = v;
        // Ghi trực tiếp các trường, không có ID.
        TlWriter sub;
        if (!serialize(v, sub, error)) return false;
        // Bỏ 4 byte ID ở đầu.
        if (sub.size() < 4) {
            error = "Dữ liệu trần quá ngắn";
            return false;
        }
        out.writeRaw(sub.buffer().data() + 4, sub.size() - 4);
        return true;
    }

    // Kiểu boxed thông thường (hoặc generic !X).
    return serialize(v, out, error);
}

bool TlCodec::writeArg(const TlArg& arg, const TlValue& v, TlWriter& out,
                       std::string& error) const {
    if (arg.isTrueFlag) return true;
    return writeTyped(arg.type, v, out, error);
}

bool TlCodec::serialize(const TlValue& value, TlWriter& out, std::string& error) const {
    if (!value.isObject()) {
        error = "Chỉ ghi được đối tượng TL, nhận: " + value.describe(1);
        return false;
    }
    const TlConstructor* ctorDef = schema_.byName(value.ctorName());
    if (!ctorDef) {
        error = "Không tìm thấy hàm dựng '" + value.ctorName() + "' trong schema";
        return false;
    }

    out.writeUInt(ctorDef->id);

    // Tính giá trị các trường cờ.
    std::vector<std::pair<std::string, uint32_t>> flagValues;
    for (const auto& arg : ctorDef->args) {
        if (!arg.isFlagsInt) continue;
        uint32_t flags = 0;
        for (const auto& other : ctorDef->args) {
            if (!other.conditional || other.flagsField != arg.name) continue;
            const TlValue& fv = value[other.name];
            bool present;
            if (other.isTrueFlag) {
                present = fv.asBool(false);
            } else {
                present = !fv.isNull();
            }
            if (present) flags |= (1u << other.flagBit);
        }
        // Cho phép người gọi ép thêm bit thủ công.
        const TlValue& explicitFlags = value[arg.name];
        if (!explicitFlags.isNull()) flags |= static_cast<uint32_t>(explicitFlags.asInt());
        flagValues.emplace_back(arg.name, flags);
    }

    auto flagOf = [&](const std::string& name) -> uint32_t {
        for (const auto& kv : flagValues)
            if (kv.first == name) return kv.second;
        return 0;
    };

    for (const auto& arg : ctorDef->args) {
        if (arg.isFlagsInt) {
            out.writeUInt(flagOf(arg.name));
            continue;
        }
        if (arg.conditional) {
            uint32_t flags = flagOf(arg.flagsField);
            if (!(flags & (1u << arg.flagBit))) continue;
        }
        const TlValue& fv = value[arg.name];
        if (fv.isNull() && !arg.isTrueFlag && !arg.conditional) {
            // Cho phép trường vắng với kiểu đơn giản — ghi giá trị 0/rỗng.
            TlValue empty;
            if (arg.type.name == "int" || arg.type.name == "long" ||
                arg.type.name == "double" || arg.type.name == "string" ||
                arg.type.name == "bytes" || arg.type.name == "Bool" || arg.type.isVector) {
                if (!writeArg(arg, empty, out, error)) return false;
                continue;
            }
            error = "Thiếu trường bắt buộc '" + arg.name + "' của " + ctorDef->name;
            return false;
        }
        if (!writeArg(arg, fv, out, error)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  TlCodec — đọc
// ---------------------------------------------------------------------------
bool TlCodec::readTyped(const TlType& type, TlReader& in, TlValue& out,
                        std::string& error) const {
    if (type.isVector) {
        if (type.boxedVector) {
            uint32_t id;
            if (!in.readUInt(id)) {
                error = in.error();
                return false;
            }
            if (id != ctor::kVector) {
                // Một số trường trả về Vector đã bị bọc trong gzip hoặc kiểu khác.
                error = "Chờ Vector nhưng nhận ID 0x" + toHex(reinterpret_cast<uint8_t*>(&id), 4);
                return false;
            }
        }
        uint32_t count;
        if (!in.readUInt(count)) {
            error = in.error();
            return false;
        }
        if (count > 10000000u) {
            error = "Số phần tử vector không hợp lý";
            return false;
        }
        TlVector items;
        items.reserve(count < 1024 ? count : 1024);
        for (uint32_t i = 0; i < count; ++i) {
            TlValue item;
            if (!readTyped(*type.item, in, item, error)) {
                if (!item.isNull()) items.push_back(std::move(item));
                out = TlValue::makeVector(std::move(items));
                return false;
            }
            items.push_back(std::move(item));
        }
        out = TlValue::makeVector(std::move(items));
        return true;
    }

    const std::string& n = type.name;
    if (n == "int") {
        int32_t v;
        if (!in.readInt(v)) { error = in.error(); return false; }
        out = TlValue::makeInt(v);
        return true;
    }
    if (n == "long") {
        int64_t v;
        if (!in.readLong(v)) { error = in.error(); return false; }
        out = TlValue::makeLong(v);
        return true;
    }
    if (n == "double") {
        double v;
        if (!in.readDouble(v)) { error = in.error(); return false; }
        out = TlValue::makeDouble(v);
        return true;
    }
    if (n == "string" || n == "bytes") {
        Bytes b;
        if (!in.readBytes(b)) { error = in.error(); return false; }
        out = TlValue::makeBytes(std::move(b));
        return true;
    }
    if (n == "int128") {
        Bytes b;
        if (!in.readRaw(16, b)) { error = in.error(); return false; }
        out = TlValue::makeBytes(std::move(b));
        return true;
    }
    if (n == "int256") {
        Bytes b;
        if (!in.readRaw(32, b)) { error = in.error(); return false; }
        out = TlValue::makeBytes(std::move(b));
        return true;
    }
    if (n == "Bool") {
        uint32_t id;
        if (!in.readUInt(id)) { error = in.error(); return false; }
        out = TlValue::makeBool(id == ctor::kBoolTrue);
        return true;
    }
    if (n == "true") {
        out = TlValue::makeBool(true);
        return true;
    }

    if (type.bare) {
        const TlConstructor* c = schema_.soleConstructorOfType(n);
        if (!c) {
            error = "Không tìm thấy hàm dựng trần duy nhất cho kiểu " + n;
            return false;
        }
        return deserializeByCtor(in, *c, out, error);
    }

    return deserialize(in, out, error);
}

bool TlCodec::deserializeByCtor(TlReader& in, const TlConstructor& ctorDef, TlValue& out,
                                std::string& error) const {
    TlValue obj = TlValue::makeObject(ctorDef.name);
    std::vector<std::pair<std::string, uint32_t>> flagValues;

    for (const auto& arg : ctorDef.args) {
        if (arg.isFlagsInt) {
            uint32_t flags;
            if (!in.readUInt(flags)) {
                error = in.error();
                obj.set("_partial", TlValue::makeBool(true));
                out = std::move(obj);
                return false;
            }
            flagValues.emplace_back(arg.name, flags);
            obj.set(arg.name, TlValue::makeInt(static_cast<int32_t>(flags)));
            continue;
        }
        if (arg.conditional) {
            uint32_t flags = 0;
            for (const auto& kv : flagValues)
                if (kv.first == arg.flagsField) flags = kv.second;
            if (!(flags & (1u << arg.flagBit))) continue;
            if (arg.isTrueFlag) {
                obj.set(arg.name, TlValue::makeBool(true));
                continue;
            }
        } else if (arg.isTrueFlag) {
            obj.set(arg.name, TlValue::makeBool(true));
            continue;
        }
        TlValue v;
        if (!readTyped(arg.type, in, v, error)) {
            // Giải mã một phần: giữ lại những trường đã đọc được. Nhờ vậy khi
            // schema hơi lệch so với máy chủ, các trường quan trọng nằm phía
            // trước (id, access_hash, file_reference…) vẫn dùng được.
            if (!v.isNull()) obj.set(arg.name, std::move(v));
            obj.set("_partial", TlValue::makeBool(true));
            obj.set("_error", TlValue::makeBytes(error));
            error = ctorDef.name + "." + arg.name + ": " + error;
            out = std::move(obj);
            return false;
        }
        obj.set(arg.name, std::move(v));
    }
    out = std::move(obj);
    return true;
}

const TlValue* findFirstObject(const TlValue& root, const std::string& ctorName, int maxDepth) {
    if (maxDepth < 0) return nullptr;
    if (root.isObject()) {
        if (root.ctorName() == ctorName) return &root;
        for (const auto& f : root.fields()) {
            const TlValue* r = findFirstObject(f.value, ctorName, maxDepth - 1);
            if (r) return r;
        }
    } else if (root.isVector()) {
        for (const auto& item : root.asVector()) {
            const TlValue* r = findFirstObject(item, ctorName, maxDepth - 1);
            if (r) return r;
        }
    }
    return nullptr;
}

void collectObjects(const TlValue& root, const std::string& ctorName,
                    std::vector<const TlValue*>& out, int maxDepth) {
    if (maxDepth < 0) return;
    if (root.isObject()) {
        if (root.ctorName() == ctorName) out.push_back(&root);
        for (const auto& f : root.fields()) collectObjects(f.value, ctorName, out, maxDepth - 1);
    } else if (root.isVector()) {
        for (const auto& item : root.asVector()) collectObjects(item, ctorName, out, maxDepth - 1);
    }
}

bool TlCodec::deserialize(TlReader& in, TlValue& out, std::string& error) const {
    uint32_t id;
    if (!in.readUInt(id)) {
        error = in.error();
        return false;
    }
    if (id == ctor::kBoolTrue) {
        out = TlValue::makeBool(true);
        return true;
    }
    if (id == ctor::kBoolFalse) {
        out = TlValue::makeBool(false);
        return true;
    }
    if (id == ctor::kVector) {
        // Vector không rõ kiểu phần tử: đọc như vector các đối tượng boxed.
        uint32_t count;
        if (!in.readUInt(count)) {
            error = in.error();
            return false;
        }
        TlVector items;
        for (uint32_t i = 0; i < count; ++i) {
            TlValue item;
            if (!deserialize(in, item, error)) return false;
            items.push_back(std::move(item));
        }
        out = TlValue::makeVector(std::move(items));
        return true;
    }

    const TlConstructor* c = schema_.byId(id);
    if (!c) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "0x%08x", id);
        error = std::string("Hàm dựng lạ ") + buf +
                " — schema có thể đã cũ so với layer của máy chủ";
        return false;
    }
    return deserializeByCtor(in, *c, out, error);
}

}  // namespace tg
}  // namespace ttd
