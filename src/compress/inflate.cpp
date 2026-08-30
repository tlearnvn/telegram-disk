#include "compress/inflate.h"

#include <cstring>

#include "crypto/hash.h"

namespace ttd {
namespace compress {

namespace {

// Cây Huffman biểu diễn theo dạng bảng đếm/ký hiệu (giống puff của zlib).
struct HuffmanTable {
    // count[i] = số mã có độ dài i (1..15)
    uint16_t count[16];
    // symbol theo thứ tự mã tăng dần
    std::vector<uint16_t> symbol;
};

class BitReader {
public:
    BitReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    // Đọc `need` bit theo thứ tự LSB-first.
    bool bits(int need, uint32_t& out) {
        while (bitCount_ < need) {
            if (pos_ >= len_) return false;
            bitBuf_ |= static_cast<uint32_t>(data_[pos_++]) << bitCount_;
            bitCount_ += 8;
        }
        out = bitBuf_ & ((1u << need) - 1);
        bitBuf_ >>= need;
        bitCount_ -= need;
        return true;
    }

    void alignToByte() {
        bitBuf_ = 0;
        bitCount_ = 0;
    }

    bool readBytes(uint8_t* dst, size_t n) {
        if (pos_ + n > len_) return false;
        std::memcpy(dst, data_ + pos_, n);
        pos_ += n;
        return true;
    }

    bool skip(size_t n) {
        if (pos_ + n > len_) return false;
        pos_ += n;
        return true;
    }

    size_t position() const { return pos_; }
    size_t remaining() const { return len_ > pos_ ? len_ - pos_ : 0; }
    const uint8_t* dataAt() const { return data_ + pos_; }

private:
    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
    uint32_t bitBuf_ = 0;
    int bitCount_ = 0;
};

bool buildHuffman(HuffmanTable& t, const uint8_t* lengths, size_t n) {
    std::memset(t.count, 0, sizeof(t.count));
    for (size_t i = 0; i < n; ++i) t.count[lengths[i]]++;
    if (t.count[0] == n) return false;  // không có mã nào

    // Kiểm tra tính đầy đủ của mã.
    int left = 1;
    for (int len = 1; len <= 15; ++len) {
        left <<= 1;
        left -= t.count[len];
        if (left < 0) return false;  // mã dư
    }

    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + t.count[len]);

