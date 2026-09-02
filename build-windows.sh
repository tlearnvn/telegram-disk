#!/usr/bin/env bash
# =============================================================================
#  Tuấn's Telegram Disk — dựng bản chạy độc lập cho Windows x64
#  Biên dịch chéo từ Linux bằng mingw-w64.
#    sudo apt install mingw-w64 cmake
#  Thiết kế bởi Tuandethuong.
# =============================================================================
set -euo pipefail

GOC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THU_MUC_BUILD="${GOC}/build-win"
THU_MUC_XUAT="${GOC}/dist/windows-x64"
KIEU="${TTD_BUILD_TYPE:-Release}"
SO_LUONG="${TTD_JOBS:-$(nproc 2>/dev/null || echo 4)}"
TU_TANG="${TTD_AUTO_BUMP:-ON}"

mau()  { printf '\033[%sm%s\033[0m\n' "$1" "$2"; }
buoc() { mau '1;36' "▸ $*"; }
xong() { mau '1;32' "✓ $*"; }
loi()  { mau '1;31' "✗ $*" >&2; }

buoc "Kiểm tra bộ biên dịch chéo mingw-w64"
if ! command -v x86_64-w64-mingw32-g++ >/dev/null 2>&1 &&
   ! command -v x86_64-w64-mingw32-g++-posix >/dev/null 2>&1; then
  loi "Chưa có mingw-w64. Cài bằng: sudo apt install mingw-w64"
  exit 1
fi
xong "$(x86_64-w64-mingw32-g++ --version 2>/dev/null | head -1 ||
        x86_64-w64-mingw32-g++-posix --version | head -1)"

buoc "Cấu hình CMake cho Windows x64 (kiểu $KIEU)"
cmake -S "$GOC" -B "$THU_MUC_BUILD" \
      -DCMAKE_TOOLCHAIN_FILE="$GOC/cmake/toolchain-mingw64.cmake" \
      -DCMAKE_BUILD_TYPE="$KIEU" \
      -DTTD_STATIC=ON \
      -DTTD_AUTO_BUMP="$TU_TANG" \
      -DTTD_BUILD_TESTS=OFF

buoc "Biên dịch với $SO_LUONG luồng"
cmake --build "$THU_MUC_BUILD" -j "$SO_LUONG"

EXE="$THU_MUC_BUILD/tuan-telegram-disk.exe"
if [ ! -f "$EXE" ]; then
  loi "Không tạo được tệp .exe"
  exit 1
fi

buoc "Kiểm tra tệp .exe chỉ phụ thuộc DLL hệ thống"
PHU_THUOC="$(x86_64-w64-mingw32-objdump -p "$EXE" | grep 'DLL Name' | awk '{print $3}' |
             sort -u | tr '\n' ' ')"
echo "   $PHU_THUOC"
for dll in $PHU_THUOC; do
  case "$(echo "$dll" | tr 'A-Z' 'a-z')" in
    kernel32.dll|msvcrt.dll|ws2_32.dll|iphlpapi.dll|bcrypt.dll|advapi32.dll|user32.dll|\
    shell32.dll|ole32.dll|oleaut32.dll|version.dll|crypt32.dll|secur32.dll) ;;
    *) loi "Phụ thuộc DLL ngoài hệ thống: $dll — bản build sẽ không chạy độc lập"; exit 1 ;;
  esac
done
xong "Chỉ dùng DLL có sẵn trong Windows"

buoc "Đóng gói vào $THU_MUC_XUAT"
rm -rf "$THU_MUC_XUAT"
mkdir -p "$THU_MUC_XUAT/schema"
cp "$EXE" "$THU_MUC_XUAT/"
cp "$GOC/schema/"*.tl "$THU_MUC_XUAT/schema/"
cp "$GOC/README.md" "$THU_MUC_XUAT/" 2>/dev/null || true
cp "$GOC/LICENSE" "$THU_MUC_XUAT/" 2>/dev/null || true
x86_64-w64-mingw32-strip --strip-unneeded "$THU_MUC_XUAT/tuan-telegram-disk.exe" 2>/dev/null || true

