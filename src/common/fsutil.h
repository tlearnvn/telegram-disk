// Tiện ích hệ thống tệp, an toàn với đường dẫn UTF-8 trên cả Linux và Windows.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "common/strutil.h"

namespace ttd {

bool pathExists(const std::string& path);
bool isDirectory(const std::string& path);
uint64_t fileSizeOf(const std::string& path);
int64_t fileModifiedTime(const std::string& path);
bool ensureDirectoryExists(const std::string& path);   // tạo đệ quy
bool removeFileIfExists(const std::string& path);
bool renamePath(const std::string& from, const std::string& to);
bool removeDirectoryRecursive(const std::string& path);
std::vector<std::string> listDirectory(const std::string& path);

std::string parentDirectoryOf(const std::string& path);
std::string joinPath(const std::string& a, const std::string& b);
// Đường dẫn tuyệt đối (giải quyết ".", ".." theo thư mục hiện hành).
std::string absolutePath(const std::string& path);
std::string currentWorkingDirectory();
// Thư mục chứa tệp thực thi đang chạy.
std::string executableDirectory();

// Mở tệp với đường dẫn UTF-8 (trên Windows sẽ chuyển sang UTF-16).
FILE* fsutilOpen(const std::string& path, const char* mode);
FILE* fsutilOpenAppend(const std::string& path);

bool readWholeFile(const std::string& path, std::string& out);
bool readWholeFileBytes(const std::string& path, Bytes& out);
bool writeWholeFile(const std::string& path, const std::string& data);
// Ghi an toàn: ghi ra tệp tạm rồi đổi tên đè lên, tránh mất dữ liệu khi tắt đột ngột.
bool writeWholeFileAtomic(const std::string& path, const std::string& data);

// Dung lượng còn trống của ổ đĩa chứa `path` (0 nếu không xác định được).
uint64_t freeDiskSpace(const std::string& path);
// Tổng bộ nhớ RAM vật lý của máy (0 nếu không xác định được).
uint64_t totalSystemMemory();
// RAM khả dụng hiện tại (0 nếu không xác định được).
uint64_t availableSystemMemory();

}  // namespace ttd
