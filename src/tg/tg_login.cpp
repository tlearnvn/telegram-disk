#include "tg/tg_login.h"

#include "common/logging.h"
#include "common/strutil.h"
#include "common/timeutil.h"
#include "crypto/srp.h"

namespace ttd {
namespace tg {

namespace {
constexpr const char* kTag = "tg.login";

std::string normalizePhone(const std::string& phone) {
    std::string out;
    for (char c : phone)
        if (c >= '0' && c <= '9') out.push_back(c);
    return out;
}

std::string describeCodeType(const TlValue& sentCode) {
    const TlValue& type = sentCode["type"];
    if (type.is("auth.sentCodeTypeApp")) return "ứng dụng Telegram trên thiết bị khác";
    if (type.is("auth.sentCodeTypeSms")) return "tin nhắn SMS";
    if (type.is("auth.sentCodeTypeCall")) return "cuộc gọi thoại";
    if (type.is("auth.sentCodeTypeFlashCall")) return "cuộc gọi nhỡ";
    if (type.is("auth.sentCodeTypeMissedCall")) return "cuộc gọi nhỡ";
    if (type.is("auth.sentCodeTypeEmailCode")) return "thư điện tử";
    if (type.is("auth.sentCodeTypeFragmentSms")) return "Fragment SMS";
    return "Telegram";
}

std::string friendlyError(const RpcError& err) {
    const std::string& m = err.message;
    if (m == "PHONE_NUMBER_INVALID") return "Số điện thoại không hợp lệ.";
    if (m == "PHONE_NUMBER_BANNED") return "Số điện thoại này đã bị Telegram khoá.";
    if (m == "PHONE_CODE_INVALID") return "Mã xác thực không đúng.";
    if (m == "PHONE_CODE_EXPIRED") return "Mã xác thực đã hết hạn, hãy yêu cầu mã mới.";
    if (m == "PHONE_CODE_EMPTY") return "Chưa nhập mã xác thực.";
    if (m == "PASSWORD_HASH_INVALID") return "Mật khẩu hai lớp không đúng.";
    if (m == "API_ID_INVALID") return "api_id hoặc api_hash không hợp lệ.";
    if (m == "API_ID_PUBLISHED_FLOOD")
        return "api_id này bị Telegram hạn chế do dùng quá phổ biến. Hãy tạo api_id riêng tại "
               "my.telegram.org.";
    if (m == "AUTH_RESTART") return "Telegram yêu cầu bắt đầu lại quá trình đăng nhập.";
    if (m == "SESSION_PASSWORD_NEEDED") return "Tài khoản có bật xác thực hai lớp.";
    if (startsWith(m, "FLOOD_WAIT_"))
        return "Telegram yêu cầu chờ " + formatDuration(err.value) + " trước khi thử lại.";
    return "Telegram báo lỗi: " + m;
}

}  // namespace

LoginResult loginSendCode(TgAccount& account, const std::string& phone) {
    LoginResult res;
    std::string clean = normalizePhone(phone);
    if (clean.size() < 6) {
        res.message = "Số điện thoại không hợp lệ.";
        return res;
    }

    std::string error;
    if (!account.connect(error)) {
        res.message = "Không kết nối được tới Telegram: " + error;
        return res;
    }

    TlValue settings = TlValue::makeObject("codeSettings");

    TlValue req = TlValue::makeObject("auth.sendCode");
    req.setBytes("phone_number", clean);
    req.setInt("api_id", account.appApiId());
    req.setBytes("api_hash", account.appApiHash());
    req.set("settings", settings);

    InvokeResult ir = account.invoke(req);
    if (!ir.ok) {
        if (!ir.error.empty()) {
            int dc = 0;
            if (startsWith(ir.error.message, "PHONE_MIGRATE_")) {
                dc = ir.error.value;
                LOG_INFO(kTag, "Số %s thuộc DC%d, thử lại", clean.c_str(), dc);
                ir = account.invoke(req, dc);
            }
        }
    }
    if (!ir.ok) {
        res.message = ir.error.empty() ? ("Lỗi kết nối: " + ir.localError)
                                       : friendlyError(ir.error);
        return res;
    }

    if (!ir.value.is("auth.sentCode")) {
        res.message = "Telegram trả về phản hồi không mong đợi: " + ir.value.ctorName();
        return res;
    }

    res.ok = true;
    res.needsCode = true;
    res.state.phone = clean;
    res.state.phoneCodeHash = ir.value["phone_code_hash"].asString();
    res.state.codeSent = true;
    res.state.codeTypeText = describeCodeType(ir.value);
    res.state.codeLength = ir.value["type"]["length"].asInt(5);
    res.state.createdAt = nowUnix();
    res.message = "Đã gửi mã xác thực qua " + res.state.codeTypeText + ".";
    LOG_INFO(kTag, "Đã gửi mã xác thực tới %s qua %s", clean.c_str(),
             res.state.codeTypeText.c_str());
    return res;
}

LoginResult loginSubmitCode(TgAccount& account, const LoginState& state,
                            const std::string& code) {
    LoginResult res;
    res.state = state;
    std::string clean;
    for (char c : code)
        if (c >= '0' && c <= '9') clean.push_back(c);
    if (clean.empty()) {
        res.message = "Chưa nhập mã xác thực.";
        res.needsCode = true;
        return res;
    }

    TlValue req = TlValue::makeObject("auth.signIn");
    req.setBytes("phone_number", state.phone);
    req.setBytes("phone_code_hash", state.phoneCodeHash);
    req.setBytes("phone_code", clean);

    InvokeResult ir = account.invoke(req);
    if (!ir.ok) {
        if (!ir.error.empty() && ir.error.message == "SESSION_PASSWORD_NEEDED") {
            res.ok = true;
            res.needsPassword = true;
            res.state.needsPassword = true;
            // Lấy gợi ý mật khẩu.
            TlValue getPwd = TlValue::makeObject("account.getPassword");
            InvokeResult pr = account.invoke(getPwd);
            if (pr.ok || pr.partial) res.state.passwordHint = pr.value["hint"].asString();
            res.message = "Tài khoản có bật xác thực hai lớp. Hãy nhập mật khẩu.";
            return res;
        }
        res.message = ir.error.empty() ? ("Lỗi kết nối: " + ir.localError)
                                       : friendlyError(ir.error);
        res.needsCode = true;
        return res;
    }

    if (ir.value.is("auth.authorizationSignUpRequired")) {
        res.needsSignUp = true;
        res.message =
            "Số điện thoại này chưa có tài khoản Telegram. Hãy đăng ký bằng ứng dụng Telegram "
            "trước, rồi thêm lại vào đây.";
        return res;
    }

    account.setAuthorized(true);
    std::string error;
    if (!account.fetchSelf(error, res.displayName, res.userId)) {
        LOG_WARN(kTag, "Đăng nhập xong nhưng chưa đọc được hồ sơ: %s", error.c_str());
        res.displayName = state.phone;
    }
    res.ok = true;
    res.message = "Đăng nhập thành công.";
    LOG_INFO(kTag, "Tài khoản %s đăng nhập thành công", res.displayName.c_str());
    return res;
}

LoginResult loginSubmitPassword(TgAccount& account, const std::string& password) {
    LoginResult res;
    if (password.empty()) {
        res.message = "Chưa nhập mật khẩu.";
        res.needsPassword = true;
        return res;
    }

    TlValue getPwd = TlValue::makeObject("account.getPassword");
    InvokeResult pr = account.invoke(getPwd);
    if (!pr.ok && !pr.partial) {
        res.message = "Không lấy được tham số mật khẩu: " + pr.describe();
        res.needsPassword = true;
        return res;
    }

    const TlValue& algoValue = pr.value["current_algo"];
    if (!algoValue.is("passwordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000SHA256ModPow")) {
        res.message =
            "Telegram dùng thuật toán mật khẩu chưa được hỗ trợ (" + algoValue.ctorName() + ").";
        res.needsPassword = true;
        return res;
    }

    crypto::SrpAlgo algo;
    algo.salt1 = algoValue["salt1"].asBytes();
    algo.salt2 = algoValue["salt2"].asBytes();
    algo.g = algoValue["g"].asInt();
    algo.p = algoValue["p"].asBytes();

    Bytes srpB = pr.value["srp_B"].asBytes();
    int64_t srpId = pr.value["srp_id"].asLong();

    crypto::SrpResult srp = crypto::srpCompute(algo, srpB, password);
    if (!srp.ok) {
        res.message = "Tính toán SRP thất bại: " + srp.error;
        res.needsPassword = true;
        return res;
    }

    TlValue input = TlValue::makeObject("inputCheckPasswordSRP");
    input.setLong("srp_id", srpId);
    input.setBytes("A", srp.A);
    input.setBytes("M1", srp.M1);

    TlValue req = TlValue::makeObject("auth.checkPassword");
    req.set("password", input);

    InvokeResult ir = account.invoke(req);
    if (!ir.ok) {
        res.message = ir.error.empty() ? ("Lỗi kết nối: " + ir.localError)
                                       : friendlyError(ir.error);
        res.needsPassword = true;
        return res;
    }

    account.setAuthorized(true);
    std::string error;
    if (!account.fetchSelf(error, res.displayName, res.userId))
        LOG_WARN(kTag, "Đăng nhập xong nhưng chưa đọc được hồ sơ: %s", error.c_str());
    res.ok = true;
    res.message = "Đăng nhập thành công.";
    return res;
}

bool logout(TgAccount& account, std::string& error) {
    TlValue req = TlValue::makeObject("auth.logOut");
    InvokeResult ir = account.invoke(req);
    account.setAuthorized(false);
    if (!ir.ok && !ir.partial) {
        error = ir.describe();
        return false;
    }
    return true;
}

}  // namespace tg
}  // namespace ttd