# Tệp .bat để bấm đúp chạy trên Windows.
printf '@echo off\r\nchcp 65001 >nul\r\ncd /d "%%~dp0"\r\ntitle Tuan Telegram Disk\r\ntuan-telegram-disk.exe %%*\r\npause\r\n' \
  > "$THU_MUC_XUAT/chay.bat"

printf '@echo off\r\nchcp 65001 >nul\r\ncd /d "%%~dp0"\r\ntuan-telegram-disk.exe --check-schema\r\npause\r\n' \
  > "$THU_MUC_XUAT/kiem-tra-schema.bat"

cat > "$THU_MUC_XUAT/CAI-DAT.txt" <<'HUONGDAN'
TUẤN'S TELEGRAM DISK — BẢN WINDOWS 64-BIT
=========================================

CÁCH CHẠY
1. Giải nén toàn bộ thư mục này ra một chỗ cố định, ví dụ C:\TelegramDisk
2. Bấm đúp vào "chay.bat"
3. Lần chạy đầu tiên, cửa sổ dòng lệnh sẽ in ra mật khẩu quản trị. Hãy chép lại.
4. Mở trình duyệt tới http://127.0.0.1:8088 rồi đăng nhập bằng tài khoản "admin".
5. Vào Cài đặt để điền api_id / api_hash, sau đó thêm tài khoản Telegram và
   chọn siêu nhóm dùng làm nơi lưu trữ.

CHẠY NGẦM NHƯ MỘT DỊCH VỤ
   Dùng NSSM (https://nssm.cc):
     nssm install TuanTelegramDisk C:\TelegramDisk\tuan-telegram-disk.exe
     nssm set TuanTelegramDisk AppDirectory C:\TelegramDisk
     nssm start TuanTelegramDisk

TƯỜNG LỬA
   Lần đầu chạy, Windows sẽ hỏi cho phép truy cập mạng. Chọn "Cho phép" nếu
   muốn truy cập ổ đĩa từ máy khác trong cùng mạng nội bộ.

GẮN Ổ ĐĨA VÀO WINDOWS EXPLORER (WebDAV)
   This PC → Map network drive → http://127.0.0.1:8088/webdav
   Đăng nhập bằng chính tài khoản web.
   Lưu ý: Windows chỉ chấp nhận WebDAV qua HTTP với máy cục bộ; truy cập từ máy
   khác nên đặt ứng dụng sau một proxy có HTTPS.

TỆP DỮ LIỆU
   config.json  — cấu hình (sửa được trong giao diện web)
   data\        — cơ sở dữ liệu SQLite, tệp tạm, bộ đệm
   logs\        — nhật ký
   schema\      — mô tả giao thức Telegram; thay api.tl mới vào đây khi cần

Thiết kế bởi Tuandethuong.
HUONGDAN

PHIEN_BAN="$(cat "$GOC/VERSION")"
SO_BUILD="$(cat "$GOC/BUILD_NUMBER" 2>/dev/null || echo 0)"
GOI="$GOC/dist/tuan-telegram-disk-${PHIEN_BAN}-windows-x64.zip"
rm -f "$GOI"
if command -v zip >/dev/null 2>&1; then
  (cd "$GOC/dist" && zip -qr "$GOI" windows-x64)
else
  tar -czf "${GOI%.zip}.tar.gz" -C "$GOC/dist" windows-x64
  GOI="${GOI%.zip}.tar.gz"
fi

echo
xong "Xong! Phiên bản $PHIEN_BAN (build $SO_BUILD)"
echo "   Tệp thực thi : $THU_MUC_XUAT/tuan-telegram-disk.exe"
echo "   Kích thước   : $(du -h "$THU_MUC_XUAT/tuan-telegram-disk.exe" | cut -f1)"
echo "   Gói nén      : $GOI"
echo
mau '0;90' "Thiết kế bởi Tuandethuong."
