#include "common/fsutil.h"

#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ttd {

#if defined(_WIN32)
namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr,
                        nullptr);
    return s;
}
}  // namespace
#endif

bool pathExists(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
#endif
}

bool isDirectory(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    DWORD a = GetFileAttributesW(widen(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
#endif
}

uint64_t fileSizeOf(const std::string& path) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.st_size);
#endif
}

int64_t fileModifiedTime(const std::string& path) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &d)) return 0;
    ULARGE_INTEGER u;
    u.LowPart = d.ftLastWriteTime.dwLowDateTime;
    u.HighPart = d.ftLastWriteTime.dwHighDateTime;
    // FILETIME tính từ 1601-01-01, đơn vị 100ns.
    return static_cast<int64_t>(u.QuadPart / 10000000ULL) - 11644473600LL;
#else
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return 0;
    return static_cast<int64_t>(st.st_mtime);
#endif
}

bool ensureDirectoryExists(const std::string& path) {
    if (path.empty()) return true;
    if (isDirectory(path)) return true;

    std::string norm = path;
    for (auto& c : norm)
        if (c == '\\') c = '/';
    // Bỏ dấu '/' cuối.
    while (norm.size() > 1 && norm.back() == '/') norm.pop_back();

    size_t start = 0;
#if defined(_WIN32)
    if (norm.size() >= 2 && norm[1] == ':') start = 2;
#endif
    if (!norm.empty() && norm[0] == '/') start = 1;

    for (size_t i = start; i <= norm.size(); ++i) {
        if (i == norm.size() || norm[i] == '/') {
            std::string sub = norm.substr(0, i);
            if (sub.empty()) continue;
            if (isDirectory(sub)) continue;
#if defined(_WIN32)
            if (!CreateDirectoryW(widen(sub).c_str(), nullptr)) {
                if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
            }
#else
            if (::mkdir(sub.c_str(), 0755) != 0 && errno != EEXIST) return false;
#endif
        }
    }
    return isDirectory(norm);
}

bool removeFileIfExists(const std::string& path) {
    if (path.empty() || !pathExists(path)) return true;
#if defined(_WIN32)
    return DeleteFileW(widen(path).c_str()) != 0;
#else
    return ::remove(path.c_str()) == 0;
#endif
}

bool renamePath(const std::string& from, const std::string& to) {
#if defined(_WIN32)
    return MoveFileExW(widen(from).c_str(), widen(to).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

std::vector<std::string> listDirectory(const std::string& path) {
    std::vector<std::string> out;
#if defined(_WIN32)
    std::wstring pattern = widen(path) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        out.push_back(narrow(name));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = ::opendir(path.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        out.push_back(name);
    }
    ::closedir(d);
#endif
    return out;
}

bool removeDirectoryRecursive(const std::string& path) {
    if (!pathExists(path)) return true;
    if (!isDirectory(path)) return removeFileIfExists(path);
    for (const auto& name : listDirectory(path)) {
        std::string child = joinPath(path, name);
        if (isDirectory(child)) {
            removeDirectoryRecursive(child);
        } else {
            removeFileIfExists(child);
        }
    }
#if defined(_WIN32)
    return RemoveDirectoryW(widen(path).c_str()) != 0;
#else
    return ::rmdir(path.c_str()) == 0;
#endif
}

std::string parentDirectoryOf(const std::string& path) {
    std::string p = path;
    for (auto& c : p)
        if (c == '\\') c = '/';
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    size_t pos = p.rfind('/');
    if (pos == std::string::npos) return "";
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
#if defined(_WIN32)
    if (b.size() >= 2 && b[1] == ':') return b;
#endif
    if (b[0] == '/' || b[0] == '\\') return b;
    char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

std::string currentWorkingDirectory() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), buf);
    if (n == 0) return ".";
    return narrow(std::wstring(buf, n));
#else
    char buf[4096];
    if (!::getcwd(buf, sizeof(buf))) return ".";
    return buf;
#endif
}

