#include "tg/tl_schema.h"

#include <cstdio>
#include <cstdlib>

#include "common/logging.h"
#include "common/strutil.h"
#include "crypto/hash.h"
#include "http/assets.h"

namespace ttd {
namespace tg {

namespace {
constexpr const char* kTag = "tl";

std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Phân tích chuỗi kiểu: "Vector<long>", "vector<%Message>", "!X", "int", "bytes"...
TlType parseType(const std::string& raw) {
    TlType t;
    std::string s = raw;
    if (!s.empty() && s[0] == '!') {
        t.isGeneric = true;
        t.name = s.substr(1);
        return t;
    }
    if (!s.empty() && s[0] == '%') {
        t.bare = true;
        s = s.substr(1);
    }
    size_t lt = s.find('<');
    if (lt != std::string::npos && s.back() == '>') {
        std::string head = s.substr(0, lt);
        std::string inner = s.substr(lt + 1, s.size() - lt - 2);
        t.isVector = true;
        t.name = head;
        t.boxedVector = !head.empty() && head[0] == 'V';  // Vector<T> có ID; vector<T> trần
        t.item = std::make_shared<TlType>(parseType(inner));
        return t;
    }
    t.name = s;
    return t;
}

}  // namespace

namespace {

// Nhận biết token dạng `ten:flags.N?true` hoặc `ten:flags2.N?true`.
bool isTrueFlagToken(const std::string& tok) {
    size_t colon = tok.find(':');
    if (colon == std::string::npos) return false;
    if (!endsWith(tok, "?true")) return false;
    std::string cond = tok.substr(colon + 1, tok.size() - colon - 1 - 5);  // bỏ "?true"
    if (!startsWith(cond, "flags")) return false;
    size_t dot = cond.find('.');
    if (dot == std::string::npos) return false;
    // Phần giữa "flags" và '.' phải là số (hoặc rỗng), phần sau '.' phải là số.
    for (size_t i = 5; i < dot; ++i)
        if (cond[i] < '0' || cond[i] > '9') return false;
    if (dot + 1 >= cond.size()) return false;
    for (size_t i = dot + 1; i < cond.size(); ++i)
        if (cond[i] < '0' || cond[i] > '9') return false;
    return true;
}

}  // namespace

std::string TlSchema::normalizeDeclaration(const std::string& declaration) {
    std::string s = trim(declaration);
    // Bỏ dấu ';' cuối.
    while (!s.empty() && (s.back() == ';' || s.back() == ' ')) s.pop_back();

    // Bước 1: bỏ hẳn các trường điều kiện kiểu `ten:flags.N?true`.
    std::vector<std::string> kept;
    for (const auto& tok : tokenize(s)) {
        if (isTrueFlagToken(tok)) continue;
        kept.push_back(tok);
    }
    std::string joined = join(kept, " ");

    // Bước 2: `bytes` được tính CRC như `string`, nhưng CHỈ khi nó là kiểu trực
    // tiếp của trường (`ten:bytes`, `ten:flags.N?bytes`). Nằm trong tham số kiểu
    // như `Vector<bytes>` thì giữ nguyên — đối chiếu với schema chính thức cho
    // thấy Telegram không đổi ở đó (codeSettings, messages.sendVote,
    // secureValueErrorFiles… đều chỉ khớp khi giữ nguyên).
    // Phải làm TRƯỚC khi bỏ ngoặc, vì bỏ rồi thì không còn phân biệt được.
    std::vector<std::string> pre = tokenize(joined);
    for (auto& tok : pre) {
        if (endsWith(tok, ":bytes") || endsWith(tok, "?bytes"))
            tok = tok.substr(0, tok.size() - 5) + "string";
    }
    joined = join(pre, " ");

    // Bước 3: đổi ngoặc thành khoảng trắng rồi gộp khoảng trắng thừa.
    std::string tmp;
    tmp.reserve(joined.size());
    for (char c : joined) {
        if (c == '<' || c == '>' || c == '{' || c == '}' || c == '\t')
            tmp.push_back(' ');
        else
            tmp.push_back(c);
    }
    return join(tokenize(tmp), " ");
}

uint32_t TlSchema::computeId(const std::string& declaration) {
    std::string norm = normalizeDeclaration(declaration);
    return crypto::crc32(norm);
}

bool TlSchema::parseLine(const std::string& rawLine, bool isFunction,
                         std::vector<std::string>* warnings) {
    std::string line = rawLine;
    // Bỏ chú thích.
    size_t slash = line.find("//");
    if (slash != std::string::npos) line = line.substr(0, slash);
    line = trim(line);
    if (line.empty()) return false;
    while (!line.empty() && line.back() == ';') line.pop_back();
    line = trim(line);
    if (line.empty()) return false;

    size_t eq = line.rfind('=');
    if (eq == std::string::npos) return false;
    std::string lhs = trim(line.substr(0, eq));
    std::string rhs = trim(line.substr(eq + 1));
    if (lhs.empty() || rhs.empty()) return false;

    std::vector<std::string> toks = tokenize(lhs);
    if (toks.empty()) return false;

    auto ctor = std::make_shared<TlConstructor>();
    ctor->isFunction = isFunction;
    ctor->resultType = rhs;
    ctor->declaration = line;

    // Token đầu: tên[#id]
    std::string head = toks[0];
    uint32_t declaredId = 0;
    bool hasDeclaredId = false;
    size_t hash = head.find('#');
    if (hash != std::string::npos) {
        ctor->name = head.substr(0, hash);
        std::string idHex = head.substr(hash + 1);
        declaredId = static_cast<uint32_t>(std::strtoul(idHex.c_str(), nullptr, 16));
        hasDeclaredId = true;
    } else {
        ctor->name = head;
    }
    if (ctor->name.empty()) return false;
    // Bỏ qua khai báo nội bộ của TL.
    if (ctor->name == "vector" || ctor->name == "int" || ctor->name == "long" ||
        ctor->name == "double" || ctor->name == "string" || ctor->name == "bytes" ||
        ctor->name == "int128" || ctor->name == "int256")
        return false;

    // Tính ID từ khai báo (bỏ phần #id).
    std::string forCrc = ctor->name;
    for (size_t i = 1; i < toks.size(); ++i) forCrc += " " + toks[i];
    forCrc += " = " + rhs;
    uint32_t computed = computeId(forCrc);

    if (hasDeclaredId) {
        ctor->id = declaredId;
        // Vài hàm dựng lõi MTProto có định danh do đặc tả ấn định cứng, không
        // suy ra từ CRC32 của khai báo. Chúng luôn "lệch" nên đừng báo động.
        static const char* kAnDinhCung[] = {"msg_container", "msg_copy", "gzip_packed",
                                            "vector", nullptr};
        bool boQuaCanhBao = false;
        for (int i = 0; kAnDinhCung[i]; ++i)
            if (ctor->name == kAnDinhCung[i]) boQuaCanhBao = true;
        if (computed != declaredId && warnings && !boQuaCanhBao) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "%s: ID khai báo 0x%08x khác ID tính được 0x%08x (dùng ID khai báo)",
                          ctor->name.c_str(), declaredId, computed);
            warnings->push_back(buf);
        }
    } else {
        ctor->id = computed;
    }

