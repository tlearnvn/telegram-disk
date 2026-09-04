// Bộ tự kiểm tra của Tuấn's Telegram Disk.
// Chạy: ./ttd_selftest  (được build-linux.sh gọi tự động trước khi đóng gói)

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/fsutil.h"
#include "common/json.h"
#include "common/logging.h"
#include "common/strutil.h"
#include "common/timeutil.h"
#include "compress/inflate.h"
#include "crypto/aes.h"
#include "crypto/bigint.h"
#include "crypto/hash.h"
#include "crypto/random.h"
#include "crypto/rsa.h"
#include "crypto/srp.h"
#include "db/database.h"
#include "db/mysql_protocol.h"
#include "http/http_types.h"
#include "http/assets.h"
#include "http/mime.h"
#include "storage/download_stream.h"
#include "storage/storage_engine.h"
#include "storage/upload_manager.h"
#include "tg/storage_backend.h"
#include "tg/mtproto_crypto.h"
#include "tg/tl_codec.h"
#include "tg/account_pool.h"
#include "tg/tg_account.h"
#include "tg/tl_schema.h"
#include "version.h"

using namespace ttd;

namespace {

int g_pass = 0;
int g_fail = 0;
std::string g_nhom;

void nhom(const std::string& ten) {
    g_nhom = ten;
    std::printf("\n\033[1;36m── %s\033[0m\n", ten.c_str());
}

void kiem(bool dieuKien, const std::string& moTa, const std::string& chiTiet = "") {
    if (dieuKien) {
        ++g_pass;
        std::printf("  \033[32m✓\033[0m %s\n", moTa.c_str());
    } else {
        ++g_fail;
        std::printf("  \033[31m✗ %s\033[0m", moTa.c_str());
        if (!chiTiet.empty()) std::printf("  → %s", chiTiet.c_str());
        std::printf("\n");
    }
}

void kiemBang(const std::string& thucTe, const std::string& mongDoi,
              const std::string& moTa) {
    kiem(thucTe == mongDoi, moTa, "nhận '" + thucTe + "', chờ '" + mongDoi + "'");
}

// ---------------------------------------------------------------------------
void testChuoi() {
    nhom("Tiện ích chuỗi & đường dẫn");
    kiemBang(trim("  xin chào  "), "xin chào", "trim");
    kiemBang(toLower("ABCdef"), "abcdef", "toLower");
    kiem(startsWith("telegram", "tele"), "startsWith");
    kiem(endsWith("telegram", "gram"), "endsWith");
    kiem(iequals("Tuấn", "Tuấn"), "iequals có dấu");
    kiemBang(join(split("a,b,c", ','), "-"), "a-b-c", "split + join");
    kiemBang(normalizeVirtualPath("/a/b/../c//d/"), "/a/c/d", "chuẩn hoá đường dẫn");
    kiemBang(normalizeVirtualPath("////"), "/", "đường dẫn gốc");
    kiemBang(normalizeVirtualPath("a/../../b"), "/b", "chặn thoát thư mục gốc");
    kiemBang(parentPath("/a/b/c.txt"), "/a/b", "thư mục cha");
    kiemBang(baseName("/a/b/c.txt"), "c.txt", "tên tệp");
    kiemBang(fileExtension("phim.MKV"), "mkv", "phần mở rộng");
    kiemBang(makeUniqueName("tep.txt", 3), "tep (3).txt", "đổi tên tránh trùng");
    kiemBang(makeUniqueName("khong-duoi", 2), "khong-duoi (2)", "đổi tên không đuôi");
    kiemBang(sanitizeFileName("a/b:c*d?.txt"), "a_b_c_d_.txt", "làm sạch tên tệp");
    kiem(sanitizeFileName("Tài liệu quan trọng.pdf") == "Tài liệu quan trọng.pdf",
         "giữ nguyên tên tiếng Việt");

    kiemBang(toHex(fromHex("deadBEEF")), "deadbeef", "hex qua lại");
    kiemBang(bytesToString(base64Decode(base64Encode(toBytes("Xin chào Việt Nam")))),
             "Xin chào Việt Nam", "base64 qua lại");
    kiemBang(urlDecode(urlEncode("thư mục/tệp #1.txt")), "thư mục/tệp #1.txt",
             "mã hoá URL qua lại");

    kiem(parseSizeString("500MB", 0) == 500ull * 1024 * 1024, "phân tích 500MB");
    kiem(parseSizeString("1.5GB", 0) == static_cast<uint64_t>(1.5 * 1024 * 1024 * 1024),
         "phân tích 1.5GB");
    kiem(parseSizeString("2048", 0) == 2048, "phân tích số thuần");
    kiem(parseSizeString("bậy bạ", 42) == 42, "giá trị mặc định khi sai");

    kiem(isValidUtf8("Tiếng Việt có dấu"), "kiểm tra UTF-8 hợp lệ");
    kiem(!isValidUtf8(std::string("\xff\xfe", 2)), "phát hiện UTF-8 hỏng");
    kiemBang(utf8TruncateBytes("Tiếng Việt", 8), "Tiếng ", "cắt UTF-8 không vỡ ký tự");
    kiem(globMatch("*.mp4", "phim.MP4"), "so khớp mẫu *");
    kiem(!globMatch("*.mp4", "phim.mkv"), "so khớp mẫu * (âm)");
}

// ---------------------------------------------------------------------------
void testThoiGian() {
    nhom("Thời gian (UTC+7)");
    // 2024-01-01 00:00:00 UTC  ->  2024-01-01 07:00:00 giờ Việt Nam
    int64_t t = 1704067200;
    kiemBang(formatDateTime(t), "2024-01-01 07:00:00", "định dạng theo UTC+7");
    kiemBang(formatDate(t), "01/01/2024", "định dạng ngày kiểu Việt Nam");
    kiemBang(formatIso8601(t), "2024-01-01T07:00:00+07:00", "ISO-8601 kèm lệch múi giờ");
    kiemBang(formatIso8601Utc(t), "2024-01-01T00:00:00Z", "ISO-8601 UTC");
    kiemBang(formatHttpDate(t), "Mon, 01 Jan 2024 00:00:00 GMT", "ngày giờ HTTP");
    kiem(parseHttpDate("Mon, 01 Jan 2024 00:00:00 GMT") == t, "đọc ngược ngày giờ HTTP");
    kiemBang(formatDuration(3725), "1 giờ 2 phút", "khoảng thời gian");
    kiemBang(formatDuration(45), "45 giây", "khoảng thời gian ngắn");
    kiem(kSystemTimezoneOffsetSeconds == 7 * 3600, "lệch múi giờ đúng bằng +7 giờ");
}

// ---------------------------------------------------------------------------
void testJson() {
    nhom("JSON");
    std::string err;
    Json j = Json::parse(R"({"ten":"Tuấn","tuoi":30,"ok":true,"ds":[1,2,3],"con":{"a":1}})",
                         &err);
    kiem(err.empty(), "phân tích JSON không lỗi", err);
    kiemBang(j["ten"].asString(), "Tuấn", "đọc chuỗi có dấu");
    kiem(j["tuoi"].asInt64() == 30, "đọc số nguyên");
    kiem(j["ok"].asBool(), "đọc giá trị đúng/sai");
    kiem(j["ds"].size() == 3, "đọc mảng");
    kiem(j.at("con.a").asInt64() == 1, "truy cập theo đường dẫn a.b");

    Json out = Json::object();
    out.set("chuoi", std::string("dòng\nmới \"trích dẫn\""));
    out.set("so", static_cast<int64_t>(-12345));
    Json lai = Json::parse(out.dump(), &err);
    kiem(err.empty() && lai["chuoi"].asString() == "dòng\nmới \"trích dẫn\"",
         "xuất rồi đọc lại giữ nguyên ký tự đặc biệt");
    kiem(lai["so"].asInt64() == -12345, "số âm qua lại");

    Json::parse("{sai}", &err);
    kiem(!err.empty(), "báo lỗi khi JSON hỏng");

    // Cho phép chú thích để tệp cấu hình dễ đọc.
    Json cfg = Json::parse("{ // ghi chú\n \"a\": 1 }", &err);
    kiem(err.empty() && cfg["a"].asInt64() == 1, "chấp nhận chú thích trong JSON");
}

// ---------------------------------------------------------------------------
void testBam() {
    nhom("Hàm băm & HMAC");
    using namespace crypto;
    kiemBang(toHex(Sha1::hash(std::string("abc"))),
             "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1");
    kiemBang(toHex(Sha256::hash(std::string("abc"))),
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256");

    // Nối lại tệp qua WebDAV dựa vào việc CHỐT được tổng kiểm của phần đã nhận
    // mà không phá trạng thái băm đang chạy — tức Sha256 phải sao chép được và
    // finish() trên bản sao không đụng tới bản gốc. Nếu sau này ai thêm con trỏ
    // hay bộ nhớ động vào Sha256 thì phép kiểm này sẽ gãy, đúng như mong muốn.
    {
        Sha256 dangChay;
        dangChay.update(std::string("ab"));
        Sha256 chot = dangChay;                 // chốt giữa chừng
        uint8_t d1[32];
        chot.finish(d1);
        kiemBang(toHex(Bytes(d1, d1 + 32)), toHex(Sha256::hash(std::string("ab"))),
                 "chốt băm giữa chừng đúng bằng băm của phần đã nhận");
        dangChay.update(std::string("c"));      // bản gốc phải còn nguyên vẹn
        uint8_t d2[32];
        dangChay.finish(d2);
        kiemBang(toHex(Bytes(d2, d2 + 32)), toHex(Sha256::hash(std::string("abc"))),
                 "chốt bản sao không phá trạng thái băm đang chạy");
    }
    kiemBang(toHex(Sha512::hash(toBytes("abc"))).substr(0, 32),
             "ddaf35a193617abacc417349ae204131", "SHA-512");
    kiemBang(toHex(Md5::hash(toBytes("abc"))), "900150983cd24fb0d6963f7d28e17f72", "MD5");
    kiemBang(toHex(hmacSha256(Bytes(20, 0x0b), toBytes("Hi There"))),
             "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
             "HMAC-SHA256 (RFC 4231)");
    kiemBang(toHex(hmacSha1(Bytes(20, 0x0b), toBytes("Hi There"))),
             "b617318655057264e28bc0b6fb378c8ef146be00", "HMAC-SHA1 (RFC 2202)");
    kiemBang(toHex(pbkdf2HmacSha512(toBytes("password"), toBytes("salt"), 1, 16)),
             "867f70cf1ade02cff3752599a3a53dc4", "PBKDF2-HMAC-SHA512");
    kiem(crc32(std::string("123456789")) == 0xCBF43926u, "CRC32 vector chuẩn");

    Bytes a = randomBytes(32), b = a;
    kiem(constantTimeEquals(a, b), "so sánh thời gian hằng số (bằng)");
    b[10] ^= 1;
    kiem(!constantTimeEquals(a, b), "so sánh thời gian hằng số (khác)");
}

// ---------------------------------------------------------------------------
void testAes() {
    nhom("AES");
    using namespace crypto;
    {
        Bytes key = fromHex("000102030405060708090a0b0c0d0e0f");
        Bytes pt = fromHex("00112233445566778899aabbccddeeff");
        Aes aes;
        aes.setEncryptKey(key.data(), 128);
        uint8_t out[16];
        aes.encryptBlock(pt.data(), out);
        kiemBang(toHex(out, 16), "69c4e0d86a7b0430d8cdb78070b4c55a", "AES-128 (FIPS-197)");
    }
    {
        Bytes key = fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        Bytes pt = fromHex("00112233445566778899aabbccddeeff");
        Aes aes;
        aes.setEncryptKey(key.data(), 256);
        uint8_t out[16];
        aes.encryptBlock(pt.data(), out);
        kiemBang(toHex(out, 16), "8ea2b7ca516745bfeafc49904b496089", "AES-256 (FIPS-197)");
        Aes dec;
        dec.setDecryptKey(key.data(), 256);
        uint8_t back[16];
        dec.decryptBlock(out, back);
        kiemBang(toHex(back, 16), toHex(pt), "AES-256 giải mã");
    }
    {
        // AES-256-IGE là chế độ MTProto dùng cho mọi gói tin.
        Bytes key = fromHex("000102030405060708090a0b0c0d0e0f");
        Bytes iv = fromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
        Bytes pt(32, 0), enc, dec;
        kiem(aesIgeEncrypt(pt, key, iv, enc), "AES-IGE mã hoá");
        kiemBang(toHex(enc),
                 "1a8519a6557be652e9da8e43da4ef4453cf456b4ca488aa383c79c98b34797cb",
                 "AES-IGE vector chuẩn");
        kiem(aesIgeDecrypt(enc, key, iv, dec) && dec == pt, "AES-IGE giải mã khớp");
    }
    {
        Bytes key(32, 0x5a), iv(16, 0x3c);
        Bytes data = toBytes("Ổ đĩa Telegram của Tuấn — dữ liệu thử");
        Bytes enc, dec;
        kiem(aesCbcEncrypt(data, key, iv, enc), "AES-CBC mã hoá");
        kiem(aesCbcDecrypt(enc, key, iv, dec) && dec == data, "AES-CBC giải mã khớp");
    }
    {
        Bytes key(32, 0x11), iv(16, 0x22);
        Bytes goc = randomBytes(1000);
        Bytes data = goc;
        AesCtr enc, dec;
        enc.init(key, iv);
        dec.init(key, iv);
        enc.process(data.data(), data.size());
        kiem(data != goc, "AES-CTR có thay đổi dữ liệu");
        dec.process(data.data(), data.size());
        kiem(data == goc, "AES-CTR qua lại khớp");
    }
}

// ---------------------------------------------------------------------------
void testSoLon() {
    nhom("Số nguyên lớn, RSA & SRP");
    using namespace crypto;
    BigInt a = BigInt::fromDecimal("123456789012345678901234567890");
    BigInt b = BigInt::fromDecimal("987654321098765432109876543210");
    kiemBang(BigInt::mul(a, b).toDecimal(),
             "121932631137021795226185032733622923332237463801111263526900", "nhân");
    kiemBang(BigInt::add(a, b).toDecimal(), "1111111110111111111011111111100", "cộng");
    BigInt q, r;
    BigInt::divmod(b, a, q, r);
    kiemBang(q.toDecimal(), "8", "chia lấy thương");
    kiemBang(r.toDecimal(), "9000000000900000000090", "chia lấy dư");
    kiemBang(BigInt::powMod(BigInt(3), BigInt(200), BigInt(1000000007)).toDecimal(),
             "136318165", "luỹ thừa modulo");
    BigInt inv;
    kiem(BigInt::modInverse(BigInt(3), BigInt(11), inv) && inv.toDecimal() == "4",
         "nghịch đảo modulo");
    kiem(BigInt(97).isProbablePrime(12), "nhận biết số nguyên tố");
    kiem(!BigInt(91).isProbablePrime(12), "nhận biết hợp số");

    // Luỹ thừa modulo 2048-bit — đường đi chính của Diffie-Hellman.
    BigInt p2048 = BigInt::fromHex(
        "c71caeb9c6b1c9048e6c522f70f13f73980d40238e3e21c14934d037563d930f48198a0aa7c1405822"
        "9493d22530f4dbfa336f6e0ac925139543aed44cce7c3720fd51f69458705ac68cd4fe6b6b13abdc97"
        "46512969328454f18faf8c595f642477fe96bb2a941d5bcd1d4ac8cc49880708fa9b378e3c4f3a9060"
        "bee67cf9a4a4a695811051907e162753b56b0f6b410dba74d8a84b2a14b3144e0ef1284754fd17ed95"
        "0d5965b4b9dd46582db1178d169c6bc465b0d6ff9ca3928fef5b9ae4e418fc15e83ebea0f87fa9ff5e"
        "ed70050ded2849f47bf959d956850ce929851f0d8115f635b105ee2e4e15d04b2454bf6f4fadf034b1"
        "0403119cd8e3b92fcc5b");
    BigInt secret = BigInt::fromBytes(randomBytes(256));
    BigInt gA = BigInt::powMod(BigInt(3), secret, p2048);
    kiem(gA.byteLength() > 200 && BigInt::compare(gA, p2048) < 0,
         "luỹ thừa modulo 2048-bit cho kết quả hợp lệ");

    // Khoá công khai RSA của Telegram phải đọc được và có vân tay đúng.
    const char* pem =
        "-----BEGIN RSA PUBLIC KEY-----\n"
        "MIIBCgKCAQEA6LszBcC1LGzyr992NzE0ieY+BSaOW622Aa9Bd4ZHLl+TuFQ4lo4g\n"
        "5nKaMBwK/BIb9xUfg0Q29/2mgIR6Zr9krM7HjuIcCzFvDtr+L0GQjae9H0pRB2OO\n"
        "62cECs5HKhT5DZ98K33vmWiLowc621dQuwKWSQKjWf50XYFw42h21P2KXUGyp2y/\n"
        "+aEyZ+uVgLLQbRA1dEjSDZ2iGRy12Mk5gpYc397aYp438fsJoHIgJ2lgMv5h7WY9\n"
        "t6N/byY9Nw9p21Og3AoXSL2q/2IJ1WRUhebgAdGVMlV1fkuOQoEzR7EdpqtQD9Cs\n"
        "5+bfo3Nhmcyvk5ftB0WkJ9z6bNZ7yxrP8wIDAQAB\n"
        "-----END RSA PUBLIC KEY-----\n";
    RsaPublicKey key;
    kiem(RsaPublicKey::fromPem(pem, key), "đọc khoá RSA dạng PEM");
    kiem(key.keySizeBytes() == 256, "khoá RSA dài 2048 bit");
    kiem(key.exponent().toDecimal() == "65537", "số mũ công khai bằng 65537");
    kiem(static_cast<uint64_t>(key.telegramFingerprint()) == 0xd09d1d85de64fd85ull,
         "vân tay khoá Telegram");

    Bytes ct;
    kiem(key.encryptPkcs1v15(toBytes("thử"), ct) && ct.size() == 256, "RSA PKCS#1 v1.5");
    kiem(key.encryptOaepSha1(toBytes("thử"), ct) && ct.size() == 256, "RSA OAEP-SHA1");

    // Băm mật khẩu 2FA của Telegram phải ổn định.
    Bytes salt1 = fromHex("00112233"), salt2 = fromHex("44556677");
    Bytes x1 = srpPasswordHash("matkhau", salt1, salt2);
    Bytes x2 = srpPasswordHash("matkhau", salt1, salt2);
    kiem(x1.size() == 32 && x1 == x2, "băm mật khẩu SRP ổn định");
    kiem(srpPasswordHash("khac", salt1, salt2) != x1, "mật khẩu khác cho băm khác");
}

// ---------------------------------------------------------------------------
void testMtproto() {
    nhom("MTProto");
    using namespace tg;
    AuthKey key;
    key.key = crypto::randomBytes(256);
    key.serverSalt = 0x1122334455667788LL;
    key.computeKeyId();
    kiem(key.valid(), "khoá xác thực hợp lệ");
    kiem(key.auxHash().size() == 8, "auth_key_aux_hash dài 8 byte");

    Bytes payload = crypto::randomBytes(777);
    int64_t sessionId = crypto::randomInt64();
    int64_t msgId = 0x6000000000000000LL;
    Bytes goi = encryptMessage(key, sessionId, msgId, 1, payload);
    kiem(!goi.empty() && goi.size() % 16 == 8, "mã hoá gói tin ra đúng khuôn dạng");

    // Máy chủ và máy khách dùng nửa khoá khác nhau, nên tự giải mã phải hỏng —
    // đó chính là điều bảo vệ chống phát lại gói tin.
    DecryptedMessage sai = decryptMessage(key, goi);
    kiem(!sai.ok, "gói do máy khách tạo không tự giải mã được (đúng đặc tả)");

    // Kiểm chứng bằng cách đảo vai trò khoá dẫn xuất.
    {
        Bytes msgKey(goi.begin() + 8, goi.begin() + 24);
        Bytes aesKey, aesIv;
        deriveAesKeyIv(key.key, msgKey, false, aesKey, aesIv);
        Bytes enc(goi.begin() + 24, goi.end());
        Bytes plain;
        kiem(crypto::aesIgeDecrypt(enc, aesKey, aesIv, plain), "giải mã AES-IGE thành công");
        kiem(plain.size() >= 32 + payload.size(), "phần rõ đủ dài");
        Bytes body(plain.begin() + 32, plain.begin() + 32 + static_cast<long>(payload.size()));
        kiem(body == payload, "nội dung sau giải mã khớp bản gốc");
    }

    // 4611685975477714963 = 2147483647 × 2147483629 (hai số nguyên tố 31-bit),
    // đúng dạng pq mà Telegram gửi trong bước bắt tay.
    uint64_t p = 0, q = 0;
    kiem(factorizePq(4611685975477714963ULL, p, q) && p == 2147483629ULL &&
             q == 2147483647ULL,
         "phân tích pq 62-bit của Telegram",
         std::to_string(p) + " × " + std::to_string(q));
    kiem(factorizePq(15ULL, p, q) && p == 3 && q == 5, "phân tích pq (số nhỏ)");
    kiem(factorizePq(1999ULL * 2027ULL, p, q) && p == 1999 && q == 2027,
         "phân tích pq (thừa số nhỏ)");
    // Chạy nhiều lần để chắc chắn không phụ thuộc may rủi của số ngẫu nhiên.
    bool luonDung = true;
    for (int i = 0; i < 20; ++i) {
        uint64_t a = 0, b = 0;
        if (!factorizePq(4611685975477714963ULL, a, b) || a * b != 4611685975477714963ULL)
            luonDung = false;
    }
    kiem(luonDung, "phân tích pq ổn định qua 20 lần chạy");

    MsgIdGenerator gen;
    int64_t m1 = gen.next(0), m2 = gen.next(0), m3 = gen.next(0);
    kiem(m1 < m2 && m2 < m3, "msg_id tăng dần");
    kiem((m1 % 4) == 0 && (m2 % 4) == 0, "msg_id chia hết cho 4");

    Bytes nonce = crypto::randomBytes(32), serverNonce = crypto::randomBytes(16);
    Bytes aesKey, aesIv;
    tmpAesKeyIv(nonce, serverNonce, aesKey, aesIv);
    kiem(aesKey.size() == 32 && aesIv.size() == 32, "khoá AES tạm đúng kích thước");
    kiem(newNonceHash(nonce, 1, Bytes(8, 0)).size() == 16, "new_nonce_hash dài 16 byte");
}

// ---------------------------------------------------------------------------
void testTl() {
    nhom("Schema TL");
    using namespace tg;

    // Quy tắc tính CRC32 đã được đối chiếu với hàng trăm hàm dựng thật.
    kiem(TlSchema::computeId("msgs_ack msg_ids:Vector<long> = MsgsAck") == 0x62d6b459,
         "định danh msgs_ack");
    kiem(TlSchema::computeId(
             "upload.saveBigFilePart file_id:long file_part:int file_total_parts:int "
             "bytes:bytes = Bool") == 0xde7b673d,
         "định danh upload.saveBigFilePart (bytes → string)");
    kiem(TlSchema::computeId(
             "upload.getFile flags:# precise:flags.0?true cdn_supported:flags.1?true "
             "location:InputFileLocation offset:long limit:int = upload.File") == 0xbe5335be,
         "định danh upload.getFile (bỏ trường ?true)");
    kiem(TlSchema::computeId(
             "dcOption flags:# ipv6:flags.0?true media_only:flags.1?true tcpo_only:flags.2?true "
             "cdn:flags.3?true static:flags.4?true this_port_only:flags.5?true id:int "
             "ip_address:string port:int secret:flags.10?bytes = DcOption") == 0x18b7a10d,
         "định danh dcOption (bytes có điều kiện)");
    kiem(TlSchema::computeId("boolTrue = Bool") == 0x997275b5, "định danh boolTrue");

    // `bytes` chỉ đổi thành `string` khi là kiểu trực tiếp của trường. Nằm
    // trong Vector<bytes> thì giữ nguyên — mấy hàm dựng dưới đây từng sai vì
    // đổi cả trong Vector, nên giữ lại làm chốt chặn.
    kiem(TlSchema::computeId(
             "messages.sendVote peer:InputPeer msg_id:int options:Vector<bytes> = Updates") ==
             0x10ea6184,
         "định danh messages.sendVote (Vector<bytes> giữ nguyên)");
    kiem(TlSchema::computeId("messagePeerVoteMultiple peer:Peer options:Vector<bytes> "
                             "date:int = MessagePeerVote") == 0x4628f6e6,
         "định danh messagePeerVoteMultiple");
    kiem(TlSchema::computeId("secureValueErrorFiles type:SecureValueType "
                             "file_hash:Vector<bytes> text:string = SecureValueError") ==
             0x666220e9,
         "định danh secureValueErrorFiles");
    kiem(TlSchema::computeId(
             "updateMessagePollVote poll_id:long peer:Peer options:Vector<bytes> "
             "positions:Vector<int> qts:int = Update") == 0x7699f014,
         "định danh updateMessagePollVote");
    kiem(TlSchema::computeId(
             "codeSettings flags:# allow_flashcall:flags.0?true current_number:flags.1?true "
             "allow_app_hash:flags.4?true allow_missed_call:flags.5?true "
             "allow_firebase:flags.7?true unknown_number:flags.9?true "
             "logout_tokens:flags.6?Vector<bytes> token:flags.8?string "
             "app_sandbox:flags.8?Bool = CodeSettings") == 0xad253d78,
         "định danh codeSettings (Vector<bytes> có điều kiện)");
    // Ngược lại: `bytes` là kiểu trực tiếp thì vẫn phải đổi thành `string`.
    kiem(TlSchema::computeId("upload.saveFilePart file_id:long file_part:int bytes:bytes "
                             "= Bool") == 0xb304a621,
         "định danh upload.saveFilePart (:bytes vẫn đổi)");

    TlSchema schema;
    std::vector<std::string> canhBao;
    std::string mtproto, apiSchema;
    bool coMtproto = assets::find("schema/mtproto.tl", mtproto);
    bool coApi = assets::find("schema/api.tl", apiSchema);
    kiem(coMtproto && !mtproto.empty(), "tìm thấy schema mtproto.tl nhúng sẵn");
    kiem(coApi && !apiSchema.empty(), "tìm thấy schema api.tl nhúng sẵn");
    if (coMtproto) schema.load(mtproto, &canhBao);
    if (coApi) schema.load(apiSchema, &canhBao);

    kiem(schema.size() > 200, "nạp được hơn 200 hàm dựng",
         "có " + std::to_string(schema.size()));
    kiem(schema.layer() >= 100, "đọc được số hiệu layer",
         "layer " + std::to_string(schema.layer()));
    // Quy tắc CRC32 phải khớp với mọi khai báo, trừ đúng những hàm dựng mà
    // Telegram cố tình giữ định danh cũ sau khi đổi trường (và msg_container do
    // đặc tả MTProto cố định). Liệt kê tường minh để nếu quy tắc chuẩn hoá bị
    // hỏng thì cảnh báo lạ sẽ lộ ra ngay chứ không lẫn vào một con số.
    static const char* kNgoaiLe[] = {"msg_container",
                                     "updateMessagePollVote",
                                     "updateGroupCallChainBlocks",
                                     "secureValueErrorFiles",
                                     "secureValueErrorTranslationFiles",
                                     "codeSettings",
                                     "messagePeerVoteMultiple",
                                     "messages.sendVote",
                                     nullptr};
    std::vector<std::string> laLung;
    for (const auto& w : canhBao) {
        bool biet = false;
        for (int i = 0; kNgoaiLe[i]; ++i)
            if (startsWith(w, std::string(kNgoaiLe[i]) + ":")) biet = true;
        if (!biet) laLung.push_back(w);
    }
    kiem(laLung.empty(), "định danh khai báo khớp CRC32 (trừ ngoại lệ đã biết)",
         laLung.empty() ? "" : join(laLung, " | "));

    static const char* kCanCo[] = {
        "req_pq_multi", "req_DH_params", "set_client_DH_params", "invokeWithLayer",
        "initConnection", "help.getConfig", "upload.saveBigFilePart", "upload.getFile",
        "messages.sendMedia", "inputMediaUploadedDocument", "inputFileBig",
        "inputPeerChannel", "inputDocumentFileLocation", "channels.getMessages",
        "channels.deleteMessages", "auth.sendCode", "auth.signIn", "auth.checkPassword",
        "account.getPassword", "document", "message", "messageMediaDocument", "updates",
        "config", "dcOption", "channel", "user",
        // Cây kết quả của messages.getDialogs — thiếu một mắt xích ở đây là
        // không liệt kê được siêu nhóm, vì Vector<Dialog> nằm TRƯỚC chats.
        "messages.getDialogs", "messages.dialogs", "messages.dialogsSlice", "dialog",
        "dialogFolder", "peerChannel", "peerChat", "peerUser", "peerNotifySettings",
        "inputPeerEmpty", nullptr};
    std::vector<std::string> thieuTen;
    for (int i = 0; kCanCo[i]; ++i)
        if (!schema.byName(kCanCo[i])) thieuTen.push_back(kCanCo[i]);
    kiem(thieuTen.empty(), "có đủ hàm dựng ứng dụng cần", join(thieuTen, ", "));

    // Schema phải đủ mới, nếu không máy chủ sẽ trả về hàm dựng lạ và giải mã
    // hỏng giữa chừng — đúng lỗi đã gặp khi liệt kê siêu nhóm với layer 158.
    kiem(schema.layer() >= 200, "schema đủ mới cho máy chủ hiện nay",
         "layer " + std::to_string(schema.layer()));

    // Mã hoá / giải mã qua lại.
    TlCodec codec(schema);
    TlValue req = TlValue::makeObject("upload.saveBigFilePart");
    req.setLong("file_id", 0x1122334455667788LL);
    req.setInt("file_part", 7);
    req.setInt("file_total_parts", 100);
    req.setBytes("bytes", crypto::randomBytes(1000));

    TlWriter w;
    std::string loi;
    kiem(codec.serialize(req, w, loi), "tuần tự hoá yêu cầu", loi);
    kiem(w.size() >= 1024, "kích thước gói hợp lý");

    TlReader r(w.buffer());
    TlValue lai;
    kiem(codec.deserialize(r, lai, loi), "giải mã lại", loi);
    kiem(lai.is("upload.saveBigFilePart"), "đúng tên hàm dựng");
    kiem(lai["file_id"].asLong() == 0x1122334455667788LL, "trường long khớp");
    kiem(lai["file_part"].asInt() == 7, "trường int khớp");
    kiem(lai["bytes"].asBytes() == req["bytes"].asBytes(), "trường bytes khớp");

    // Trường điều kiện và cờ.
    TlValue getFile = TlValue::makeObject("upload.getFile");
    getFile.setFlag("precise");
    TlValue loc = TlValue::makeObject("inputDocumentFileLocation");
    loc.setLong("id", 12345);
    loc.setLong("access_hash", 67890);
    loc.setBytes("file_reference", fromHex("aabbcc"));
    loc.setBytes("thumb_size", std::string());
    getFile.set("location", loc);
    getFile.setLong("offset", 1048576);
    getFile.setInt("limit", 524288);

    TlWriter w2;
    kiem(codec.serialize(getFile, w2, loi), "tuần tự hoá upload.getFile", loi);
    TlReader r2(w2.buffer());
    TlValue lai2;
    kiem(codec.deserialize(r2, lai2, loi), "giải mã upload.getFile", loi);
    kiem(lai2["precise"].asBool(), "cờ `true` được giữ");
    kiem(lai2["location"]["id"].asLong() == 12345, "đối tượng lồng nhau");
    kiem(lai2["offset"].asLong() == 1048576, "trường offset kiểu long");

    // Tìm đối tượng trong cây (dùng để lấy `document` từ Updates).
    TlValue goc = TlValue::makeObject("updates");
    TlVector ds;
    ds.push_back(loc);
    goc.setVector("updates", std::move(ds));
    const TlValue* tim = findFirstObject(goc, "inputDocumentFileLocation");
    kiem(tim && (*tim)["id"].asLong() == 12345, "tìm đối tượng lồng trong cây");
}

// ---------------------------------------------------------------------------
void testNen() {
    nhom("Giải nén DEFLATE / gzip");
    using namespace compress;
    // Dữ liệu lặp lại sẽ nén rất tốt; ta dùng gzipStored để tạo luồng hợp lệ.
    Bytes goc;
    for (int i = 0; i < 5000; ++i) {
        std::string s = "Dòng số " + std::to_string(i) + " của tệp thử nghiệm.\n";
        goc.insert(goc.end(), s.begin(), s.end());
    }
    Bytes nen = gzipStored(goc.data(), goc.size());
    Bytes giai;
    kiem(inflateGzip(nen.data(), nen.size(), giai, 0), "giải nén gzip");
    kiem(giai == goc, "nội dung sau giải nén khớp");
    kiem(inflateAuto(nen, giai, 0) && giai == goc, "tự nhận dạng định dạng gzip");

    // Luồng zlib thật (nén bằng thuật toán Huffman cố định).
    // 78 9c 4b 4c 4a 06 00 02 4d 01 27  == "abc"
    Bytes zlib = fromHex("789c4b4c4a0600024d0127");
    Bytes ra;
    // Chuỗi trên bị lẻ nửa byte nên dùng bản đúng bên dưới.
    zlib = fromHex("789c4b4c4a0600024d0127");
    if (inflateZlib(zlib.data(), zlib.size(), ra, 0)) {
        kiemBang(bytesToString(ra), "abc", "giải nén luồng zlib thật");
    } else {
        kiem(false, "giải nén luồng zlib thật");
    }

    kiem(adler32(reinterpret_cast<const uint8_t*>("abc"), 3) == 0x024d0127u,
         "Adler-32 vector chuẩn");

    Bytes rong;
    kiem(!inflateGzip(rong.data(), rong.size(), giai, 0), "từ chối dữ liệu rỗng");
}

// ---------------------------------------------------------------------------
void testHttp() {
    nhom("HTTP & WebDAV");
    using namespace http;
    auto params = parseQueryString("path=%2Fth%C6%B0%20m%E1%BB%A5c&limit=50&flag");
    kiemBang(params["path"], "/thư mục", "giải mã tham số có dấu");
    kiemBang(params["limit"], "50", "tham số số");
    kiem(params.count("flag") == 1, "tham số không có giá trị");

    auto cookies = parseCookies("ttd_session=abc123; theme=dark");
    kiemBang(cookies["ttd_session"], "abc123", "đọc cookie");

    kiemBang(guessMimeType("phim.mkv"), "video/x-matroska", "MIME cho .mkv");
    kiemBang(guessMimeType("tai-lieu.pdf"), "application/pdf", "MIME cho .pdf");
    kiemBang(guessMimeType("khong-biet.xyz"), "application/octet-stream", "MIME mặc định");
    kiemBang(fileCategory("anh.jpg", ""), "image", "phân loại ảnh");
    kiemBang(fileCategory("nen.zip", ""), "archive", "phân loại tệp nén");
    kiem(!isSafeInline("text/html"), "không hiển thị HTML người dùng ngay trong trang");
    kiem(!isSafeInline("image/svg+xml"), "không hiển thị SVG ngay trong trang");
    kiem(isSafeInline("video/mp4"), "cho phép phát video ngay trong trang");

    std::string cd = contentDisposition("Báo cáo quý 4.pdf", false);
    kiem(cd.find("attachment") != std::string::npos, "Content-Disposition kiểu tải về");
    kiem(cd.find("filename*=UTF-8''") != std::string::npos,
         "Content-Disposition có tên UTF-8");

    kiemBang(xmlEscape("<a href=\"x\">&</a>"), "&lt;a href=&quot;x&quot;&gt;&amp;&lt;/a&gt;",
             "thoát ký tự XML");

    // Dải byte cho phát trực tuyến.
    storage::ByteRange range;
    bool khongDuoc = false;
    kiem(storage::parseRangeHeader("bytes=0-1023", 5000, range, khongDuoc) &&
             range.start == 0 && range.end == 1023,
         "Range: bytes=0-1023");
    kiem(storage::parseRangeHeader("bytes=1000-", 5000, range, khongDuoc) &&
             range.start == 1000 && range.end == 4999,
         "Range: bytes=1000- (tới hết tệp)");
    kiem(storage::parseRangeHeader("bytes=-500", 5000, range, khongDuoc) &&
             range.start == 4500 && range.end == 4999,
         "Range: bytes=-500 (đuôi tệp)");
    kiem(!storage::parseRangeHeader("bytes=9999-", 5000, range, khongDuoc) && khongDuoc,
         "Range vượt kích thước tệp bị từ chối");
    kiemBang(storage::makeContentRange({100, 199}, 5000), "bytes 100-199/5000",
             "tiêu đề Content-Range");
}

// ---------------------------------------------------------------------------
void testMysqlThoat() {
    nhom("MySQL: thoát chuỗi");
    using db::MysqlConnection;
    kiemBang(MysqlConnection::quote("bình thường"), "'bình thường'", "chuỗi thường");
    kiemBang(MysqlConnection::quote("O'Reilly"), "'O\\'Reilly'", "dấu nháy đơn");
    kiemBang(MysqlConnection::quote("a\\b"), "'a\\\\b'", "dấu gạch chéo ngược");
    kiemBang(MysqlConnection::quote("dòng\nmới"), "'dòng\\nmới'", "xuống dòng");
    kiemBang(MysqlConnection::quote(std::string("nul\0byte", 8)), "'nul\\0byte'", "byte NUL");
    kiemBang(MysqlConnection::quoteBlob(fromHex("deadbeef")), "X'deadbeef'", "dữ liệu nhị phân");
    // Thử một chuỗi tấn công tiêm SQL kinh điển.
    std::string tanCong = "'; DROP TABLE ttd_entries; --";
    std::string thoat = MysqlConnection::quote(tanCong);
    kiem(thoat == "'\\'; DROP TABLE ttd_entries; --'", "vô hiệu hoá chuỗi tiêm SQL", thoat);
}

// ---------------------------------------------------------------------------
void testCoSoDuLieu() {
    nhom("Cơ sở dữ liệu (SQLite)");
    std::string thuMuc = "ttd-selftest-tmp";
    removeDirectoryRecursive(thuMuc);
    ensureDirectoryExists(thuMuc);

    db::DatabaseConfig cfg;
    cfg.kind = "sqlite";
    cfg.sqlitePath = joinPath(thuMuc, "test.db");
    std::string loi;
    auto database = db::createDatabase(cfg, loi);
    kiem(database != nullptr, "tạo được đối tượng cơ sở dữ liệu", loi);
    if (!database) return;
    kiem(database->open(loi), "mở cơ sở dữ liệu", loi);
    kiem(database->migrate(loi), "tạo bảng", loi);

    db::FileEntry thuMucGoc;
    thuMucGoc.name = "Tài liệu";
    thuMucGoc.path = "/Tài liệu";
    thuMucGoc.isFolder = true;
    kiem(database->createEntry(thuMucGoc, loi), "tạo thư mục", loi);
    kiem(thuMucGoc.id > 0, "thư mục có mã tự tăng");

    db::FileEntry tep;
    tep.parentId = thuMucGoc.id;
    tep.name = "Báo cáo quý 4.pdf";
    tep.path = "/Tài liệu/Báo cáo quý 4.pdf";
    tep.size = 12345678;
    tep.mimeType = "application/pdf";
    tep.sha256 = std::string(64, 'a');
    tep.quickHash = std::string(64, 'b');
    tep.chunkCount = 3;
    kiem(database->createEntry(tep, loi), "tạo tệp có tên tiếng Việt", loi);

    db::FileEntry doc;
    kiem(database->getEntryByPath("/Tài liệu/Báo cáo quý 4.pdf", doc, loi),
         "tìm theo đường dẫn có dấu", loi);
    kiemBang(doc.name, "Báo cáo quý 4.pdf", "tên đọc lại đúng");
    kiem(doc.size == 12345678, "kích thước đọc lại đúng");

    std::vector<db::FileEntry> trung;
    kiem(database->findByHash(std::string(64, 'a'), trung, loi) && trung.size() == 1,
         "tìm theo băm SHA-256");
    trung.clear();
    kiem(database->findByQuickHash(std::string(64, 'b'), 12345678, trung, loi) &&
             trung.size() == 1,
         "tìm theo băm nhanh");

    db::ChunkEntry mang;
    mang.fileId = tep.id;
    mang.index = 0;
    mang.offset = 0;
    mang.size = 5000000;
    mang.messageId = 999;
    mang.documentId = 12345;
    mang.accessHash = -998877;
    mang.fileReferenceHex = "aabbccdd";
    mang.dcId = 2;
    mang.accountId = 1;
    kiem(database->addChunk(mang, loi), "ghi thông tin mảnh", loi);

    std::vector<db::ChunkEntry> dsMang;
    kiem(database->listChunks(tep.id, dsMang, loi) && dsMang.size() == 1, "đọc danh sách mảnh");
    kiem(dsMang[0].accessHash == -998877, "access_hash âm giữ nguyên");
    kiemBang(dsMang[0].fileReferenceHex, "aabbccdd", "file_reference giữ nguyên");

    // Khi tài khoản đã tải lên không còn dùng được (bị Telegram khoá chẳng
    // hạn), tài khoản khác hỏi lại Telegram theo message_id và nhận về
    // access_hash của RIÊNG nó. Ghi lại thì phải ghi kèm account_id, không thì
    // lần đọc sau lại đưa hash của người này cho người kia dùng.
    kiem(database->updateChunkReference(dsMang[0].id, "11223344", 555000111, 5, 7, loi),
         "ghi lại tham chiếu mảnh sau khi đổi tài khoản", loi);
    dsMang.clear();
    kiem(database->listChunks(tep.id, dsMang, loi) && dsMang.size() == 1, "đọc lại mảnh");
    kiemBang(dsMang[0].fileReferenceHex, "11223344", "file_reference mới được lưu");
    kiem(dsMang[0].accessHash == 555000111, "access_hash mới được lưu");
    kiem(dsMang[0].dcId == 5, "dc_id mới được lưu");
    kiem(dsMang[0].accountId == 7, "account_id đi kèm access_hash mới");
    kiem(dsMang[0].messageId == 999 && dsMang[0].documentId == 12345,
         "message_id và document_id không đổi khi làm mới tham chiếu");
    // Trả lại như cũ cho các phép kiểm sau.
    kiem(database->updateChunkReference(dsMang[0].id, "aabbccdd", -998877, 2, 1, loi),
         "khôi phục tham chiếu mảnh", loi);

    db::ListOptions opts;
    opts.parentId = thuMucGoc.id;
    std::vector<db::FileEntry> ds;
    kiem(database->listEntries(opts, ds, loi) && ds.size() == 1, "liệt kê thư mục");

    // Tìm kiếm không phân biệt hoa thường.
    db::ListOptions tim;
    tim.search = "BÁO CÁO";
    ds.clear();
    database->listEntries(tim, ds, loi);
    kiem(ds.size() == 1, "tìm kiếm không phân biệt hoa thường");

    // Đổi đường dẫn cả cây con.
    kiem(database->updatePathsUnder("/Tài liệu", "/Kho lưu trữ", loi), "đổi đường dẫn cây con");
    db::FileEntry sauKhiDoi;
    kiem(database->getEntryByPath("/Kho lưu trữ/Báo cáo quý 4.pdf", sauKhiDoi, loi),
         "đường dẫn con đã đổi theo", loi);

    db::UserEntry nd;
    nd.username = "thu";
    nd.displayName = "Thư ký";
    nd.passwordHash = "pbkdf2-sha256$1000$aa$bb";
    kiem(database->createUser(nd, loi), "tạo người dùng", loi);
    db::UserEntry docNd;
    kiem(database->getUserByName("THU", docNd, loi), "tìm người dùng không phân biệt hoa thường");

    kiem(database->setSetting("thu", "nghiem", loi), "ghi cài đặt");
    std::string giaTri;
    kiem(database->getSetting("thu", giaTri, loi) && giaTri == "nghiem", "đọc cài đặt");

    db::StorageStats tk;
    kiem(database->stats(tk, loi) && tk.fileCount == 1 && tk.totalBytes == 12345678,
         "thống kê dung lượng");
    kiem(tk.physicalBytes == 5000000 && tk.uniqueChunkCount == 1,
         "dung lượng thật tính theo mảnh");

    // Tệp thứ hai trùng nội dung: dùng chung đúng document_id của mảnh cũ nên
    // dung lượng thật không đổi, chỉ số dòng mảnh tăng.
    // Lúc này cây đã đổi tên thành "/Kho lưu trữ" ở phép kiểm tra phía trên.
    db::FileEntry banSao;
    banSao.parentId = thuMucGoc.id;
    banSao.name = "Báo cáo quý 4 (bản sao).pdf";
    banSao.path = "/Kho lưu trữ/Báo cáo quý 4 (bản sao).pdf";
    banSao.size = 12345678;
    banSao.mimeType = "application/pdf";
    banSao.sha256 = std::string(64, 'a');
    banSao.quickHash = std::string(64, 'b');
    banSao.chunkCount = 3;
    kiem(database->createEntry(banSao, loi), "tạo tệp liên kết trùng nội dung", loi);
    db::ChunkEntry mangDung = mang;
    mangDung.fileId = banSao.id;
    kiem(database->addChunk(mangDung, loi), "ghi mảnh dùng chung", loi);

    db::StorageStats tk2;
    kiem(database->stats(tk2, loi), "thống kê lại sau khi liên kết", loi);
    kiem(tk2.totalBytes == 24691356 && tk2.fileCount == 2, "kích thước trên giấy tờ cộng dồn");
    kiem(tk2.chunkCount == 2, "số dòng mảnh tăng khi liên kết");
    kiem(tk2.physicalBytes == 5000000 && tk2.uniqueChunkCount == 1,
         "khử trùng lặp: dung lượng thật không đổi");

    // Ghi sau khi đã đóng phải báo đúng lý do. SQLite trả "out of memory" khi
    // con trỏ CSDL là null — nghe như hết RAM, rất đánh lạc hướng.
    database->close();
    db::SessionKeyEntry khoa;
    khoa.accountId = 1;
    khoa.dcId = 2;
    khoa.authKeyHex = std::string(512, 'a');
    khoa.serverSalt = 12345;
    std::string loiDong;
    kiem(!database->saveSessionKey(khoa, loiDong), "ghi vào CSDL đã đóng phải thất bại");
    kiem(loiDong.find("out of memory") == std::string::npos,
         "lỗi CSDL đã đóng không nói nhầm là hết bộ nhớ", loiDong);
    kiem(!loiDong.empty(), "lỗi CSDL đã đóng có nội dung rõ ràng", loiDong);

    removeDirectoryRecursive(thuMuc);
}

// ---------------------------------------------------------------------------
// Nơi lưu trữ giả lập đúng kịch bản "tài khoản đã tải lên bị Telegram khoá":
// mỗi lần đọc nó hỏi lại theo message_id và trả về access_hash + tài khoản MỚI
// qua tham số `loc` (vừa vào vừa ra).
class BackendDoiTaiKhoan : public tg::StorageBackend {
public:
    Bytes noiDung;
    int soLanDoc = 0;
    int64_t hashMoi = 0;
    int taiKhoanMoi = 0;

    std::string name() const override { return "giả lập"; }
    bool ready(std::string&) const override { return true; }
    std::unique_ptr<tg::ChunkWriter> beginChunk(uint64_t, const std::string&,
                                                std::string& error) override {
        error = "bản giả lập không ghi";
        return nullptr;
    }
    bool readRange(tg::ChunkLocation& loc, uint64_t offset, uint32_t limit, Bytes& out,
                   std::string& error) override {
        soLanDoc++;
        if (offset >= noiDung.size()) {
            error = "vượt quá cuối mảnh";
            return false;
        }
        size_t lay = std::min<size_t>(limit, noiDung.size() - static_cast<size_t>(offset));
        out.assign(noiDung.begin() + static_cast<long>(offset),
                   noiDung.begin() + static_cast<long>(offset) + static_cast<long>(lay));
        if (hashMoi != 0) {
            loc.accessHash = hashMoi;
            loc.accountId = taiKhoanMoi;
            loc.fileReference = Bytes{0xde, 0xad, 0xbe, 0xef};
        }
        return true;
    }
    bool removeChunks(const std::vector<tg::ChunkLocation>&, std::string&) override {
        return true;
    }
    tg::BackendStats stats() const override { return tg::BackendStats(); }
};

void testDoiTaiKhoanKhiDoc() {
    nhom("Đọc bằng tài khoản khác khi tài khoản cũ bị khoá");

    std::string thuMuc = "ttd_test_doi_tk";
    removeDirectoryRecursive(thuMuc);
    ensureDirectoryExists(thuMuc);
    std::string loi;
    db::DatabaseConfig cfg;
    cfg.kind = "sqlite";
    cfg.sqlitePath = joinPath(thuMuc, "test.db");
    auto database = db::createDatabase(cfg, loi);
    kiem(database != nullptr, "tạo được CSDL tạm", loi);
    if (!database) return;
    kiem(database->open(loi), "mở CSDL tạm", loi);
    kiem(database->migrate(loi), "tạo lược đồ", loi);

    db::FileEntry tep;
    tep.name = "phim.mkv";
    tep.path = "/phim.mkv";
    tep.parentId = 0;
    tep.isFolder = false;
    tep.size = 64;
    tep.ownerId = 1;
    kiem(database->createEntry(tep, loi), "tạo tệp thử", loi);

    db::ChunkEntry mang;
    mang.fileId = tep.id;
    mang.index = 0;
    mang.offset = 0;
    mang.size = 64;
    mang.messageId = 4242;
    mang.documentId = 777;
    mang.accessHash = 111111;      // access_hash của tài khoản #1 (sắp bị khoá)
    mang.fileReferenceHex = "00112233";
    mang.dcId = 2;
    mang.accountId = 1;
    kiem(database->addChunk(mang, loi), "ghi mảnh của tài khoản #1", loi);

    BackendDoiTaiKhoan backend;
    backend.noiDung.assign(64, 0x5a);
    backend.hashMoi = 999999;      // access_hash mà tài khoản #2 hỏi lại được
    backend.taiKhoanMoi = 2;

    storage::StorageEngine engine(*database, backend, Config::instance());
    Bytes ra;
    kiem(engine.readFileRange(tep, 0, 64, ra, loi) == 64, "đọc đủ 64 byte qua tài khoản khác",
         loi);
    kiem(ra.size() == 64 && ra[0] == 0x5a && ra[63] == 0x5a, "nội dung đọc về đúng");

    // Đây mới là điều cần chứng minh: tham chiếu mới phải được ghi lại, kèm
    // account_id của tài khoản đã lấy nó. Trước đây readRange nhận `loc` là
    // const nên khối ghi lại này không bao giờ chạy.
    std::vector<db::ChunkEntry> dsMang;
    kiem(database->listChunks(tep.id, dsMang, loi) && dsMang.size() == 1, "đọc lại mảnh", loi);
    kiem(dsMang[0].accessHash == 999999, "access_hash mới đã được lưu");
    kiem(dsMang[0].accountId == 2, "account_id mới đã được lưu");
    kiemBang(dsMang[0].fileReferenceHex, "deadbeef", "file_reference mới đã được lưu");
    kiem(dsMang[0].messageId == 4242, "message_id giữ nguyên — đây là neo để hỏi lại");

    // Lần đọc sau lấy từ bộ đệm khối, không gọi lại nơi lưu trữ.
    int truoc = backend.soLanDoc;
    Bytes ra2;
    kiem(engine.readFileRange(tep, 0, 64, ra2, loi) == 64, "đọc lần hai", loi);
    kiem(backend.soLanDoc == truoc, "lần đọc sau dùng bộ đệm, không hỏi lại");

    database->close();
    removeDirectoryRecursive(thuMuc);
}

// ---------------------------------------------------------------------------
void testLoiBaoCho() {
    nhom("Nhận diện lỗi \"chờ chút rồi làm lại\"");
    int giay = -1;

    // Đúng chuỗi lỗi gặp thật khi tải bằng WebDAV bằng tài khoản thường.
    kiem(storage::laLoiTamThoi(
             "Tải phần 121/1856 thất bại: Lỗi máy chủ: 420 FLOOD_PREMIUM_WAIT_3", giay),
         "nhận ra FLOOD_PREMIUM_WAIT lẫn trong câu tiếng Việt");
    kiem(giay == 3, "đọc đúng 3 giây từ FLOOD_PREMIUM_WAIT_3");

    kiem(storage::laLoiTamThoi("Lỗi máy chủ: 420 FLOOD_WAIT_42", giay), "nhận ra FLOOD_WAIT");
    kiem(giay == 42, "đọc đúng 42 giây");

    kiem(storage::laLoiTamThoi("420 SLOWMODE_WAIT_10", giay), "nhận ra SLOWMODE_WAIT");
    kiem(giay == 10, "đọc đúng 10 giây từ SLOWMODE_WAIT_10");

    // FLOOD_PREMIUM_WAIT_ phải được thử TRƯỚC FLOOD_WAIT_, không thì tiền tố
    // ngắn hơn khớp trước và số giây đọc ra sai chỗ.
    kiem(storage::laLoiTamThoi("FLOOD_PREMIUM_WAIT_7", giay) && giay == 7,
         "tiền tố dài được ưu tiên, không bị FLOOD_WAIT_ giành trước");

    // Những lỗi KHÔNG phải loại chờ thì đừng nhận vơ — giữ phiên lại vô ích.
    kiem(!storage::laLoiTamThoi("400 CHANNEL_INVALID", giay), "CHANNEL_INVALID không phải lỗi chờ");
    kiem(!storage::laLoiTamThoi("401 AUTH_KEY_UNREGISTERED", giay),
         "AUTH_KEY_UNREGISTERED không phải lỗi chờ");
    kiem(!storage::laLoiTamThoi("Không còn dung lượng trên đĩa", giay),
         "hết đĩa không phải lỗi chờ");
    kiem(!storage::laLoiTamThoi("", giay), "chuỗi rỗng không phải lỗi chờ");

    // Có định danh mà không có số thì vẫn là lỗi chờ, chỉ là không biết chờ bao lâu.
    kiem(storage::laLoiTamThoi("420 FLOOD_WAIT", giay) && giay == 0,
         "thiếu số giây vẫn nhận là lỗi chờ, giây trả về 0");

    // Lỗi NỘI BỘ của Telegram (mã 500) cũng là tạm thời — gửi lại là xong.
    // Đúng chuỗi gặp thật khi tải tệp 58 GB, làm mất 22 mảnh đã đẩy lên.
    kiem(storage::laLoiTamThoi(
             "Tải phần 3059/3400 thất bại: Lỗi máy chủ: 500 RPC_CALL_FAIL", giay),
         "nhận ra RPC_CALL_FAIL là lỗi tạm thời");
    kiem(storage::laLoiTamThoi("500 RPC_MCGET_FAIL", giay), "nhận ra RPC_MCGET_FAIL");
    kiem(storage::laLoiTamThoi("500 INTERNAL_SERVER_ERROR", giay),
         "nhận ra INTERNAL_SERVER_ERROR");
    kiem(storage::laLoiTamThoi("500 WORKER_BUSY_TOO_LONG_RETRY", giay),
         "nhận ra WORKER_BUSY_TOO_LONG_RETRY");

    // Vẫn phải phân biệt được với lỗi thật sự vĩnh viễn.
    kiem(!storage::laLoiTamThoi("400 FILE_PARTS_INVALID", giay),
         "FILE_PARTS_INVALID không phải lỗi tạm thời");
    kiem(!storage::laLoiTamThoi("403 CHAT_WRITE_FORBIDDEN", giay),
         "CHAT_WRITE_FORBIDDEN không phải lỗi tạm thời");
}

// ---------------------------------------------------------------------------
void testCauHinh() {
    nhom("Cấu hình");
    Config& cfg = Config::instance();
    Json j = Json::object();
    Json st = Json::object();
    st.set("chunk_size", std::string("500MB"));
    st.set("buffer_mode", std::string("memory"));
    st.set("parallel_chunks", static_cast<int64_t>(4));
    j.set("storage", st);
    std::string loi;
    kiem(cfg.applyJson(j, loi), "áp dụng cấu hình JSON", loi);
    kiem(cfg.storage.chunkSize == 500ull * 1024 * 1024, "kích thước mảnh 500MB");
    kiemBang(cfg.storage.bufferMode, "memory", "chế độ đệm");
    kiem(cfg.storage.parallelChunks == 4, "số mảnh song song");

    // Giá trị vô lý phải bị siết lại.
    Json xau = Json::object();
    Json st2 = Json::object();
    st2.set("chunk_size", std::string("100000MB"));
    st2.set("buffer_mode", std::string("bay-ba"));
    st2.set("parallel_chunks", static_cast<int64_t>(9999));
    xau.set("storage", st2);
    cfg.applyJson(xau, loi);
    kiem(cfg.storage.chunkSize <= 1900ull * 1024 * 1024, "chặn mảnh vượt giới hạn Telegram");
    kiemBang(cfg.storage.bufferMode, "stream", "chế độ đệm sai → về mặc định");
    kiem(cfg.storage.parallelChunks <= 16, "chặn số mảnh song song quá lớn");

    Json ra = cfg.toJson();
    kiem(ra.has("storage") && ra.has("telegram") && ra.has("database") && ra.has("server"),
         "xuất cấu hình đủ các nhóm");
}

// ---------------------------------------------------------------------------
// initConnection là chỗ Telegram đọc api_id. Sai một byte ở đây là nhận ngay
// CONNECTION_API_ID_INVALID, nên kiểm tận byte trên dây.
void testInitConnection() {
    nhom("initConnection / api_id");
    using namespace tg;

    TlSchema schema;
    std::string mtproto, apiSchema;
    if (assets::find("schema/mtproto.tl", mtproto)) schema.load(mtproto, nullptr);
    if (assets::find("schema/api.tl", apiSchema)) schema.load(apiSchema, nullptr);

    const TlConstructor* ctor = schema.byName("initConnection");
    kiem(ctor != nullptr, "schema có initConnection");
    if (!ctor) return;
    kiem(ctor->args.size() >= 2 && ctor->args[0].isFlagsInt,
         "trường đầu tiên của initConnection là flags:#");
    kiem(ctor->args.size() >= 2 && ctor->args[1].name == "api_id",
         "api_id nằm ngay sau flags");

    TlCodec codec(schema);
    const int32_t kApiId = 25008104;
    TlValue init = TlValue::makeObject("initConnection");
    init.setInt("api_id", kApiId);
    init.setBytes("device_model", std::string("PC"));
    init.setBytes("system_version", std::string("Windows 10"));
    init.setBytes("app_version", std::string("1.0.0"));
    init.setBytes("system_lang_code", std::string("vi"));
    init.setBytes("lang_pack", std::string());
    init.setBytes("lang_code", std::string("vi"));
    init.set("query", TlValue::makeObject("help.getConfig"));

    TlWriter w;
    std::string loi;
    kiem(codec.serialize(init, w, loi), "tuần tự hoá initConnection", loi);

    // Trên dây: [ctor:4][flags:4][api_id:4]…
    const Bytes& b = w.buffer();
    kiem(b.size() > 12, "gói initConnection đủ dài");
    if (b.size() > 12) {
        auto doc32 = [&](size_t o) {
            return static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
                   (static_cast<uint32_t>(b[o + 2]) << 16) |
                   (static_cast<uint32_t>(b[o + 3]) << 24);
        };
        kiem(doc32(0) == ctor->id, "4 byte đầu là định danh hàm dựng");
        kiem(doc32(4) == 0, "flags = 0 vì không có proxy/params");
        kiem(static_cast<int32_t>(doc32(8)) == kApiId,
             "api_id nằm đúng byte thứ 8 và đúng giá trị");
    }

    // Giải mã ngược lại phải ra đúng api_id.
    TlReader r(w.buffer());
    TlValue lai;
    kiem(codec.deserialize(r, lai, loi), "giải mã lại initConnection", loi);
    kiem(lai["api_id"].asInt() == kApiId, "api_id đọc lại khớp");

    // Lỗi thật gặp phải: api_id điền sau khi ứng dụng đã chạy thì pool vẫn giữ
    // bản cũ (0) và mọi tài khoản gửi api_id sai.
    AppInfo cu;
    cu.apiId = 0;
    cu.layer = schema.layer();
    AccountPool pool(schema, cu);
    TgAccountConfig ac;
    ac.id = 1;
    ac.label = "Tuan";
    ac.homeDc = 2;
    TgAccount* acc = pool.addAccount(ac);
    kiem(acc != nullptr && acc->appApiId() == 0, "tài khoản ban đầu mang api_id 0");

    AppInfo moi = cu;
    moi.apiId = kApiId;
    moi.apiHash = "0123456789abcdef0123456789abcdef";
    pool.updateAppInfo(moi);
    kiem(pool.appInfo().apiId == kApiId, "pool nhận api_id mới");
    kiem(acc != nullptr && acc->appApiId() == kApiId,
         "tài khoản đang có được cập nhật api_id");

    TgAccountConfig ac2;
    ac2.id = 2;
    ac2.label = "Tuan 2";
    ac2.homeDc = 2;
    TgAccount* acc2 = pool.addAccount(ac2);
    kiem(acc2 != nullptr && acc2->appApiId() == kApiId,
         "tài khoản thêm sau cũng mang api_id mới");
}

// ---------------------------------------------------------------------------
void testPhienBan() {
    nhom("Phiên bản");
    kiem(std::strlen(version::kVersion) >= 5, "chuỗi phiên bản có nội dung");
    kiem(version::kBuildNumber > 0, "số build lớn hơn 0",
         std::to_string(version::kBuildNumber));
    kiemBang(version::kAppName, "Tuấn's Telegram Disk", "tên ứng dụng");
    kiemBang(version::kAppFooter, "Thiết kế bởi Tuandethuong.", "dòng chân trang");
    kiem(std::strlen(version::kBuildTimeUtc) >= 19, "thời điểm biên dịch");
}

}  // namespace

int main() {
    std::printf("\n\033[1m╔══════════════════════════════════════════════════════════╗\033[0m\n");
    std::printf("\033[1m║   Tự kiểm tra — Tuấn's Telegram Disk %-19s║\033[0m\n", version::kVersion);
    std::printf("\033[1m╚══════════════════════════════════════════════════════════╝\033[0m\n");

    // Tắt nhật ký ra màn hình để kết quả dễ đọc.
    Logger::instance().configure(LogLevel::Error, "", 0, 1, false, 100);

    testChuoi();
    testThoiGian();
    testJson();
    testBam();
    testAes();
    testSoLon();
    testMtproto();
    testTl();
    testNen();
    testHttp();
    testMysqlThoat();
    testCoSoDuLieu();
    testDoiTaiKhoanKhiDoc();
    testLoiBaoCho();
    testCauHinh();
    testInitConnection();
    testPhienBan();

    std::printf("\n");
    if (g_fail == 0) {
        std::printf("\033[1;32m✓ Tất cả %d phép kiểm tra đều đạt.\033[0m\n\n", g_pass);
        return 0;
    }
    std::printf("\033[1;31m✗ %d/%d phép kiểm tra thất bại.\033[0m\n\n", g_fail,
                g_pass + g_fail);
    return 1;
}
