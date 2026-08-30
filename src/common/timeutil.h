// Tiện ích thời gian. Toàn hệ thống hiển thị theo múi giờ Việt Nam (UTC+7).
#pragma once

#include <cstdint>
#include <string>

namespace ttd {

// Lệch múi giờ hệ thống, tính bằng giây. Mặc định +7 giờ (Việt Nam).
constexpr int64_t kSystemTimezoneOffsetSeconds = 7 * 3600;
constexpr const char* kSystemTimezoneName = "UTC+7";

// Số giây kể từ epoch (UTC).
int64_t nowUnix();
// Số mili-giây kể từ epoch (UTC).
int64_t nowUnixMillis();
// Đồng hồ đơn điệu tính bằng mili-giây (dùng để đo khoảng thời gian).
int64_t monotonicMillis();

// Định dạng theo giờ Việt Nam.
std::string formatDateTime(int64_t unixSeconds);        // 2026-08-30 14:05:09
std::string formatDateTimeMillis(int64_t unixMillis);   // 2026-08-30 14:05:09.123
std::string formatDate(int64_t unixSeconds);            // 30/08/2026
std::string formatTimeOnly(int64_t unixSeconds);        // 14:05:09
// Chuỗi ISO-8601 kèm lệch múi giờ: 2026-08-30T14:05:09+07:00
std::string formatIso8601(int64_t unixSeconds);
// Định dạng HTTP (RFC 7231), luôn theo GMT.
std::string formatHttpDate(int64_t unixSeconds);
// Định dạng ISO cho WebDAV creationdate (RFC 3339, UTC).
std::string formatIso8601Utc(int64_t unixSeconds);
// Phân tích ngày giờ HTTP (RFC 7231 / RFC 850 / asctime) -> epoch, -1 nếu lỗi.
int64_t parseHttpDate(const std::string& s);

// Khoảng thời gian dễ đọc: "2 phút 13 giây", "1,5 giờ".
std::string formatDuration(int64_t seconds);
// Tốc độ dễ đọc: "12,4 MB/s"
std::string formatSpeed(double bytesPerSecond);

}  // namespace ttd
