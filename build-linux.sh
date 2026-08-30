#!/usr/bin/env bash
# =============================================================================
#  Tuấn's Telegram Disk — dựng bản chạy độc lập cho Debian / Ubuntu (amd64)
#  Thiết kế bởi Tuandethuong.
# =============================================================================
set -euo pipefail

GOC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THU_MUC_BUILD="${GOC}/build"
THU_MUC_XUAT="${GOC}/dist/linux-amd64"
KIEU="${TTD_BUILD_TYPE:-Release}"
SO_LUONG="${TTD_JOBS:-$(nproc 2>/dev/null || echo 4)}"
# TTD_FULLY_STATIC=1 để liên kết tĩnh hoàn toàn (kể cả glibc).
HOAN_TOAN_TINH="${TTD_FULLY_STATIC:-0}"

mau()   { printf '\033[%sm%s\033[0m\n' "$1" "$2"; }
buoc()  { mau '1;36' "▸ $*"; }
xong()  { mau '1;32' "✓ $*"; }
loi()   { mau '1;31' "✗ $*" >&2; }

buoc "Kiểm tra công cụ cần thiết"
for cong_cu in cmake g++ make; do
  if ! command -v "$cong_cu" >/dev/null 2>&1; then
    loi "Thiếu '$cong_cu'. Cài bằng: sudo apt install build-essential cmake"
    exit 1
  fi
done
xong "$(cmake --version | head -1)  |  $(g++ --version | head -1)"

buoc "Cấu hình CMake (kiểu $KIEU)"
cmake -S "$GOC" -B "$THU_MUC_BUILD" \
      -DCMAKE_BUILD_TYPE="$KIEU" \
      -DTTD_STATIC=ON \
      -DTTD_FULLY_STATIC="$( [ "$HOAN_TOAN_TINH" = "1" ] && echo ON || echo OFF )" \
      -DTTD_BUILD_TESTS=ON

buoc "Biên dịch với $SO_LUONG luồng"
cmake --build "$THU_MUC_BUILD" -j "$SO_LUONG"

buoc "Chạy bộ tự kiểm tra"
if "$THU_MUC_BUILD/ttd_selftest"; then
  xong "Tự kiểm tra đạt"
else
  loi "Tự kiểm tra thất bại — dừng lại"
  exit 1
fi

buoc "Đóng gói vào $THU_MUC_XUAT"
rm -rf "$THU_MUC_XUAT"
mkdir -p "$THU_MUC_XUAT/schema"
cp "$THU_MUC_BUILD/tuan-telegram-disk" "$THU_MUC_XUAT/"
cp "$GOC/schema/"*.tl "$THU_MUC_XUAT/schema/"
cp "$GOC/README.md" "$THU_MUC_XUAT/" 2>/dev/null || true
cp "$GOC/LICENSE" "$THU_MUC_XUAT/" 2>/dev/null || true
strip --strip-unneeded "$THU_MUC_XUAT/tuan-telegram-disk" 2>/dev/null || true

# Tệp dịch vụ systemd mẫu.
cat > "$THU_MUC_XUAT/tuan-telegram-disk.service" <<'DICHVU'
# Chép vào /etc/systemd/system/ rồi chạy:
#   sudo systemctl daemon-reload
#   sudo systemctl enable --now tuan-telegram-disk
[Unit]
Description=Tuấn's Telegram Disk
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=ttd
Group=ttd
WorkingDirectory=/opt/tuan-telegram-disk
ExecStart=/opt/tuan-telegram-disk/tuan-telegram-disk
Restart=on-failure
RestartSec=5
LimitNOFILE=65535
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
DICHVU

cat > "$THU_MUC_XUAT/chay.sh" <<'CHAY'
#!/usr/bin/env bash
cd "$(dirname "$0")"
exec ./tuan-telegram-disk "$@"
CHAY
chmod +x "$THU_MUC_XUAT/chay.sh"

PHIEN_BAN="$(cat "$GOC/VERSION")"
SO_BUILD="$(cat "$GOC/BUILD_NUMBER" 2>/dev/null || echo 0)"
GOI="$GOC/dist/tuan-telegram-disk-${PHIEN_BAN}-linux-amd64.tar.gz"
tar -czf "$GOI" -C "$GOC/dist" linux-amd64

echo
xong "Xong! Phiên bản $PHIEN_BAN (build $SO_BUILD)"
echo "   Tệp thực thi : $THU_MUC_XUAT/tuan-telegram-disk"
echo "   Kích thước   : $(du -h "$THU_MUC_XUAT/tuan-telegram-disk" | cut -f1)"
echo "   Gói nén      : $GOI"
echo
echo "   Chạy thử     : cd $THU_MUC_XUAT && ./chay.sh"
echo "   Kiểm schema  : ./tuan-telegram-disk --check-schema"
echo
mau '0;90' "Thiết kế bởi Tuandethuong."