    // Các tham số.
    for (size_t i = 1; i < toks.size(); ++i) {
        const std::string& tok = toks[i];
        if (tok.empty()) continue;
        if (tok[0] == '{' || tok == "[" || tok == "]" || tok == "#") continue;  // generic/nội bộ
        size_t colon = tok.find(':');
        if (colon == std::string::npos) continue;

        TlArg arg;
        arg.name = tok.substr(0, colon);
        std::string typeStr = tok.substr(colon + 1);

        if (typeStr == "#") {
            arg.isFlagsInt = true;
            arg.type.name = "int";
            ctor->args.push_back(arg);
            continue;
        }

        // Trường điều kiện: "flags.5?Type"
        size_t q = typeStr.find('?');
        if (q != std::string::npos) {
            std::string cond = typeStr.substr(0, q);
            typeStr = typeStr.substr(q + 1);
            size_t dot = cond.find('.');
            if (dot != std::string::npos) {
                arg.conditional = true;
                arg.flagsField = cond.substr(0, dot);
                arg.flagBit = std::atoi(cond.substr(dot + 1).c_str());
            }
        }
        if (typeStr == "true") {
            arg.isTrueFlag = true;
            arg.type.name = "true";
        } else {
            arg.type = parseType(typeStr);
        }
        ctor->args.push_back(arg);
    }

    byName_[ctor->name] = ctor;
    byId_[ctor->id] = ctor;
    if (!ctor->isFunction) byType_.emplace(ctor->resultType, ctor);
    return true;
}

size_t TlSchema::load(const std::string& text, std::vector<std::string>* warnings) {
    size_t before = byName_.size();
    bool isFunction = false;
    for (const auto& rawLine : split(text, '\n')) {
        std::string line = trim(rawLine);
        if (line.empty()) continue;
        if (startsWith(line, "//")) {
            // Dòng "// LAYER 158" cho biết số hiệu layer.
            size_t pos = line.find("LAYER");
            if (pos != std::string::npos) {
                int lv = std::atoi(trim(line.substr(pos + 5)).c_str());
                if (lv > 0) layer_ = lv;
            }
            continue;
        }
        if (startsWith(line, "---")) {
            std::string section = toLower(line);
            if (section.find("functions") != std::string::npos) isFunction = true;
            else if (section.find("types") != std::string::npos) isFunction = false;
            continue;
        }
        parseLine(line, isFunction, warnings);
    }
    size_t added = byName_.size() - before;
    LOG_DEBUG(kTag, "Nạp schema: thêm %zu hàm dựng (tổng %zu)", added, byName_.size());
    return added;
}

const TlConstructor* TlSchema::byName(const std::string& name) const {
    auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : it->second.get();
}

const TlConstructor* TlSchema::byId(uint32_t id) const {
    auto it = byId_.find(id);
    return it == byId_.end() ? nullptr : it->second.get();
}

const TlConstructor* TlSchema::soleConstructorOfType(const std::string& typeName) const {
    auto range = byType_.equal_range(typeName);
    if (range.first == range.second) return nullptr;
    auto it = range.first;
    const TlConstructor* first = it->second.get();
    ++it;
    if (it != range.second) return nullptr;  // nhiều hơn một hàm dựng
    return first;
}

std::vector<std::string> TlSchema::constructorNames() const {
    std::vector<std::string> out;
    out.reserve(byName_.size());
    for (const auto& kv : byName_) out.push_back(kv.first);
    return out;
}

const char* TlSchema::builtinMtprotoSchema() {
    const char* data = assets::findTextAsset("schema/mtproto.tl");
    return data ? data : "";
}

const char* TlSchema::builtinApiSchema() {
    const char* data = assets::findTextAsset("schema/api.tl");
    return data ? data : "";
}

}  // namespace tg
}  // namespace ttd
