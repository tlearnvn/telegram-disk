// Phần mật mã của MTProto 2.0: dẫn xuất khoá, mã hoá/giải mã gói tin,
// đệm RSA_PAD cho bước bắt tay và các hàm băm phụ trợ.
#pragma once

#include <cstdint>
#include <string>

#include "common/strutil.h"
#include "crypto/rsa.h"

namespace ttd {
namespace tg {

// Khoá xác thực 256 byte dùng chung cho một tài khoản trên một trung tâm dữ liệu.
struct AuthKey {
    Bytes key;        // 256 byte
    int64_t keyId = 0;
    int64_t serverSalt = 0;

    bool valid() const { return key.size() == 256 && keyId != 0; }
    void computeKeyId();
    // 8 byte đầu của SHA1(auth_key) — dùng trong new_nonce_hash.
    Bytes auxHash() const;
};

// Dẫn xuất khoá AES theo MTProto 2.0.
//   fromServer = false: gói tin do máy khách gửi (x = 0)
//   fromServer = true : gói tin do máy chủ gửi  (x = 8)
void deriveAesKeyIv(const Bytes& authKey, const Bytes& msgKey, bool fromServer, Bytes& aesKey,
                    Bytes& aesIv);

// Đóng gói và mã hoá một thông điệp.
//   payload = phần thân đã tuần tự hoá (không gồm tiêu đề)
// Kết quả: auth_key_id(8) || msg_key(16) || dữ liệu đã mã hoá
Bytes encryptMessage(const AuthKey& authKey, int64_t sessionId, int64_t msgId, int32_t seqNo,
                     const Bytes& payload);

struct DecryptedMessage {
    int64_t salt = 0;
    int64_t sessionId = 0;
    int64_t msgId = 0;
    int32_t seqNo = 0;
    Bytes body;
    bool ok = false;
    std::string error;
};

// Giải mã gói tin nhận từ máy chủ và kiểm tra toàn vẹn.
DecryptedMessage decryptMessage(const AuthKey& authKey, const Bytes& packet);

// Đệm RSA theo sơ đồ RSA_PAD mà Telegram yêu cầu cho req_DH_params.
bool rsaPadEncrypt(const Bytes& data, const crypto::RsaPublicKey& key, Bytes& out);

// Khoá AES tạm thời dùng ở bước server_DH_params (dựa trên SHA1).
void tmpAesKeyIv(const Bytes& newNonce, const Bytes& serverNonce, Bytes& aesKey, Bytes& aesIv);

// new_nonce_hash1/2/3 = SHA1(new_nonce | which | auth_key_aux_hash)[4:20]
Bytes newNonceHash(const Bytes& newNonce, uint8_t which, const Bytes& authKeyAuxHash);

// Phân tích số pq thành hai thừa số nguyên tố (thuật toán Pollard-Brent).
bool factorizePq(uint64_t pq, uint64_t& p, uint64_t& q);

// Bộ sinh mã thông điệp (msg_id) đảm bảo tăng dần và chia hết cho 4.
class MsgIdGenerator {
public:
    // timeOffsetSeconds: chênh lệch giữa đồng hồ máy chủ và máy này.
    int64_t next(int64_t timeOffsetSeconds);

private:
    int64_t last_ = 0;
};

}  // namespace tg
}  // namespace ttd
