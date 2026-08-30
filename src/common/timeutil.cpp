#include "common/timeutil.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "common/strutil.h"

namespace ttd {

namespace {

struct CivilTime {
    int year, month, day, hour, minute, second, weekday, yearday;
};

// Chuyển epoch (đã cộng lệch múi giờ) sang lịch dương — thuật toán Howard Hinnant,
// không phụ thuộc localtime/gmtime nên hoạt động giống nhau trên mọi hệ điều hành.
CivilTime civilFromEpoch(int64_t t) {
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    CivilTime c{};
    c.hour = static_cast<int>(rem / 3600);
    c.minute = static_cast<int>((rem % 3600) / 60);
    c.second = static_cast<int>(rem % 60);
    // 1970-01-01 là thứ Năm (4).
    c.weekday = static_cast<int>(((days % 7) + 11) % 7);

    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;                                  // [0, 146096]
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           // [0, 365]
    int64_t mp = (5 * doy + 2) / 153;                                // [0, 11]
    int64_t d = doy - (153 * mp + 2) / 5 + 1;                        // [1, 31]
    int64_t m = mp < 10 ? mp + 3 : mp - 9;                           // [1, 12]
    c.year = static_cast<int>(y + (m <= 2 ? 1 : 0));
    c.month = static_cast<int>(m);
    c.day = static_cast<int>(d);
    c.yearday = static_cast<int>(doy);
    return c;
}

int64_t epochFromCivil(int y, int m, int d, int hh, int mm, int ss) {
    y -= m <= 2 ? 1 : 0;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

CivilTime localCivil(int64_t unixSeconds) {
    return civilFromEpoch(unixSeconds + kSystemTimezoneOffsetSeconds);
}

}  // namespace

int64_t nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t nowUnixMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t monotonicMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string formatDateTime(int64_t unixSeconds) {
    CivilTime c = localCivil(unixSeconds);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", c.year, c.month, c.day,
                  c.hour, c.minute, c.second);
    return buf;
}

std::string formatDateTimeMillis(int64_t unixMillis) {
    int64_t sec = unixMillis / 1000;
    int ms = static_cast<int>(unixMillis % 1000);
    if (ms < 0) {
        ms += 1000;
        --sec;
    }
    CivilTime c = localCivil(sec);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", c.year, c.month, c.day,
                  c.hour, c.minute, c.second, ms);
    return buf;
}

std::string formatDate(int64_t unixSeconds) {
    CivilTime c = localCivil(unixSeconds);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02d/%02d/%04d", c.day, c.month, c.year);
    return buf;
}

std::string formatTimeOnly(int64_t unixSeconds) {
    CivilTime c = localCivil(unixSeconds);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", c.hour, c.minute, c.second);
    return buf;
}

std::string formatIso8601(int64_t unixSeconds) {
    CivilTime c = localCivil(unixSeconds);
    int64_t off = kSystemTimezoneOffsetSeconds;
    char sign = off < 0 ? '-' : '+';
    if (off < 0) off = -off;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", c.year, c.month,
                  c.day, c.hour, c.minute, c.second, sign, static_cast<int>(off / 3600),
                  static_cast<int>((off % 3600) / 60));
    return buf;
}

std::string formatIso8601Utc(int64_t unixSeconds) {
    CivilTime c = civilFromEpoch(unixSeconds);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", c.year, c.month, c.day,
                  c.hour, c.minute, c.second);
    return buf;
}

std::string formatHttpDate(int64_t unixSeconds) {
    static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    CivilTime c = civilFromEpoch(unixSeconds);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT", kDays[c.weekday], c.day,
                  kMonths[c.month - 1], c.year, c.hour, c.minute, c.second);
    return buf;
}

int64_t parseHttpDate(const std::string& s) {
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    char mon[4] = {0};
    // Wed, 21 Oct 2015 07:28:00 GMT
    if (std::sscanf(s.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) == 6 ||
        // Sunday, 06-Nov-94 08:49:37 GMT  /  Wed Oct 21 07:28:00 2015
        std::sscanf(s.c_str(), "%*[^,], %d-%3s-%d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) == 6) {
        if (year < 100) year += (year < 70 ? 2000 : 1900);
    } else if (std::sscanf(s.c_str(), "%*3s %3s %d %d:%d:%d %d", mon, &day, &hh, &mm, &ss, &year) ==
               6) {
        // asctime
    } else {
        return -1;
    }
    int month = 0;
    for (int i = 0; i < 12; ++i)
        if (std::strncmp(mon, kMonths[i], 3) == 0) month = i + 1;
    if (month == 0 || day <= 0 || day > 31) return -1;
    return epochFromCivil(year, month, day, hh, mm, ss);
}

std::string formatDuration(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds < 60) return std::to_string(seconds) + " giây";
    if (seconds < 3600) {
        int64_t m = seconds / 60, s = seconds % 60;
        return s ? std::to_string(m) + " phút " + std::to_string(s) + " giây"
                 : std::to_string(m) + " phút";
    }
    if (seconds < 86400) {
        int64_t h = seconds / 3600, m = (seconds % 3600) / 60;
        return m ? std::to_string(h) + " giờ " + std::to_string(m) + " phút"
                 : std::to_string(h) + " giờ";
    }
    int64_t d = seconds / 86400, h = (seconds % 86400) / 3600;
    return h ? std::to_string(d) + " ngày " + std::to_string(h) + " giờ"
             : std::to_string(d) + " ngày";
}

std::string formatSpeed(double bytesPerSecond) {
    if (bytesPerSecond < 0) bytesPerSecond = 0;
    return formatBytes(static_cast<uint64_t>(bytesPerSecond)) + "/s";
}

}  // namespace ttd
