#include "http/webdav.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "app/app.h"
#include "common/logging.h"
#include "common/strutil.h"
#include "common/timeutil.h"
#include "crypto/random.h"
#include "http/mime.h"
#include "storage/download_stream.h"

namespace ttd {
namespace http {

namespace {
constexpr const char* kTag = "webdav";

// Khoá WebDAV giả lập — Windows và macOS đòi hỏi LOCK trước khi ghi.
struct DavLock {
    std::string token;
    std::string path;
    int userId = 0;
    int64_t expiresAt = 0;
};

std::mutex& lockMutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, DavLock>& lockTable() {
    static std::map<std::string, DavLock> t;
    return t;
}

std::string createLock(const std::string& path, int userId, int timeoutSeconds) {
    std::lock_guard<std::mutex> lk(lockMutex());
    // Dọn khoá hết hạn.
    int64_t now = nowUnix();
    for (auto it = lockTable().begin(); it != lockTable().end();) {
        if (it->second.expiresAt < now) it = lockTable().erase(it);
        else ++it;
    }
    DavLock lock;
    lock.token = "opaquelocktoken:" + crypto::randomHex(16);
    lock.path = path;
    lock.userId = userId;
    lock.expiresAt = now + timeoutSeconds;
    lockTable()[lock.token] = lock;
    return lock.token;
}

void releaseLock(const std::string& token) {
    std::lock_guard<std::mutex> lk(lockMutex());
    lockTable().erase(token);
}

std::string davPrefix() {
    Config& cfg = Config::instance();
    std::string prefix = cfg.server.webdavPrefix;
    if (prefix.empty()) prefix = "/webdav";
    if (prefix.back() == '/' && prefix.size() > 1) prefix.pop_back();
    return prefix;
}

// Chuyển đường dẫn HTTP thành đường dẫn ảo trong ổ đĩa.
std::string toVirtualPath(const std::string& httpPath) {
    std::string prefix = davPrefix();
    std::string rest = httpPath;
    if (startsWith(rest, prefix)) rest = rest.substr(prefix.size());
    if (rest.empty()) rest = "/";
    return normalizeVirtualPath(rest);
}

std::string toHref(const std::string& virtualPath) {
    std::string prefix = davPrefix();
    if (virtualPath == "/") return prefix + "/";
    std::string encoded;
    for (const auto& part : split(virtualPath, '/', false)) encoded += "/" + urlEncode(part);
    return prefix + encoded;
}

// Lấy đường dẫn ảo từ tiêu đề Destination của MOVE/COPY.
std::string destinationPath(const Request& req) {
    std::string dest = req.header("Destination");
    if (dest.empty()) return "";
    // Bỏ phần scheme://host nếu có.
    size_t schemeEnd = dest.find("://");
    if (schemeEnd != std::string::npos) {
        size_t slash = dest.find('/', schemeEnd + 3);
        dest = slash == std::string::npos ? "/" : dest.substr(slash);
    }
    return toVirtualPath(urlDecode(dest));
}

std::string xmlPropfindResponse(const std::vector<db::FileEntry>& entries, bool includeSelf,
                                const db::FileEntry& self) {
    std::string xml;
    xml.reserve(2048);
    xml += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    xml += "<D:multistatus xmlns:D=\"DAV:\">\n";

    auto appendEntry = [&](const db::FileEntry& e) {
        std::string href = toHref(e.path);
        if (e.isFolder && (href.empty() || href.back() != '/')) href += "/";
        xml += "  <D:response>\n";
        xml += "    <D:href>" + xmlEscape(href) + "</D:href>\n";
        xml += "    <D:propstat>\n      <D:prop>\n";
        xml += "        <D:displayname>" +
               xmlEscape(e.name.empty() ? std::string("Tuan Telegram Disk") : e.name) +
               "</D:displayname>\n";
        if (e.isFolder) {
            xml += "        <D:resourcetype><D:collection/></D:resourcetype>\n";
            xml += "        <D:getcontenttype>httpd/unix-directory</D:getcontenttype>\n";
        } else {
            xml += "        <D:resourcetype/>\n";
            xml += "        <D:getcontentlength>" + std::to_string(e.size) +
                   "</D:getcontentlength>\n";
            xml += "        <D:getcontenttype>" +
                   xmlEscape(e.mimeType.empty() ? guessMimeType(e.name) : e.mimeType) +
                   "</D:getcontenttype>\n";
            if (!e.sha256.empty())
                xml += "        <D:getetag>\"" + xmlEscape(e.sha256.substr(0, 32)) +
                       "\"</D:getetag>\n";
        }
        xml += "        <D:getlastmodified>" + formatHttpDate(e.modifiedAt) +
               "</D:getlastmodified>\n";
        xml += "        <D:creationdate>" + formatIso8601Utc(e.createdAt) + "</D:creationdate>\n";
        xml += "        <D:supportedlock>\n";
        xml += "          <D:lockentry><D:lockscope><D:exclusive/></D:lockscope>"
               "<D:locktype><D:write/></D:locktype></D:lockentry>\n";
        xml += "        </D:supportedlock>\n";
        xml += "      </D:prop>\n      <D:status>HTTP/1.1 200 OK</D:status>\n";
        xml += "    </D:propstat>\n  </D:response>\n";
    };

    if (includeSelf) appendEntry(self);
    for (const auto& e : entries) appendEntry(e);
    xml += "</D:multistatus>\n";
    return xml;
}

struct DavAuth {
    bool ok = false;
    db::UserEntry user;
};

DavAuth authenticateDav(app::App& app, const Request& req, Response& res) {
    DavAuth out;
    std::string authHeader = req.header("Authorization");
    if (!authHeader.empty() && app.users()->authenticateBasic(authHeader, out.user)) {
        out.ok = true;
        return out;
    }
    // Cho phép dùng cookie phiên web để mở nhanh trong trình duyệt.
    std::string token = req.cookie("ttd_session");
    if (!token.empty() && app.users()->authenticate(token, out.user)) {
        out.ok = true;
        return out;
    }
    res.status = 401;
    res.setHeader("WWW-Authenticate", "Basic realm=\"Tuan's Telegram Disk\", charset=\"UTF-8\"");
    res.setHeader("Content-Type", "text/plain; charset=utf-8");
    res.body = "Cần đăng nhập để truy cập WebDAV.";
    return out;
}

}  // namespace

void registerWebdavRoutes(HttpServer& server, app::App& app) {
    std::string prefix = davPrefix();

    auto handler = [&app](Request& req, BodyReader& body, Response& res) {
        const std::string method = req.method;

        // OPTIONS không cần xác thực để client dò khả năng máy chủ.
        if (method == "OPTIONS") {
            res.status = 200;
            res.setHeader("DAV", "1, 2");
            res.setHeader("Allow",
                          "OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, COPY, "
                          "MOVE, LOCK, UNLOCK");
            res.setHeader("MS-Author-Via", "DAV");
            res.setHeader("Accept-Ranges", "bytes");
            res.setHeader("Content-Length", "0");
            return;
        }

        DavAuth auth = authenticateDav(app, req, res);
        if (!auth.ok) return;

        std::string vpath = toVirtualPath(req.path);
        db::Database* database = app.database();
        std::string error;

        // ---- PROPFIND: liệt kê thư mục ------------------------------------
        if (method == "PROPFIND") {
            body.readAll(req.body, 256 * 1024);
            std::string depth = req.header("Depth", "1");

            db::FileEntry self;
            if (!app.vfs()->resolve(vpath, self, error)) {
                res.setText("Không tìm thấy: " + vpath, 404);
                return;
            }
            if (vpath == "/") {
                self.name = "Tuan Telegram Disk";
                self.path = "/";
                self.isFolder = true;
                self.modifiedAt = nowUnix();
                self.createdAt = app.startedAt();
            }

            std::vector<db::FileEntry> children;
            if (self.isFolder && depth != "0") {
                db::ListOptions opts;
                opts.parentId = self.id;
                opts.limit = 5000;
                if (!auth.user.isAdmin) opts.ownerId = auth.user.id;
                database->listEntries(opts, children, error);
            }
            std::string xml = xmlPropfindResponse(children, true, self);
            res.status = 207;
            res.setHeader("Content-Type", "application/xml; charset=utf-8");
            res.body = std::move(xml);
            return;
        }

        // ---- GET / HEAD: đọc tệp ------------------------------------------
        if (method == "GET" || method == "HEAD") {
            db::FileEntry entry;
            if (!app.vfs()->resolve(vpath, entry, error)) {
                res.setText("Không tìm thấy: " + vpath, 404);
                return;
            }
            if (entry.isFolder) {
                // Trả một trang HTML đơn giản để duyệt bằng trình duyệt.
                db::ListOptions opts;
                opts.parentId = entry.id;
                opts.limit = 2000;
                std::vector<db::FileEntry> children;
                database->listEntries(opts, children, error);
                std::string html =
                    "<!doctype html><meta charset=\"utf-8\"><title>" + htmlEscape(vpath) +
                    "</title><h2>" + htmlEscape(vpath) + "</h2><ul>";
                if (vpath != "/")
                    html += "<li><a href=\"" + toHref(parentPath(vpath)) + "/\">..</a></li>";
                for (const auto& c : children) {
                    html += "<li><a href=\"" + toHref(c.path) + (c.isFolder ? "/" : "") + "\">" +
                            htmlEscape(c.name) + "</a>";
                    if (!c.isFolder) html += " — " + formatBytes(c.size);
                    html += "</li>";
                }
                html += "</ul>";
                res.setHtml(html);
                return;
            }
            if (!auth.user.isAdmin && entry.ownerId != auth.user.id) {
                res.setText("Không có quyền truy cập.", 403);
                return;
            }

            res.setHeader("Accept-Ranges", "bytes");
            res.setHeader("Content-Type",
                          entry.mimeType.empty() ? guessMimeType(entry.name) : entry.mimeType);
            res.setHeader("Last-Modified", formatHttpDate(entry.modifiedAt));
            if (!entry.sha256.empty())
                res.setHeader("ETag", "\"" + entry.sha256.substr(0, 32) + "\"");

            uint64_t start = 0;
            uint64_t length = entry.size;
            std::string rangeHeader = req.header("Range");
            if (!rangeHeader.empty()) {
                storage::ByteRange range;
                bool unsatisfiable = false;
                if (storage::parseRangeHeader(rangeHeader, entry.size, range, unsatisfiable)) {
                    start = range.start;
                    length = range.length();
                    res.status = 206;
                    res.setHeader("Content-Range", storage::makeContentRange(range, entry.size));
                } else if (unsatisfiable) {
                    res.status = 416;
                    res.setHeader("Content-Range",
                                  storage::makeUnsatisfiedContentRange(entry.size));
                    return;
                }
            }
            if (entry.size == 0 || length == 0) {
                res.setHeader("Content-Length", "0");
                return;
            }
            res.streamLength = length;
            storage::StorageEngine* engine = app.engine();
            db::FileEntry copy = entry;
            res.streamBody = [engine, copy, start, length](const ResponseWriter& write) -> bool {
                std::string streamError;
                bool ok = engine->streamFileRange(
                    copy, start, length,
                    [&](const uint8_t* data, size_t len) -> bool {
                        return write(reinterpret_cast<const char*>(data), len);
                    },
                    streamError);
                if (!ok)
                    LOG_WARN(kTag, "Lỗi khi phát '%s': %s", copy.name.c_str(),
                             streamError.c_str());
                return ok;
            };
            return;
        }

        // ---- MKCOL: tạo thư mục -------------------------------------------
        if (method == "MKCOL") {
            db::FileEntry existing;
            if (app.vfs()->resolve(vpath, existing, error)) {
                res.setText("Đã tồn tại.", 405);
                return;
            }
            std::string parent = parentPath(vpath);
            std::string name = baseName(vpath);
            app::VfsResult r = app.vfs()->createFolder(parent, name, auth.user.id);
            if (!r.ok) {
                res.setText(r.error, 409);
                return;
            }
            res.status = 201;
            res.setHeader("Content-Length", "0");
            return;
        }

        // ---- PUT: ghi tệp --------------------------------------------------
        if (method == "PUT") {
            std::string parent = parentPath(vpath);
            std::string name = baseName(vpath);
            if (name.empty()) {
                res.setText("Tên tệp không hợp lệ.", 400);
                return;
            }

            storage::UploadInitRequest ur;
            ur.name = name;
            ur.targetFolderPath = parent;
            ur.totalSize = req.contentLength;
            ur.mimeType = req.header("Content-Type");
            ur.policy = storage::ConflictPolicy::Replace;
            ur.ownerId = auth.user.id;

            storage::UploadInitResult init = app.uploads()->begin(ur);
            if (!init.ok) {
                res.setText(init.error, 507);
                return;
            }
            auto session = app.uploads()->find(init.uploadId);
            if (!session) {
                res.setText("Không mở được phiên ghi.", 500);
                return;
            }

            uint8_t buffer[256 * 1024];
            std::string writeError;
            while (true) {
                long n = body.read(buffer, sizeof(buffer));
                if (n < 0) {
                    app.uploads()->cancel(init.uploadId, "Kết nối WebDAV bị ngắt");
                    res.setText("Kết nối bị ngắt giữa chừng.", 400);
                    res.closeConnection = true;
                    return;
                }
                if (n == 0) break;
                if (!session->receive(buffer, static_cast<size_t>(n), writeError)) {
                    app.uploads()->cancel(init.uploadId, writeError);
                    res.setText(writeError, 507);
                    return;
                }
            }

            db::FileEntry entry;
            if (!app.uploads()->complete(init.uploadId, entry, writeError)) {
                res.setText(writeError, 500);
                return;
            }
            res.status = 201;
            res.setHeader("Content-Length", "0");
            if (!entry.sha256.empty())
                res.setHeader("ETag", "\"" + entry.sha256.substr(0, 32) + "\"");
            LOG_INFO(kTag, "Đã nhận '%s' (%s) qua WebDAV", entry.path.c_str(),
                     formatBytes(entry.size).c_str());
            return;
        }

        // ---- DELETE --------------------------------------------------------
        if (method == "DELETE") {
            db::FileEntry entry;
            if (!app.vfs()->resolve(vpath, entry, error)) {
                res.setText("Không tìm thấy: " + vpath, 404);
                return;
            }
            if (entry.id == 0) {
                res.setText("Không thể xoá thư mục gốc.", 403);
                return;
            }
            // WebDAV mong đợi xoá thật; chuyển vào thùng rác để còn khôi phục được.
            app::VfsResult r = app.vfs()->trash(entry.id);
            if (!r.ok) {
                res.setText(r.error, 500);
                return;
            }
            res.status = 204;
            res.setHeader("Content-Length", "0");
            return;
        }

        // ---- MOVE / COPY ---------------------------------------------------
        if (method == "MOVE" || method == "COPY") {
            db::FileEntry entry;
            if (!app.vfs()->resolve(vpath, entry, error)) {
                res.setText("Không tìm thấy: " + vpath, 404);
                return;
            }
            std::string dest = destinationPath(req);
            if (dest.empty()) {
                res.setText("Thiếu tiêu đề Destination.", 400);
                return;
            }
            std::string destParent = parentPath(dest);
            std::string destName = baseName(dest);
            bool overwrite = toUpper(req.header("Overwrite", "T")) != "F";

            db::FileEntry existing;
            std::string existingError;
            if (app.vfs()->resolve(dest, existing, existingError) && existing.id != entry.id) {
                if (!overwrite) {
                    res.setText("Đích đã tồn tại.", 412);
                    return;
                }
                app.vfs()->trash(existing.id);
            }

            app::VfsResult r;
            if (method == "MOVE") {
                r = app.vfs()->move(entry.id, destParent);
                if (r.ok && r.entry.name != destName)
                    r = app.vfs()->rename(r.entry.id, destName);
            } else {
                r = app.vfs()->copy(entry.id, destParent, auth.user.id);
                if (r.ok && r.entry.name != destName)
                    r = app.vfs()->rename(r.entry.id, destName);
            }
            if (!r.ok) {
                res.setText(r.error, 409);
                return;
            }
            res.status = 201;
            res.setHeader("Content-Length", "0");
            return;
        }

        // ---- LOCK / UNLOCK -------------------------------------------------
        if (method == "LOCK") {
            body.readAll(req.body, 64 * 1024);
            int timeout = 600;
            std::string timeoutHeader = req.header("Timeout");
            if (startsWith(timeoutHeader, "Second-")) {
                int64_t v = 0;
                if (parseInt64(timeoutHeader.substr(7), v) && v > 0 && v < 86400)
                    timeout = static_cast<int>(v);
            }
            std::string token = createLock(vpath, auth.user.id, timeout);
            std::string xml =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<D:prop xmlns:D=\"DAV:\">\n  <D:lockdiscovery>\n    <D:activelock>\n"
                "      <D:locktype><D:write/></D:locktype>\n"
                "      <D:lockscope><D:exclusive/></D:lockscope>\n"
                "      <D:depth>infinity</D:depth>\n"
                "      <D:timeout>Second-" + std::to_string(timeout) + "</D:timeout>\n"
                "      <D:locktoken><D:href>" + xmlEscape(token) + "</D:href></D:locktoken>\n"
                "      <D:lockroot><D:href>" + xmlEscape(toHref(vpath)) + "</D:href></D:lockroot>\n"
                "    </D:activelock>\n  </D:lockdiscovery>\n</D:prop>\n";
            res.status = 200;
            res.setHeader("Content-Type", "application/xml; charset=utf-8");
            res.setHeader("Lock-Token", "<" + token + ">");
            res.body = std::move(xml);
            return;
        }
        if (method == "UNLOCK") {
            std::string token = req.header("Lock-Token");
            if (!token.empty() && token.front() == '<') token = token.substr(1);
            if (!token.empty() && token.back() == '>') token.pop_back();
            releaseLock(token);
            res.status = 204;
            res.setHeader("Content-Length", "0");
            return;
        }

        // ---- PROPPATCH: chấp nhận nhưng không lưu thuộc tính tuỳ ý ----------
        if (method == "PROPPATCH") {
            body.readAll(req.body, 256 * 1024);
            db::FileEntry entry;
            if (!app.vfs()->resolve(vpath, entry, error)) {
                res.setText("Không tìm thấy: " + vpath, 404);
                return;
            }
            std::string xml =
                "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                "<D:multistatus xmlns:D=\"DAV:\">\n  <D:response>\n    <D:href>" +
                xmlEscape(toHref(vpath)) +
                "</D:href>\n    <D:propstat>\n      <D:prop/>\n"
                "      <D:status>HTTP/1.1 200 OK</D:status>\n    </D:propstat>\n"
                "  </D:response>\n</D:multistatus>\n";
            res.status = 207;
            res.setHeader("Content-Type", "application/xml; charset=utf-8");
            res.body = std::move(xml);
            return;
        }

        res.setText("Phương thức WebDAV không hỗ trợ: " + method, 501);
    };

    for (const char* method :
         {"OPTIONS", "GET", "HEAD", "PUT", "DELETE", "PROPFIND", "PROPPATCH", "MKCOL", "COPY",
          "MOVE", "LOCK", "UNLOCK"}) {
        server.routePrefix(method, prefix, handler);
    }
    LOG_INFO(kTag, "WebDAV đã bật tại %s", prefix.c_str());
}

}  // namespace http
}  // namespace ttd