    t.symbol.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (lengths[i] != 0) t.symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    }
    return true;
}

int decodeSymbol(BitReader& br, const HuffmanTable& t) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        uint32_t b;
        if (!br.bits(1, b)) return -1;
        code |= static_cast<int>(b);
        int count = t.count[len];
        if (code - count < first) {
            size_t idx = static_cast<size_t>(index + (code - first));
            if (idx >= t.symbol.size()) return -1;
            return t.symbol[idx];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19, 23, 27,
                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                                33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                                1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13};

bool inflateBlockData(BitReader& br, const HuffmanTable& lenTable, const HuffmanTable& distTable,
                      Bytes& out, size_t maxOutput) {
    while (true) {
        int sym = decodeSymbol(br, lenTable);
        if (sym < 0) return false;
        if (sym < 256) {
            if (maxOutput && out.size() >= maxOutput) return false;
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            uint32_t extra = 0;
            if (kLengthExtra[sym] && !br.bits(kLengthExtra[sym], extra)) return false;
            size_t length = kLengthBase[sym] + extra;

            int dsym = decodeSymbol(br, distTable);
            if (dsym < 0 || dsym >= 30) return false;
            uint32_t dextra = 0;
            if (kDistExtra[dsym] && !br.bits(kDistExtra[dsym], dextra)) return false;
            size_t dist = kDistBase[dsym] + dextra;
            if (dist > out.size()) return false;
            if (maxOutput && out.size() + length > maxOutput) return false;

            size_t start = out.size() - dist;
            for (size_t i = 0; i < length; ++i) out.push_back(out[start + i]);
        }
    }
}

bool inflateStream(BitReader& br, Bytes& out, size_t maxOutput) {
    static HuffmanTable fixedLen, fixedDist;
    static bool fixedReady = false;
    if (!fixedReady) {
        uint8_t lens[288];
        for (int i = 0; i < 144; ++i) lens[i] = 8;
        for (int i = 144; i < 256; ++i) lens[i] = 9;
        for (int i = 256; i < 280; ++i) lens[i] = 7;
        for (int i = 280; i < 288; ++i) lens[i] = 8;
        buildHuffman(fixedLen, lens, 288);
        uint8_t dlens[30];
        for (int i = 0; i < 30; ++i) dlens[i] = 5;
        buildHuffman(fixedDist, dlens, 30);
        fixedReady = true;
    }

    while (true) {
        uint32_t last = 0, type = 0;
        if (!br.bits(1, last)) return false;
        if (!br.bits(2, type)) return false;

        if (type == 0) {
            // Khối lưu nguyên (stored).
            br.alignToByte();
            uint8_t hdr[4];
            if (!br.readBytes(hdr, 4)) return false;
            uint16_t len = static_cast<uint16_t>(hdr[0] | (hdr[1] << 8));
            uint16_t nlen = static_cast<uint16_t>(hdr[2] | (hdr[3] << 8));
            if (static_cast<uint16_t>(~len & 0xffff) != nlen) return false;
            if (maxOutput && out.size() + len > maxOutput) return false;
            if (br.remaining() < len) return false;
            out.insert(out.end(), br.dataAt(), br.dataAt() + len);
            br.skip(len);
        } else if (type == 1) {
            if (!inflateBlockData(br, fixedLen, fixedDist, out, maxOutput)) return false;
        } else if (type == 2) {
            uint32_t hlit, hdist, hclen;
            if (!br.bits(5, hlit) || !br.bits(5, hdist) || !br.bits(4, hclen)) return false;
            size_t nlen = hlit + 257, ndist = hdist + 1, ncode = hclen + 4;
            if (nlen > 286 || ndist > 30) return false;

            static const uint8_t kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                               11, 4,  12, 3, 13, 2, 14, 1, 15};
            uint8_t clens[19];
            std::memset(clens, 0, sizeof(clens));
            for (size_t i = 0; i < ncode; ++i) {
                uint32_t v;
                if (!br.bits(3, v)) return false;
                clens[kOrder[i]] = static_cast<uint8_t>(v);
            }
            HuffmanTable codeTable;
            if (!buildHuffman(codeTable, clens, 19)) return false;

            uint8_t lengths[320];
            std::memset(lengths, 0, sizeof(lengths));
            size_t index = 0;
            while (index < nlen + ndist) {
                int sym = decodeSymbol(br, codeTable);
                if (sym < 0) return false;
                if (sym < 16) {
                    lengths[index++] = static_cast<uint8_t>(sym);
                } else if (sym == 16) {
                    if (index == 0) return false;
                    uint8_t prev = lengths[index - 1];
                    uint32_t rep;
                    if (!br.bits(2, rep)) return false;
                    for (uint32_t i = 0; i < rep + 3 && index < nlen + ndist; ++i)
                        lengths[index++] = prev;
                } else if (sym == 17) {
                    uint32_t rep;
                    if (!br.bits(3, rep)) return false;
                    for (uint32_t i = 0; i < rep + 3 && index < nlen + ndist; ++i)
                        lengths[index++] = 0;
                } else {
                    uint32_t rep;
                    if (!br.bits(7, rep)) return false;
                    for (uint32_t i = 0; i < rep + 11 && index < nlen + ndist; ++i)
                        lengths[index++] = 0;
                }
            }
            if (lengths[256] == 0) return false;  // thiếu mã kết thúc khối

            HuffmanTable lenTable, distTable;
            if (!buildHuffman(lenTable, lengths, nlen)) return false;
            // Cây khoảng cách có thể rỗng nếu khối chỉ chứa ký tự đơn.
            bool anyDist = false;
            for (size_t i = 0; i < ndist; ++i)
                if (lengths[nlen + i]) anyDist = true;
            if (anyDist && !buildHuffman(distTable, lengths + nlen, ndist)) return false;
            if (!anyDist) {
                std::memset(distTable.count, 0, sizeof(distTable.count));
                distTable.symbol.clear();
            }
            if (!inflateBlockData(br, lenTable, distTable, out, maxOutput)) return false;
        } else {
            return false;  // type == 3: không hợp lệ
        }
        if (last) break;
    }
    return true;
}

}  // namespace

