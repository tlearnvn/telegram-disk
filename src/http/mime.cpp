#include "http/mime.h"

#include <map>

#include "common/strutil.h"

namespace ttd {
namespace http {

namespace {

const std::map<std::string, std::string>& mimeTable() {
    static const std::map<std::string, std::string> table = {
        // Văn bản & mã nguồn
        {"txt", "text/plain; charset=utf-8"},
        {"md", "text/markdown; charset=utf-8"},
        {"csv", "text/csv; charset=utf-8"},
        {"log", "text/plain; charset=utf-8"},
        {"html", "text/html; charset=utf-8"},
        {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"js", "application/javascript; charset=utf-8"},
        {"mjs", "application/javascript; charset=utf-8"},
        {"json", "application/json; charset=utf-8"},
        {"xml", "application/xml; charset=utf-8"},
        {"yaml", "text/yaml; charset=utf-8"},
        {"yml", "text/yaml; charset=utf-8"},
        {"ini", "text/plain; charset=utf-8"},
        {"conf", "text/plain; charset=utf-8"},
        {"sh", "text/x-shellscript; charset=utf-8"},
        {"py", "text/x-python; charset=utf-8"},
        {"c", "text/x-c; charset=utf-8"},
        {"h", "text/x-c; charset=utf-8"},
        {"cpp", "text/x-c++; charset=utf-8"},
        {"hpp", "text/x-c++; charset=utf-8"},
        {"java", "text/x-java; charset=utf-8"},
        {"go", "text/x-go; charset=utf-8"},
        {"rs", "text/x-rust; charset=utf-8"},
        {"php", "text/x-php; charset=utf-8"},
        {"sql", "text/plain; charset=utf-8"},
        // Ảnh
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"webp", "image/webp"},
        {"bmp", "image/bmp"},
        {"svg", "image/svg+xml"},
        {"ico", "image/x-icon"},
        {"tif", "image/tiff"},
        {"tiff", "image/tiff"},
        {"heic", "image/heic"},
        {"avif", "image/avif"},
        // Âm thanh
        {"mp3", "audio/mpeg"},
        {"wav", "audio/wav"},
        {"flac", "audio/flac"},
        {"aac", "audio/aac"},
        {"ogg", "audio/ogg"},
        {"opus", "audio/opus"},
        {"m4a", "audio/mp4"},
        {"wma", "audio/x-ms-wma"},
        // Video
        {"mp4", "video/mp4"},
        {"m4v", "video/mp4"},
        {"mkv", "video/x-matroska"},
        {"webm", "video/webm"},
        {"avi", "video/x-msvideo"},
        {"mov", "video/quicktime"},
        {"wmv", "video/x-ms-wmv"},
        {"flv", "video/x-flv"},
        {"ts", "video/mp2t"},
        {"m3u8", "application/vnd.apple.mpegurl"},
        // Tài liệu
        {"pdf", "application/pdf"},
        {"doc", "application/msword"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xls", "application/vnd.ms-excel"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"ppt", "application/vnd.ms-powerpoint"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"odt", "application/vnd.oasis.opendocument.text"},
        {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
        {"odp", "application/vnd.oasis.opendocument.presentation"},
        {"epub", "application/epub+zip"},
        {"rtf", "application/rtf"},
        // Nén
        {"zip", "application/zip"},
        {"rar", "application/vnd.rar"},
        {"7z", "application/x-7z-compressed"},
        {"tar", "application/x-tar"},
        {"gz", "application/gzip"},
        {"bz2", "application/x-bzip2"},
        {"xz", "application/x-xz"},
        {"zst", "application/zstd"},
        // Khác
        {"iso", "application/x-iso9660-image"},
        {"exe", "application/vnd.microsoft.portable-executable"},
        {"msi", "application/x-msi"},
        {"apk", "application/vnd.android.package-archive"},
        {"deb", "application/vnd.debian.binary-package"},
        {"rpm", "application/x-rpm"},
        {"dmg", "application/x-apple-diskimage"},
        {"ttf", "font/ttf"},
        {"otf", "font/otf"},
        {"woff", "font/woff"},
        {"woff2", "font/woff2"},
        {"torrent", "application/x-bittorrent"},
    };
    return table;
}

}  // namespace

std::string guessMimeType(const std::string& fileName) {
    std::string ext = fileExtension(fileName);
    if (ext.empty()) return "application/octet-stream";
    const auto& table = mimeTable();
    auto it = table.find(ext);
    if (it != table.end()) return it->second;
    return "application/octet-stream";
}

std::string fileCategory(const std::string& fileName, const std::string& mimeType) {
    std::string mime = mimeType.empty() ? guessMimeType(fileName) : toLower(mimeType);
    if (startsWith(mime, "image/")) return "image";
    if (startsWith(mime, "video/")) return "video";
    if (startsWith(mime, "audio/")) return "audio";
    if (mime == "application/pdf") return "pdf";
    if (startsWith(mime, "font/")) return "font";

    std::string ext = fileExtension(fileName);
    if (ext == "zip" || ext == "rar" || ext == "7z" || ext == "tar" || ext == "gz" ||
        ext == "bz2" || ext == "xz" || ext == "zst" || ext == "iso")
        return "archive";
    if (ext == "doc" || ext == "docx" || ext == "odt" || ext == "rtf" || ext == "epub")
        return "document";
    if (ext == "xls" || ext == "xlsx" || ext == "ods" || ext == "csv") return "spreadsheet";
    if (ext == "ppt" || ext == "pptx" || ext == "odp") return "presentation";
    if (ext == "c" || ext == "h" || ext == "cpp" || ext == "hpp" || ext == "java" ||
        ext == "go" || ext == "rs" || ext == "py" || ext == "js" || ext == "ts" ||
        ext == "php" || ext == "sh" || ext == "sql" || ext == "json" || ext == "xml" ||
        ext == "yaml" || ext == "yml" || ext == "html" || ext == "css")
        return "code";
    if (startsWith(mime, "text/")) return "text";
    return "other";
}

bool isStreamable(const std::string& mimeType) {
    std::string mime = toLower(mimeType);
    return startsWith(mime, "video/") || startsWith(mime, "audio/") ||
           startsWith(mime, "image/") || mime == "application/pdf";
}

bool isSafeInline(const std::string& mimeType) {
    std::string mime = toLower(mimeType);
    // Không bao giờ hiển thị HTML/SVG/JS của người dùng ngay trong trang —
    // tránh chạy mã độc trên cùng tên miền.
    if (startsWith(mime, "text/html")) return false;
    if (mime.find("svg") != std::string::npos) return false;
    if (mime.find("javascript") != std::string::npos) return false;
    if (mime.find("xhtml") != std::string::npos) return false;
    return isStreamable(mime) || startsWith(mime, "text/plain");
}

}  // namespace http
}  // namespace ttd
