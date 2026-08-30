// Luồng đăng nhập tài khoản Telegram bằng số điện thoại:
//   gửi mã -> nhập mã -> (nếu có) nhập mật khẩu hai lớp.
#pragma once

#include <string>

#include "tg/tg_account.h"

namespace ttd {
namespace tg {

struct LoginState {
    std::string phone;
    std::string phoneCodeHash;
    bool codeSent = false;
    bool needsPassword = false;
    std::string passwordHint;
    std::string codeTypeText;   // mô tả nơi nhận mã (ứng dụng Telegram / SMS / cuộc gọi)
    int codeLength = 5;
    int64_t createdAt = 0;
};

struct LoginResult {
    bool ok = false;
    bool needsCode = false;
    bool needsPassword = false;
    bool needsSignUp = false;
    std::string message;
    std::string displayName;
    int64_t userId = 0;
    LoginState state;
};

// Bước 1: gửi mã xác thực tới số điện thoại.
LoginResult loginSendCode(TgAccount& account, const std::string& phone);

// Bước 2: xác nhận mã.
LoginResult loginSubmitCode(TgAccount& account, const LoginState& state,
                            const std::string& code);

// Bước 3 (nếu tài khoản bật xác thực hai lớp): xác nhận mật khẩu.
LoginResult loginSubmitPassword(TgAccount& account, const std::string& password);

// Đăng xuất tài khoản khỏi Telegram (huỷ phiên phía máy chủ).
bool logout(TgAccount& account, std::string& error);

}  // namespace tg
}  // namespace ttd