bool inflateRaw(const uint8_t* data, size_t len, Bytes& out, size_t maxOutput) {
    out.clear();
    BitReader br(data, len);
    return inflateStream(br, out, maxOutput);
}

bool inflateZlib(const uint8_t* data, size_t len, Bytes& out, size_t maxOutput) {
    if (len < 6) return false;
    uint8_t cmf = data[0], flg = data[1];
    if ((cmf & 0x0f) != 8) return false;               // chỉ hỗ trợ deflate
    if (((cmf << 8) | flg) % 31 != 0) return false;    // sai checksum tiêu đề
    if (flg & 0x20) return false;                      // có từ điển đặt trước — không hỗ trợ
    out.clear();
    BitReader br(data + 2, len - 2);
    return inflateStream(br, out, maxOutput);
}

bool inflateGzip(const uint8_t* data, size_t len, Bytes& out, size_t maxOutput) {
    if (len < 18) return false;
    if (data[0] != 0x1f || data[1] != 0x8b || data[2] != 8) return false;
    uint8_t flg = data[3];
    size_t pos = 10;
    if (flg & 0x04) {  // FEXTRA
        if (pos + 2 > len) return false;
        size_t xlen = static_cast<size_t>(data[pos]) | (static_cast<size_t>(data[pos + 1]) << 8);
        pos += 2 + xlen;
    }
    if (flg & 0x08) {  // FNAME
        while (pos < len && data[pos] != 0) ++pos;
        ++pos;
    }
    if (flg & 0x10) {  // FCOMMENT
        while (pos < len && data[pos] != 0) ++pos;
        ++pos;
    }
    if (flg & 0x02) pos += 2;  // FHCRC
    if (pos >= len) return false;

    out.clear();
    BitReader br(data + pos, len - pos);
    return inflateStream(br, out, maxOutput);
}

bool inflateAuto(const Bytes& data, Bytes& out, size_t maxOutput) {
    if (data.size() >= 2 && data[0] == 0x1f && data[1] == 0x8b)
        return inflateGzip(data.data(), data.size(), out, maxOutput);
    if (data.size() >= 2 && (data[0] & 0x0f) == 8 && ((data[0] << 8) | data[1]) % 31 == 0)
        return inflateZlib(data.data(), data.size(), out, maxOutput);
    return inflateRaw(data.data(), data.size(), out, maxOutput);
}

uint32_t adler32(const uint8_t* data, size_t len, uint32_t seed) {
    uint32_t a = seed & 0xffff, b = (seed >> 16) & 0xffff;
    const uint32_t kMod = 65521;
    while (len > 0) {
        size_t chunk = len < 5552 ? len : 5552;
        for (size_t i = 0; i < chunk; ++i) {
            a += data[i];
            b += a;
        }
        a %= kMod;
        b %= kMod;
        data += chunk;
        len -= chunk;
    }
    return (b << 16) | a;
}

Bytes gzipStored(const uint8_t* data, size_t len) {
    Bytes out;
    out.reserve(len + len / 65535 * 5 + 32);
    // Tiêu đề gzip.
    out.insert(out.end(), {0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0xff});
    size_t pos = 0;
    if (len == 0) {
        out.push_back(0x01);
        out.insert(out.end(), {0x00, 0x00, 0xff, 0xff});
    }
    while (pos < len) {
        size_t chunk = len - pos > 65535 ? 65535 : len - pos;
        bool last = (pos + chunk == len);
        out.push_back(last ? 0x01 : 0x00);
        out.push_back(static_cast<uint8_t>(chunk & 0xff));
        out.push_back(static_cast<uint8_t>((chunk >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>(~chunk & 0xff));
        out.push_back(static_cast<uint8_t>((~chunk >> 8) & 0xff));
        out.insert(out.end(), data + pos, data + pos + chunk);
        pos += chunk;
    }
    uint32_t crc = crypto::crc32(data, len);
    out.push_back(static_cast<uint8_t>(crc & 0xff));
    out.push_back(static_cast<uint8_t>((crc >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((crc >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((crc >> 24) & 0xff));
    uint32_t sz = static_cast<uint32_t>(len);
    out.push_back(static_cast<uint8_t>(sz & 0xff));
    out.push_back(static_cast<uint8_t>((sz >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((sz >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((sz >> 24) & 0xff));
    return out;
}

}  // namespace compress
}  // namespace ttd