std::string absolutePath(const std::string& path) {
    if (path.empty()) return currentWorkingDirectory();
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(widen(path).c_str(),
                               static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
    if (n == 0) return path;
    return narrow(std::wstring(buf, n));
#else
    if (path[0] == '/') return path;
    return joinPath(currentWorkingDirectory(), path);
#endif
}

std::string executableDirectory() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return currentWorkingDirectory();
    return parentDirectoryOf(narrow(std::wstring(buf, n)));
#else
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return currentWorkingDirectory();
    buf[n] = '\0';
    return parentDirectoryOf(buf);
#endif
}

FILE* fsutilOpen(const std::string& path, const char* mode) {
#if defined(_WIN32)
    std::wstring wm;
    for (const char* p = mode; *p; ++p) wm.push_back(static_cast<wchar_t>(*p));
    return _wfopen(widen(path).c_str(), wm.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

FILE* fsutilOpenAppend(const std::string& path) { return fsutilOpen(path, "ab"); }

bool readWholeFile(const std::string& path, std::string& out) {
    FILE* f = fsutilOpen(path, "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

bool readWholeFileBytes(const std::string& path, Bytes& out) {
    std::string s;
    if (!readWholeFile(path, s)) return false;
    out.assign(s.begin(), s.end());
    return true;
}

bool writeWholeFile(const std::string& path, const std::string& data) {
    ensureDirectoryExists(parentDirectoryOf(path));
    FILE* f = fsutilOpen(path, "wb");
    if (!f) return false;
    bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    return ok;
}

bool writeWholeFileAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    if (!writeWholeFile(tmp, data)) return false;
    if (!renamePath(tmp, path)) {
        removeFileIfExists(tmp);
        return false;
    }
    return true;
}

uint64_t freeDiskSpace(const std::string& path) {
#if defined(_WIN32)
    ULARGE_INTEGER freeBytes;
    std::string dir = isDirectory(path) ? path : parentDirectoryOf(path);
    if (dir.empty()) dir = ".";
    if (GetDiskFreeSpaceExW(widen(dir).c_str(), &freeBytes, nullptr, nullptr))
        return static_cast<uint64_t>(freeBytes.QuadPart);
    return 0;
#else
    std::string dir = isDirectory(path) ? path : parentDirectoryOf(path);
    if (dir.empty()) dir = ".";
    struct statvfs st;
    if (::statvfs(dir.c_str(), &st) != 0) return 0;
    return static_cast<uint64_t>(st.f_bavail) * static_cast<uint64_t>(st.f_frsize);
#endif
}

uint64_t totalSystemMemory() {
#if defined(_WIN32)
    MEMORYSTATUSEX st;
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st)) return static_cast<uint64_t>(st.ullTotalPhys);
    return 0;
#else
    long pages = ::sysconf(_SC_PHYS_PAGES);
    long pageSize = ::sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || pageSize <= 0) return 0;
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize);
#endif
}

uint64_t availableSystemMemory() {
#if defined(_WIN32)
    MEMORYSTATUSEX st;
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st)) return static_cast<uint64_t>(st.ullAvailPhys);
    return 0;
#else
    // Ưu tiên MemAvailable trong /proc/meminfo vì phản ánh đúng RAM có thể dùng ngay.
    std::string meminfo;
    if (readWholeFile("/proc/meminfo", meminfo)) {
        size_t pos = meminfo.find("MemAvailable:");
        if (pos != std::string::npos) {
            uint64_t kb = 0;
            if (std::sscanf(meminfo.c_str() + pos + 13, "%llu",
                            reinterpret_cast<unsigned long long*>(&kb)) == 1)
                return kb * 1024;
        }
    }
    long pages = ::sysconf(_SC_AVPHYS_PAGES);
    long pageSize = ::sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || pageSize <= 0) return 0;
    return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize);
#endif
}

}  // namespace ttd
