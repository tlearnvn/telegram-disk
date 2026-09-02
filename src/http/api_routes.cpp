#include "http/api_routes.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <thread>

#include "app/app.h"
#include "common/fsutil.h"
#include "common/logging.h"
#include "common/strutil.h"
#include "common/timeutil.h"
#include "crypto/hash.h"
#include "crypto/random.h"
#include "http/assets.h"
#include "http/mime.h"
#include "storage/download_stream.h"
#include "version.h"

namespace ttd {
namespace http {

namespace {
constexpr const char* kTag = "api";
constexpr const char* kSessionCookie = "ttd_session";

// -------------------------------------------------------------------------
//  Tiện ích
// -------------------------------------------------------------------------
Json okJson() {
    Json j = Json::object();
    j.set("ok", true);
    return j;
}

void sendOk(Response& res, Json payload = Json::object()) {
    payload.set("ok", true);
    res.setJson(payload);
}

void sendError(Response& res, int status, const std::string& message) {
    Json j = Json::object();
    j.set("ok", false);
    j.set("error", message);
    res.setJson(j, status);
}

Json entryToJson(const db::FileEntry& e) {
    Json j = Json::object();
    j.set("id", e.id);
    j.set("parent_id", e.parentId);
    j.set("name", e.name);
    j.set("path", e.path);
    j.set("is_folder", e.isFolder);
    j.set("size", e.size);
    j.set("size_text", formatBytes(e.size));
    j.set("mime_type", e.mimeType);
    j.set("category", fileCategory(e.name, e.mimeType));
    j.set("sha256", e.sha256);
    j.set("chunk_count", static_cast<int64_t>(e.chunkCount));
    j.set("chunk_size", e.chunkSize);
    j.set("created_at", e.createdAt);
    j.set("modified_at", e.modifiedAt);
    j.set("created_text", formatDateTime(e.createdAt));
    j.set("modified_text", formatDateTime(e.modifiedAt));
    j.set("owner_id", static_cast<int64_t>(e.ownerId));
    j.set("trashed", e.trashed);
    j.set("trashed_at", e.trashedAt);
    j.set("starred", e.starred);
    j.set("note", e.note);
    j.set("shared", !e.shareToken.empty());
    j.set("share_token", e.shareToken);
    j.set("share_expires_at", e.shareExpiresAt);
    j.set("streamable", isStreamable(e.mimeType));
    return j;
}

Json userToJson(const db::UserEntry& u, uint64_t usage = 0) {
    Json j = Json::object();
    j.set("id", static_cast<int64_t>(u.id));
    j.set("username", u.username);
    j.set("display_name", u.displayName);
    j.set("is_admin", u.isAdmin);
    j.set("enabled", u.enabled);
    j.set("quota_bytes", u.quotaBytes);
    j.set("quota_text", u.quotaBytes ? formatBytes(u.quotaBytes) : std::string("Không giới hạn"));
    j.set("usage_bytes", usage);
    j.set("usage_text", formatBytes(usage));
    j.set("created_at", u.createdAt);
    j.set("created_text", formatDateTime(u.createdAt));
    j.set("last_login_at", u.lastLoginAt);
    j.set("last_login_text", u.lastLoginAt ? formatDateTime(u.lastLoginAt)
                                           : std::string("Chưa đăng nhập"));
    return j;
}

// Ngữ cảnh cho mỗi yêu cầu đã xác thực.
struct AuthContext {
    bool authenticated = false;
    db::UserEntry user;
};

AuthContext authenticate(app::App& app, const Request& req) {
    AuthContext ctx;
    std::string token = req.cookie(kSessionCookie);
    if (token.empty()) {
        // Cho phép dùng tiêu đề Authorization: Bearer để tiện gọi bằng script.
        std::string authHeader = req.header("Authorization");
        if (startsWith(toLower(authHeader), "bearer ")) token = trim(authHeader.substr(7));
    }
    if (!token.empty() && app.users()->authenticate(token, ctx.user)) {
        ctx.authenticated = true;
        return ctx;
    }
    std::string authHeader = req.header("Authorization");
    if (!authHeader.empty() && app.users()->authenticateBasic(authHeader, ctx.user)) {
        ctx.authenticated = true;
    }
    return ctx;
}

bool requireAuth(app::App& app, const Request& req, Response& res, AuthContext& ctx) {
    ctx = authenticate(app, req);
    if (!ctx.authenticated) {
        sendError(res, 401, "Bạn cần đăng nhập để thực hiện thao tác này.");
        return false;
    }
    return true;
}

bool requireAdmin(app::App& app, const Request& req, Response& res, AuthContext& ctx) {
    if (!requireAuth(app, req, res, ctx)) return false;
    if (!ctx.user.isAdmin) {
        sendError(res, 403, "Chỉ quản trị viên mới dùng được chức năng này.");
        return false;
    }
    return true;
}

std::string sessionCookie(const std::string& token, int64_t maxAgeSeconds, bool secure) {
    std::string c = std::string(kSessionCookie) + "=" + token +
                    "; Path=/; HttpOnly; SameSite=Lax";
    if (maxAgeSeconds > 0) c += "; Max-Age=" + std::to_string(maxAgeSeconds);
    else c += "; Max-Age=0";
    if (secure) c += "; Secure";
    return c;
}

// Gửi nội dung tệp, hỗ trợ Range.
void serveFileContent(app::App& app, const db::FileEntry& file, Request& req, Response& res,
                      bool forceDownload) {
    storage::StorageEngine* engine = app.engine();
    res.setHeader("Accept-Ranges", "bytes");
    res.setHeader("Content-Type", file.mimeType.empty() ? guessMimeType(file.name)
                                                        : file.mimeType);
    bool inlineDisplay = !forceDownload && isSafeInline(file.mimeType);
    res.setHeader("Content-Disposition", contentDisposition(file.name, inlineDisplay));
    res.setHeader("Last-Modified", formatHttpDate(file.modifiedAt));
    if (!file.sha256.empty()) res.setHeader("ETag", "\"" + file.sha256.substr(0, 32) + "\"");
    res.setHeader("X-Content-Type-Options", "nosniff");

    // Trả 304 nếu client đã có bản mới nhất.
    std::string ifNoneMatch = req.header("If-None-Match");
    if (!ifNoneMatch.empty() && !file.sha256.empty() &&
        ifNoneMatch.find(file.sha256.substr(0, 32)) != std::string::npos) {
        res.status = 304;
        return;
    }

    uint64_t start = 0;
    uint64_t length = file.size;
    std::string rangeHeader = req.header("Range");
    if (!rangeHeader.empty()) {
        storage::ByteRange range;
        bool unsatisfiable = false;
        if (storage::parseRangeHeader(rangeHeader, file.size, range, unsatisfiable)) {
            start = range.start;
            length = range.length();
            res.status = 206;
            res.setHeader("Content-Range", storage::makeContentRange(range, file.size));
        } else if (unsatisfiable) {
            res.status = 416;
            res.setHeader("Content-Range", storage::makeUnsatisfiedContentRange(file.size));
            res.body.clear();
            return;
        }
    }

    if (file.size == 0 || length == 0) {
        res.streamLength = 0;
        res.body.clear();
        res.setHeader("Content-Length", "0");
        return;
    }

    res.streamLength = length;
    db::FileEntry fileCopy = file;
    res.streamBody = [engine, fileCopy, start, length](const ResponseWriter& write) -> bool {
        std::string error;
        bool ok = engine->streamFileRange(
            fileCopy, start, length,
            [&](const uint8_t* data, size_t len) -> bool {
                return write(reinterpret_cast<const char*>(data), len);
            },
            error);
        if (!ok)
            LOG_WARN(kTag, "Lỗi khi gửi '%s': %s", fileCopy.name.c_str(), error.c_str());
        return ok;
    };
}

}  // namespace

// ---------------------------------------------------------------------------
//  Đăng ký tuyến
// ---------------------------------------------------------------------------
void registerApiRoutes(HttpServer& server, app::App& app) {
    Config& cfg = Config::instance();

    // ---- Phiên bản & trạng thái chung -------------------------------------
    server.route("GET", "/api/version", [&app](Request&, BodyReader&, Response& res) {
        Json j = okJson();
        j.set("app", std::string(version::kAppName));
        j.set("footer", std::string(version::kAppFooter));
        j.set("version", std::string(version::kVersion));
        j.set("build", static_cast<int64_t>(version::kBuildNumber));
        j.set("commit", std::string(version::kGitCommit));
        j.set("branch", std::string(version::kGitBranch));
        j.set("build_time", formatDateTime(version::kBuildEpoch));
        j.set("timezone", std::string(kSystemTimezoneName));
        j.set("server_time", formatDateTime(nowUnix()));
        j.set("uptime_seconds", nowUnix() - app.startedAt());
        j.set("uptime_text", formatDuration(nowUnix() - app.startedAt()));
        res.setJson(j);
    });

    // ---- Xác thực ---------------------------------------------------------
    server.route("POST", "/api/auth/login", [&app, &cfg](Request& req, BodyReader& body,
                                                         Response& res) {
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        std::string username = in["username"].asString();
        std::string password = in["password"].asString();
        if (username.empty() || password.empty()) {
            sendError(res, 400, "Hãy nhập đủ tên đăng nhập và mật khẩu.");
            return;
        }
        app::AuthResult result = app.users()->login(username, password,
                                                    req.header("User-Agent"), req.clientIp);
        if (!result.ok) {
            sendError(res, 401, result.error);
            return;
        }
        bool secure = startsWith(toLower(cfg.server.publicUrl), "https://");
        res.setHeader("Set-Cookie",
                      sessionCookie(result.token,
                                    static_cast<int64_t>(cfg.security.sessionDays) * 86400,
                                    secure));
        Json j = okJson();
        j.set("user", userToJson(result.user));
        j.set("expires_at", result.expiresAt);
        j.set("token", result.token);
        res.setJson(j);
    });

    server.route("POST", "/api/auth/logout", [&app](Request& req, BodyReader&, Response& res) {
        std::string token = req.cookie(kSessionCookie);
        if (!token.empty()) app.users()->logout(token);
        res.setHeader("Set-Cookie", sessionCookie("", 0, false));
        sendOk(res);
    });

    server.route("GET", "/api/auth/me", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx = authenticate(app, req);
        if (!ctx.authenticated) {
            sendError(res, 401, "Chưa đăng nhập.");
            return;
        }
        uint64_t usage = 0;
        std::string error;
        app.database()->usageByUser(ctx.user.id, usage, error);
        Json j = okJson();
        j.set("user", userToJson(ctx.user, usage));
        res.setJson(j);
    });

    server.route("POST", "/api/auth/password", [&app](Request& req, BodyReader& body,
                                                      Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        std::string oldPassword = in["old_password"].asString();
        std::string newPassword = in["new_password"].asString();
        if (!app::verifyPassword(oldPassword, ctx.user.passwordHash)) {
            sendError(res, 400, "Mật khẩu hiện tại không đúng.");
            return;
        }
        std::string error;
        if (!app.users()->changePassword(ctx.user.id, newPassword, error)) {
            sendError(res, 400, error);
            return;
        }
        res.setHeader("Set-Cookie", sessionCookie("", 0, false));
        Json j = okJson();
        j.set("message", std::string("Đã đổi mật khẩu. Hãy đăng nhập lại."));
        res.setJson(j);
    });

    // ---- Duyệt tệp --------------------------------------------------------
    server.route("GET", "/api/files", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;

        std::string path = req.param("path", "/");
        db::FileEntry folder;
        std::string error;
        if (!app.vfs()->resolve(path, folder, error)) {
            sendError(res, 404, "Không tìm thấy thư mục: " + path);
            return;
        }
        if (!folder.isFolder) {
            sendError(res, 400, "'" + path + "' là một tệp, không phải thư mục.");
            return;
        }

        db::ListOptions opts;
        opts.parentId = folder.id;
        opts.search = req.param("search");
        opts.sortBy = req.param("sort", "name");
        opts.descending = req.paramBool("desc", false);
        opts.limit = static_cast<int>(req.paramInt("limit", 500));
        opts.offset = static_cast<int>(req.paramInt("offset", 0));
        opts.onlyTrashed = req.paramBool("trash", false);
        opts.onlyStarred = req.paramBool("starred", false);
        if (!ctx.user.isAdmin) opts.ownerId = ctx.user.id;

        std::vector<db::FileEntry> entries;
        if (!app.database()->listEntries(opts, entries, error)) {
            sendError(res, 500, "Không đọc được danh sách: " + error);
            return;
        }
        uint64_t total = 0;
        app.database()->countEntries(opts, total, error);

        Json list = Json::array();
        for (const auto& e : entries) list.push(entryToJson(e));

        Json crumbs = Json::array();
        Json rootCrumb = Json::object();
        rootCrumb.set("id", static_cast<int64_t>(0));
        rootCrumb.set("name", std::string("Ổ đĩa của tôi"));
        rootCrumb.set("path", std::string("/"));
        crumbs.push(rootCrumb);
        for (const auto& c : app.vfs()->breadcrumb(folder.id)) {
            Json j = Json::object();
            j.set("id", c.id);
            j.set("name", c.name);
            j.set("path", c.path);
            crumbs.push(j);
        }

        Json j = okJson();
        j.set("path", folder.path);
        j.set("folder_id", folder.id);
        j.set("entries", list);
        j.set("breadcrumb", crumbs);
        j.set("total", total);
        j.set("offset", static_cast<int64_t>(opts.offset));
        j.set("limit", static_cast<int64_t>(opts.limit));
        res.setJson(j);
    });

    server.routePrefix("GET", "/api/file/", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        int64_t id = 0;
        parseInt64(req.path.substr(std::strlen("/api/file/")), id);
        db::FileEntry entry;
        std::string error;
        if (!app.database()->getEntry(id, entry, error)) {
            sendError(res, 404, "Không tìm thấy mục.");
            return;
        }
        if (!ctx.user.isAdmin && entry.ownerId != ctx.user.id) {
            sendError(res, 403, "Bạn không có quyền xem mục này.");
            return;
        }
        Json j = okJson();
        j.set("entry", entryToJson(entry));
        if (entry.isFolder) {
            j.set("folder_size", app.vfs()->folderSize(entry.id));
        } else {
            std::vector<db::ChunkEntry> chunks;
            app.database()->listChunks(entry.id, chunks, error);
            Json chunkList = Json::array();
            for (const auto& c : chunks) {
                Json cj = Json::object();
                cj.set("index", static_cast<int64_t>(c.index));
                cj.set("offset", c.offset);
                cj.set("size", c.size);
                cj.set("size_text", formatBytes(c.size));
                cj.set("dc_id", static_cast<int64_t>(c.dcId));
                cj.set("account_id", static_cast<int64_t>(c.accountId));
                cj.set("message_id", c.messageId);
                cj.set("created_text", formatDateTime(c.createdAt));
                chunkList.push(cj);
            }
            j.set("chunks", chunkList);
        }
        res.setJson(j);
    });

    // ---- Thao tác trên tệp/thư mục ---------------------------------------
    server.route("POST", "/api/folders", [&app](Request& req, BodyReader& body, Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        app::VfsResult r = app.vfs()->createFolder(in["parent"].asString("/"),
                                                   in["name"].asString(), ctx.user.id);
        if (!r.ok) {
            sendError(res, 400, r.error);
            return;
        }
        Json j = okJson();
        j.set("entry", entryToJson(r.entry));
        res.setJson(j);
    });

    server.route("POST", "/api/files/rename", [&app](Request& req, BodyReader& body,
                                                     Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        app::VfsResult r = app.vfs()->rename(in["id"].asInt64(), in["name"].asString());
        if (!r.ok) {
            sendError(res, 400, r.error);
            return;
        }
        Json j = okJson();
        j.set("entry", entryToJson(r.entry));
        res.setJson(j);
    });

    server.route("POST", "/api/files/move", [&app](Request& req, BodyReader& body,
                                                   Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        body.readAll(req.body, 256 * 1024);
        Json in = req.json();
        std::string target = in["target"].asString("/");
        Json results = Json::array();
        int failed = 0;
        for (const auto& idValue : in["ids"].arr()) {
            app::VfsResult r = app.vfs()->move(idValue.asInt64(), target);
            Json item = Json::object();
            item.set("id", idValue.asInt64());
            item.set("ok", r.ok);
            if (!r.ok) {
                item.set("error", r.error);
                ++failed;
            }
            results.push(item);
        }
        Json j = okJson();
        j.set("results", results);
        j.set("failed", static_cast<int64_t>(failed));
        res.setJson(j);
    });

    server.route("POST", "/api/files/copy", [&app](Request& req, BodyReader& body,
                                                   Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        body.readAll(req.body, 256 * 1024);
        Json in = req.json();
        std::string target = in["target"].asString("/");
        Json results = Json::array();
        for (const auto& idValue : in["ids"].arr()) {
            app::VfsResult r = app.vfs()->copy(idValue.asInt64(), target, ctx.user.id);
            Json item = Json::object();
            item.set("id", idValue.asInt64());
            item.set("ok", r.ok);
            if (!r.ok) item.set("error", r.error);
            else item.set("entry", entryToJson(r.entry));
            results.push(item);
        }
        Json j = okJson();
        j.set("results", results);
        res.setJson(j);
    });

    auto bulkOperation = [&app](Request& req, BodyReader& body, Response& res,
                                const std::string& operation) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        body.readAll(req.body, 256 * 1024);
        Json in = req.json();
        Json results = Json::array();
        int failed = 0;
        for (const auto& idValue : in["ids"].arr()) {
            int64_t id = idValue.asInt64();
            app::VfsResult r;
            if (operation == "trash") r = app.vfs()->trash(id);
            else if (operation == "restore") r = app.vfs()->restore(id);
            else if (operation == "purge") r = app.vfs()->purge(id);
            Json item = Json::object();
            item.set("id", id);
            item.set("ok", r.ok);
            if (!r.ok) {
                item.set("error", r.error);
                ++failed;
            }
            results.push(item);
        }
        Json j = okJson();
        j.set("results", results);
        j.set("failed", static_cast<int64_t>(failed));
        res.setJson(j);
    };

    server.route("POST", "/api/files/trash",
                 [bulkOperation](Request& req, BodyReader& body, Response& res) {
                     bulkOperation(req, body, res, "trash");
                 });
    server.route("POST", "/api/files/restore",
                 [bulkOperation](Request& req, BodyReader& body, Response& res) {
                     bulkOperation(req, body, res, "restore");
                 });
    server.route("POST", "/api/files/delete",
                 [bulkOperation](Request& req, BodyReader& body, Response& res) {
                     bulkOperation(req, body, res, "purge");
                 });

    server.route("POST", "/api/trash/empty", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        int removed = app.vfs()->emptyTrash(ctx.user.isAdmin ? 0 : ctx.user.id);
        Json j = okJson();
        j.set("removed", static_cast<int64_t>(removed));
        j.set("message", "Đã dọn " + std::to_string(removed) + " mục khỏi thùng rác.");
        res.setJson(j);
    });

    server.route("POST", "/api/files/star", [&app](Request& req, BodyReader& body,
                                                   Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        app::VfsResult r = app.vfs()->setStarred(in["id"].asInt64(), in["starred"].asBool(true));
        if (!r.ok) {
            sendError(res, 400, r.error);
            return;
        }
        Json j = okJson();
        j.set("entry", entryToJson(r.entry));
        res.setJson(j);
    });

    server.route("POST", "/api/files/note", [&app](Request& req, BodyReader& body,
                                                   Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        app::VfsResult r = app.vfs()->setNote(in["id"].asInt64(), in["note"].asString());
        if (!r.ok) {
            sendError(res, 400, r.error);
            return;
        }
        Json j = okJson();
        j.set("entry", entryToJson(r.entry));
        res.setJson(j);
    });

    server.route("POST", "/api/files/share", [&app, &cfg](Request& req, BodyReader& body,
                                                          Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        int64_t expires = in["expires_seconds"].asInt64(0);
        app::VfsResult r = app.vfs()->createShare(in["id"].asInt64(), expires);
        if (!r.ok) {
            sendError(res, 400, r.error);
            return;
        }
        std::string base = cfg.server.publicUrl;
        if (base.empty()) base = "http://" + req.header("Host", "localhost");
        while (!base.empty() && base.back() == '/') base.pop_back();
        Json j = okJson();
        j.set("entry", entryToJson(r.entry));
        j.set("url", base + "/s/" + r.entry.shareToken);
        res.setJson(j);
    });

    server.route("POST", "/api/files/unshare", [&app](Request& req, BodyReader& body,
                                                      Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        app::VfsResult r = app.vfs()->revokeShare(in["id"].asInt64());
        if (!r.ok) {
            sendError(res, 400, r.error);
            return;
        }
        sendOk(res);
    });

    // ---- Tải xuống --------------------------------------------------------
    server.routePrefix("GET", "/d/", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        std::string rest = req.path.substr(3);
        size_t slash = rest.find('/');
        std::string idStr = slash == std::string::npos ? rest : rest.substr(0, slash);
        int64_t id = 0;
        parseInt64(idStr, id);

        db::FileEntry file;
        std::string error;
        if (!app.database()->getEntry(id, file, error) || file.isFolder) {
            sendError(res, 404, "Không tìm thấy tệp.");
            return;
        }
        if (!ctx.user.isAdmin && file.ownerId != ctx.user.id) {
            sendError(res, 403, "Bạn không có quyền tải tệp này.");
            return;
        }
        serveFileContent(app, file, req, res, req.paramBool("download", false));
    });

    server.routePrefix("HEAD", "/d/", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        std::string rest = req.path.substr(3);
        size_t slash = rest.find('/');
        int64_t id = 0;
        parseInt64(slash == std::string::npos ? rest : rest.substr(0, slash), id);
        db::FileEntry file;
        std::string error;
        if (!app.database()->getEntry(id, file, error) || file.isFolder) {
            sendError(res, 404, "Không tìm thấy tệp.");
            return;
        }
        serveFileContent(app, file, req, res, false);
    });

    // Liên kết chia sẻ công khai.
    server.routePrefix("GET", "/s/", [&app, &cfg](Request& req, BodyReader&, Response& res) {
        if (!cfg.security.publicShareLinks) {
            sendError(res, 403, "Quản trị viên đã tắt liên kết chia sẻ công khai.");
            return;
        }
        std::string token = req.path.substr(3);
        size_t slash = token.find('/');
        if (slash != std::string::npos) token = token.substr(0, slash);
        db::FileEntry file;
        std::string error;
        if (!app.vfs()->resolveShare(token, file, error)) {
            sendError(res, 404, error);
            return;
        }
        if (file.isFolder) {
            sendError(res, 400, "Hiện chỉ chia sẻ được tệp đơn lẻ.");
            return;
        }
        serveFileContent(app, file, req, res, req.paramBool("download", false));
    });

    // ---- Tải lên ----------------------------------------------------------
    server.route("POST", "/api/upload/init", [&app](Request& req, BodyReader& body,
                                                    Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        body.readAll(req.body, 256 * 1024);
        Json in = req.json();

        storage::UploadInitRequest ur;
        ur.name = in["name"].asString();
        ur.targetFolderPath = in["path"].asString("/");
        ur.totalSize = in["size"].asUInt64(0);
        ur.mimeType = in["mime_type"].asString();
        ur.quickHash = in["quick_hash"].asString();
        ur.sha256 = in["sha256"].asString();
        ur.policy = storage::parseConflictPolicy(in["policy"].asString("ask"));
        ur.ownerId = ctx.user.id;

        // Kiểm tra hạn mức dung lượng của người dùng.
        if (ctx.user.quotaBytes > 0) {
            uint64_t usage = 0;
            std::string usageError;
            app.database()->usageByUser(ctx.user.id, usage, usageError);
            if (usage + ur.totalSize > ctx.user.quotaBytes) {
                sendError(res, 507,
                          "Vượt quá hạn mức dung lượng (" + formatBytes(ctx.user.quotaBytes) +
                              "). Đã dùng " + formatBytes(usage) + ".");
                return;
            }
        }

        storage::UploadInitResult result = app.uploads()->begin(ur);
        if (!result.ok) {
            sendError(res, 400, result.error);
            return;
        }
        Json j = okJson();
        j.set("upload_id", result.uploadId);
        j.set("chunk_size", result.chunkSize);
        j.set("browser_chunk_size", result.browserChunkSize);
        j.set("needs_decision", result.needsDecision);
        j.set("skipped", result.skipped);
        j.set("linked", result.linked);
        j.set("linked_file_id", result.linkedFileId);
        j.set("message", result.message);
        Json dups = Json::array();
        for (const auto& d : result.duplicates) {
            Json dj = Json::object();
            dj.set("file_id", d.fileId);
            dj.set("name", d.name);
            dj.set("path", d.path);
            dj.set("size", d.size);
            dj.set("size_text", formatBytes(d.size));
            dj.set("modified_at", d.modifiedAt);
            dj.set("modified_text", formatDateTime(d.modifiedAt));
            dj.set("reason", d.reason);
            dups.push(dj);
        }
        j.set("duplicates", dups);
        res.setJson(j);
    });

    // Nhận dữ liệu thô theo luồng — đây là đường đi chính của tệp lớn.
    server.routePrefix("PUT", "/api/upload/", [&app](Request& req, BodyReader& body,
                                                     Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        std::string rest = req.path.substr(std::strlen("/api/upload/"));
        size_t slash = rest.find('/');
        std::string id = slash == std::string::npos ? rest : rest.substr(0, slash);
        std::string action = slash == std::string::npos ? "" : rest.substr(slash + 1);
        if (action != "data") {
            sendError(res, 404, "Đường dẫn tải lên không hợp lệ.");
            return;
        }

        auto session = app.uploads()->find(id);
        if (!session) {
            sendError(res, 404, "Phiên tải lên không tồn tại hoặc đã kết thúc.");
            return;
        }

        // Nối lại sau khi rớt mạng: trình duyệt gửi kèm X-Upload-Offset để nói
        // "tôi đang gửi tiếp từ byte thứ N". Nếu N không khớp với số byte máy
        // chủ đã nhận thì phải từ chối — ghi đè lên thì dữ liệu lệch âm thầm,
        // mãi tới lúc đối chiếu SHA-256 ở cuối mới lộ ra. Trả 409 kèm vị trí
        // đúng để trình duyệt cắt lại cho khớp.
        std::string offsetHeader = req.header("X-Upload-Offset");
        if (!offsetHeader.empty()) {
            uint64_t want = session->receivedBytes();
            uint64_t got = strtoull(offsetHeader.c_str(), nullptr, 10);
            if (got != want) {
                Json j = Json::object();
                j.set("ok", false);
                j.set("error", "Vị trí gửi tiếp không khớp: máy chủ đã nhận " +
                                   std::to_string(want) + " byte.");
                j.set("received", want);
                res.setJson(j, 409);
                return;
            }
        }

        uint8_t buffer[256 * 1024];
        uint64_t received = 0;
        std::string error;
        while (true) {
            long n = body.read(buffer, sizeof(buffer));
            if (n < 0) {
                // Trình duyệt ngắt kết nối giữa chừng — coi như tạm dừng, giữ phiên
                // để người dùng có thể tiếp tục. Phần đã đọc được thì đã ghi vào
                // phiên rồi, nên vị trí gửi tiếp của trình duyệt đã cũ: trả 409
                // kèm vị trí đúng, cùng mã với lỗi lệch vị trí, để bên kia biết
                // đây là chuyện nối lại được chứ không phải hỏng hẳn.
                LOG_WARN(kTag, "[%s] Kết nối tải lên bị ngắt sau %s", id.c_str(),
                         formatBytes(received).c_str());
                Json j = Json::object();
                j.set("ok", false);
                j.set("error", "Kết nối bị ngắt giữa chừng.");
                j.set("received", session->receivedBytes());
                res.closeConnection = true;
                res.setJson(j, 409);
                return;
            }
            if (n == 0) break;
            if (!session->receive(buffer, static_cast<size_t>(n), error)) {
                // Giới hạn tần suất là tạm thời: trả 503 kèm Retry-After để
                // vòng thử lại của trình duyệt chờ rồi nối tiếp, thay vì 500
                // làm nó bỏ hẳn phiên.
                int giayCho = 0;
                if (storage::laLoiTamThoi(error, giayCho)) {
                    LOG_WARN(kTag, "[%s] Tạm thời không ghi được: %s", id.c_str(), error.c_str());
                    res.setHeader("Retry-After", std::to_string(giayCho > 0 ? giayCho : 5));
                    Json j = Json::object();
                    j.set("ok", false);
                    j.set("error", error);
                    j.set("received", session->receivedBytes());
                    j.set("retry_after", static_cast<int64_t>(giayCho > 0 ? giayCho : 5));
                    res.setJson(j, 503);
                    return;
                }
                sendError(res, 500, error);
                return;
            }
            received += static_cast<uint64_t>(n);
        }

        storage::UploadProgress p = session->progress();
        Json j = okJson();
        j.set("received", p.receivedBytes);
        j.set("stored", p.storedBytes);
        j.set("chunk_index", static_cast<int64_t>(p.chunkIndex));
        j.set("chunk_total", static_cast<int64_t>(p.chunkTotal));
        j.set("speed", p.speedBytesPerSecond);
        j.set("account", p.currentAccount);
        res.setJson(j);
    });

    server.routePrefix("POST", "/api/upload/", [&app](Request& req, BodyReader& body,
                                                      Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        std::string rest = req.path.substr(std::strlen("/api/upload/"));
        size_t slash = rest.find('/');
        if (slash == std::string::npos) {
            sendError(res, 404, "Thiếu hành động.");
            return;
        }
        std::string id = rest.substr(0, slash);
        std::string action = rest.substr(slash + 1);

        if (action == "complete") {
            body.readAll(req.body, 64 * 1024);
            db::FileEntry entry;
            std::string error;
            if (!app.uploads()->complete(id, entry, error)) {
                sendError(res, 500, error);
                return;
            }
            Json j = okJson();
            j.set("entry", entryToJson(entry));
            j.set("message", std::string("Đã lưu xong."));
            res.setJson(j);
            return;
        }
        if (action == "cancel") {
            body.readAll(req.body, 64 * 1024);
            Json in = req.json();
            std::string reason = in["reason"].asString("Người dùng huỷ");
            bool found = app.uploads()->cancel(id, reason);
            Json j = okJson();
            j.set("cancelled", found);
            j.set("message", found ? std::string("Đã huỷ và dọn dữ liệu.")
                                   : std::string("Phiên đã kết thúc trước đó."));
            res.setJson(j);
            return;
        }
        sendError(res, 404, "Hành động không hỗ trợ: " + action);
    });

    server.routePrefix("GET", "/api/upload/", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        std::string rest = req.path.substr(std::strlen("/api/upload/"));
        size_t slash = rest.find('/');
        std::string id = slash == std::string::npos ? rest : rest.substr(0, slash);
        auto session = app.uploads()->find(id);
        if (!session) {
            sendError(res, 404, "Phiên tải lên không tồn tại.");
            return;
        }
        storage::UploadProgress p = session->progress();
        Json j = okJson();
        j.set("id", p.id);
        j.set("name", p.name);
        j.set("total", p.totalSize);
        j.set("received", p.receivedBytes);
        j.set("stored", p.storedBytes);
        j.set("state", std::string(storage::uploadStateName(p.state)));
        j.set("state_text", std::string(storage::uploadStateNameVi(p.state)));
        j.set("message", p.message);
        j.set("speed", p.speedBytesPerSecond);
        j.set("eta_seconds", p.etaSeconds);
        res.setJson(j);
    });

    server.route("GET", "/api/uploads", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        auto list = app.uploads()->activeUploads(ctx.user.isAdmin ? 0 : ctx.user.id);
        Json arr = Json::array();
        for (const auto& p : list) {
            Json j = Json::object();
            j.set("id", p.id);
            j.set("name", p.name);
            j.set("folder", p.targetFolder);
            j.set("total", p.totalSize);
            j.set("total_text", formatBytes(p.totalSize));
            j.set("received", p.receivedBytes);
            j.set("stored", p.storedBytes);
            j.set("chunk_index", static_cast<int64_t>(p.chunkIndex));
            j.set("chunk_total", static_cast<int64_t>(p.chunkTotal));
            j.set("state", std::string(storage::uploadStateName(p.state)));
            j.set("state_text", std::string(storage::uploadStateNameVi(p.state)));
            j.set("message", p.message);
            j.set("account", p.currentAccount);
            j.set("speed", p.speedBytesPerSecond);
            j.set("speed_text", formatSpeed(p.speedBytesPerSecond));
            j.set("eta_seconds", p.etaSeconds);
            j.set("eta_text", p.etaSeconds > 0 ? formatDuration(p.etaSeconds) : std::string("—"));
            j.set("started_text", formatDateTime(p.startedAt));
            arr.push(j);
        }
        Json j = okJson();
        j.set("uploads", arr);
        res.setJson(j);
    });

    // ---- Thống kê ---------------------------------------------------------
    server.route("GET", "/api/stats", [&app, &cfg](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        storage::EngineStats s = app.engine()->stats();

        Json j = okJson();
        Json storageJson = Json::object();
        storageJson.set("total_bytes", s.totalBytes);
        storageJson.set("total_text", formatBytes(s.totalBytes));
        storageJson.set("file_count", s.fileCount);
        storageJson.set("folder_count", s.folderCount);
        storageJson.set("chunk_count", s.chunkCount);
        // Dung lượng thật chiếm trên Telegram, sau khi trừ phần dùng chung của
        // các tệp trùng nội dung.
        storageJson.set("physical_bytes", s.physicalBytes);
        storageJson.set("physical_text", formatBytes(s.physicalBytes));
        storageJson.set("unique_chunk_count", s.uniqueChunkCount);
        uint64_t saved = s.totalBytes > s.physicalBytes ? s.totalBytes - s.physicalBytes : 0;
        storageJson.set("saved_bytes", saved);
        storageJson.set("saved_text", formatBytes(saved));
        storageJson.set("trashed_bytes", s.trashedBytes);
        storageJson.set("trashed_text", formatBytes(s.trashedBytes));
        storageJson.set("trashed_count", s.trashedCount);
        storageJson.set("uploaded_bytes", s.uploadedBytes);
        storageJson.set("uploaded_text", formatBytes(s.uploadedBytes));
        storageJson.set("downloaded_bytes", s.downloadedBytes);
        storageJson.set("downloaded_text", formatBytes(s.downloadedBytes));
        storageJson.set("chunk_size", cfg.storage.chunkSize);
        storageJson.set("chunk_size_text", formatBytes(cfg.storage.chunkSize));
        storageJson.set("buffer_mode", cfg.storage.bufferMode);
        j.set("storage", storageJson);

        Json cache = Json::object();
        cache.set("used", s.cacheUsed);
        cache.set("used_text", formatBytes(s.cacheUsed));
        cache.set("capacity", s.cacheCapacity);
        cache.set("capacity_text", formatBytes(s.cacheCapacity));
        cache.set("hits", s.cacheHits);
        cache.set("misses", s.cacheMisses);
        j.set("cache", cache);

        Json backend = Json::object();
        backend.set("name", s.backendName);
        backend.set("ready", s.backendReady);
        backend.set("message", s.backendMessage);
        backend.set("ready_accounts", static_cast<int64_t>(s.readyAccounts));
        backend.set("total_accounts", static_cast<int64_t>(s.totalAccounts));
        backend.set("channel_title", cfg.telegram.channelTitle);
        backend.set("channel_id", cfg.telegram.channelId);
        j.set("backend", backend);

        Json server_ = Json::object();
        server_.set("requests", app.server()->requestsHandled());
        server_.set("bytes_sent", app.server()->bytesSent());
        server_.set("bytes_sent_text", formatBytes(app.server()->bytesSent()));
        server_.set("bytes_received", app.server()->bytesReceived());
        server_.set("bytes_received_text", formatBytes(app.server()->bytesReceived()));
        server_.set("connections", static_cast<int64_t>(app.server()->activeConnections()));
        server_.set("uptime_seconds", nowUnix() - app.startedAt());
        server_.set("uptime_text", formatDuration(nowUnix() - app.startedAt()));
        j.set("server", server_);

        Json system = Json::object();
        system.set("free_disk", freeDiskSpace(cfg.dataRoot()));
        system.set("free_disk_text", formatBytes(freeDiskSpace(cfg.dataRoot())));
        system.set("total_memory", totalSystemMemory());
        system.set("total_memory_text", formatBytes(totalSystemMemory()));
        system.set("available_memory", availableSystemMemory());
        system.set("available_memory_text", formatBytes(availableSystemMemory()));
        system.set("database", app.database()->description());
        system.set("timezone", std::string(kSystemTimezoneName));
        system.set("server_time", formatDateTime(nowUnix()));
        j.set("system", system);

        uint64_t counters[5];
        Logger::instance().counters(counters);
        Json logs = Json::object();
        logs.set("trace", counters[0]);
        logs.set("debug", counters[1]);
        logs.set("info", counters[2]);
        logs.set("warn", counters[3]);
        logs.set("error", counters[4]);
        j.set("logs", logs);

        if (ctx.user.quotaBytes > 0 || !ctx.user.isAdmin) {
            uint64_t usage = 0;
            std::string usageError;
            app.database()->usageByUser(ctx.user.id, usage, usageError);
            Json quota = Json::object();
            quota.set("used", usage);
            quota.set("used_text", formatBytes(usage));
            quota.set("limit", ctx.user.quotaBytes);
            quota.set("limit_text", ctx.user.quotaBytes ? formatBytes(ctx.user.quotaBytes)
                                                        : std::string("Không giới hạn"));
            j.set("quota", quota);
        }
        res.setJson(j);
    });

    // ---- Nhật ký ----------------------------------------------------------
    server.route("GET", "/api/logs", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        uint64_t after = static_cast<uint64_t>(req.paramInt("after", 0));
        size_t limit = static_cast<size_t>(req.paramInt("limit", 300));
        if (limit > 2000) limit = 2000;
        LogLevel minLevel = parseLogLevel(req.param("level", "trace"), LogLevel::Trace);
        std::string filter = req.param("filter");

        auto records = Logger::instance().recent(after, limit, minLevel, filter);
        Json arr = Json::array();
        for (const auto& r : records) {
            Json j = Json::object();
            j.set("seq", r.seq);
            j.set("time", formatDateTimeMillis(r.timeMillis));
            j.set("level", std::string(logLevelName(r.level)));
            j.set("level_vi", std::string(logLevelNameVi(r.level)));
            j.set("tag", r.tag);
            j.set("message", r.message);
            arr.push(j);
        }
        Json j = okJson();
        j.set("records", arr);
        j.set("last_seq", Logger::instance().lastSeq());
        res.setJson(j);
    });

    server.route("POST", "/api/logs/clear", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        Logger::instance().clearMemory();
        sendOk(res);
    });

    // Luồng nhật ký thời gian thực (Server-Sent Events).
    server.route("GET", "/api/logs/stream", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAuth(app, req, res, ctx)) return;
        res.setHeader("Content-Type", "text/event-stream; charset=utf-8");
        res.setHeader("Cache-Control", "no-cache");
        res.setHeader("X-Accel-Buffering", "no");
        res.streamLength = 0;

        uint64_t after = static_cast<uint64_t>(req.paramInt("after", 0));
        LogLevel minLevel = parseLogLevel(req.param("level", "trace"), LogLevel::Trace);
        std::string filter = req.param("filter");

        res.streamBody = [after, minLevel, filter](const ResponseWriter& write) -> bool {
            uint64_t last = after;
            int64_t started = monotonicMillis();
            // Giữ luồng tối đa 10 phút rồi để trình duyệt tự kết nối lại.
            while (monotonicMillis() - started < 600000) {
                auto records = Logger::instance().recent(last, 200, minLevel, filter);
                for (const auto& r : records) {
                    last = r.seq;
                    Json j = Json::object();
                    j.set("seq", r.seq);
                    j.set("time", formatDateTimeMillis(r.timeMillis));
                    j.set("level", std::string(logLevelName(r.level)));
                    j.set("level_vi", std::string(logLevelNameVi(r.level)));
                    j.set("tag", r.tag);
                    j.set("message", r.message);
                    std::string line = "data: " + j.dump() + "\n\n";
                    if (!write(line.data(), line.size())) return true;
                }
                if (records.empty()) {
                    std::string ping = ": nhip\n\n";
                    if (!write(ping.data(), ping.size())) return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(700));
                }
            }
            return true;
        };
    });

    // ---- Cài đặt (quản trị) ----------------------------------------------
    server.route("GET", "/api/settings", [&app, &cfg](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        Json settings = cfg.toJson();
        // Không trả mật khẩu ra ngoài.
        if (settings.has("database")) settings["database"].set("mysql_password", std::string(""));
        Json j = okJson();
        j.set("settings", settings);
        j.set("config_path", cfg.path());
        j.set("schema_layer", static_cast<int64_t>(app.schema().layer()));
        j.set("schema_constructors", static_cast<int64_t>(app.schema().size()));
        res.setJson(j);
    });

    server.route("POST", "/api/settings", [&app](Request& req, BodyReader& body, Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        body.readAll(req.body, 1024 * 1024);
        Json in = req.json();
        std::string error;
        if (!app.applySettings(in, error)) {
            sendError(res, 400, error);
            return;
        }
        Json j = okJson();
        j.set("message",
              std::string("Đã lưu cài đặt. Một số thay đổi (cổng, số luồng, loại cơ sở dữ "
                          "liệu) cần khởi động lại ứng dụng."));
        res.setJson(j);
    });

    // ---- Người dùng (quản trị) -------------------------------------------
    server.route("GET", "/api/users", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        std::vector<db::UserEntry> users;
        std::string error;
        if (!app.users()->listUsers(users, error)) {
            sendError(res, 500, error);
            return;
        }
        Json arr = Json::array();
        for (const auto& u : users) {
            uint64_t usage = 0;
            std::string usageError;
            app.database()->usageByUser(u.id, usage, usageError);
            arr.push(userToJson(u, usage));
        }
        Json j = okJson();
        j.set("users", arr);
        res.setJson(j);
    });

    server.route("POST", "/api/users", [&app](Request& req, BodyReader& body, Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        db::UserEntry created;
        std::string error;
        uint64_t quota = in["quota_bytes"].isString()
                             ? parseSizeString(in["quota_bytes"].asString(), 0)
                             : in["quota_bytes"].asUInt64(0);
        if (!app.users()->createUser(in["username"].asString(), in["password"].asString(),
                                     in["display_name"].asString(), in["is_admin"].asBool(false),
                                     quota, created, error)) {
            sendError(res, 400, error);
            return;
        }
        Json j = okJson();
        j.set("user", userToJson(created));
        res.setJson(j);
    });

    server.route("POST", "/api/users/update", [&app](Request& req, BodyReader& body,
                                                     Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        int id = in["id"].asInt();
        std::string error;

        if (in.has("password") && !in["password"].asString().empty()) {
            if (!app.users()->changePassword(id, in["password"].asString(), error)) {
                sendError(res, 400, error);
                return;
            }
        }
        if (in.has("enabled")) {
            if (!app.users()->setEnabled(id, in["enabled"].asBool(true), error)) {
                sendError(res, 400, error);
                return;
            }
        }
        if (in.has("quota_bytes") || in.has("display_name") || in.has("is_admin")) {
            db::UserEntry user;
            if (!app.database()->getUser(id, user, error)) {
                sendError(res, 404, error);
                return;
            }
            if (in.has("display_name")) user.displayName = in["display_name"].asString();
            if (in.has("is_admin")) user.isAdmin = in["is_admin"].asBool(user.isAdmin);
            if (in.has("quota_bytes"))
                user.quotaBytes = in["quota_bytes"].isString()
                                      ? parseSizeString(in["quota_bytes"].asString(), 0)
                                      : in["quota_bytes"].asUInt64(0);
            if (!app.database()->updateUser(user, error)) {
                sendError(res, 400, error);
                return;
            }
        }
        sendOk(res);
    });

    server.route("POST", "/api/users/delete", [&app](Request& req, BodyReader& body,
                                                     Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        int id = in["id"].asInt();
        if (id == ctx.user.id) {
            sendError(res, 400, "Không thể tự xoá tài khoản đang đăng nhập.");
            return;
        }
        std::string error;
        if (!app.users()->deleteUser(id, error)) {
            sendError(res, 400, error);
            return;
        }
        sendOk(res);
    });

    // ---- Tài khoản Telegram (quản trị) -----------------------------------
    server.route("GET", "/api/accounts", [&app](Request& req, BodyReader&, Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        Json arr = Json::array();
        if (app.pool()) {
            for (const auto& s : app.pool()->statuses()) {
                Json j = Json::object();
                j.set("id", static_cast<int64_t>(s.id));
                j.set("label", s.label);
                j.set("phone", s.phone);
                j.set("display_name", s.displayName);
                j.set("enabled", s.enabled);
                j.set("authorized", s.authorized);
                j.set("connected", s.connected);
                j.set("home_dc", static_cast<int64_t>(s.homeDc));
                j.set("active_uploads", static_cast<int64_t>(s.activeUploads));
                j.set("uploaded", s.bytesUploaded);
                j.set("uploaded_text", formatBytes(s.bytesUploaded));
                j.set("downloaded", s.bytesDownloaded);
                j.set("downloaded_text", formatBytes(s.bytesDownloaded));
                j.set("status", s.status);
                j.set("last_error", s.lastError);
                arr.push(j);
            }
        }
        Json j = okJson();
        j.set("accounts", arr);
        j.set("backend", app.pool() ? std::string("telegram") : std::string("local"));
        res.setJson(j);
    });

    server.route("POST", "/api/accounts/add", [&app](Request& req, BodyReader& body,
                                                     Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        int accountId = 0;
        tg::LoginResult loginResult;
        std::string error;
        if (!app.addAccountAndSendCode(in["label"].asString(), in["phone"].asString(), accountId,
                                       loginResult, error)) {
            sendError(res, 400, error);
            return;
        }
        Json j = okJson();
        j.set("account_id", static_cast<int64_t>(accountId));
        j.set("message", loginResult.message);
        j.set("code_type", loginResult.state.codeTypeText);
        j.set("code_length", static_cast<int64_t>(loginResult.state.codeLength));
        j.set("needs_code", true);
        res.setJson(j);
    });

    server.route("POST", "/api/accounts/code", [&app](Request& req, BodyReader& body,
                                                      Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        tg::LoginResult loginResult;
        std::string error;
        if (!app.submitAccountCode(in["account_id"].asInt(), in["code"].asString(), loginResult,
                                   error)) {
            sendError(res, 400, error);
            return;
        }
        Json j = okJson();
        j.set("needs_password", loginResult.needsPassword);
        j.set("password_hint", loginResult.state.passwordHint);
        j.set("display_name", loginResult.displayName);
        j.set("message", loginResult.message);
        res.setJson(j);
    });

    server.route("POST", "/api/accounts/password", [&app](Request& req, BodyReader& body,
                                                          Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        tg::LoginResult loginResult;
        std::string error;
        if (!app.submitAccountPassword(in["account_id"].asInt(), in["password"].asString(),
                                       loginResult, error)) {
            sendError(res, 400, error);
            return;
        }
        Json j = okJson();
        j.set("display_name", loginResult.displayName);
        j.set("message", loginResult.message);
        res.setJson(j);
    });

    server.route("POST", "/api/accounts/remove", [&app](Request& req, BodyReader& body,
                                                        Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        std::string error;
        if (!app.removeAccount(in["account_id"].asInt(), error)) {
            sendError(res, 400, error);
            return;
        }
        sendOk(res);
    });

    server.route("POST", "/api/accounts/toggle", [&app](Request& req, BodyReader& body,
                                                        Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        int id = in["account_id"].asInt();
        bool enabled = in["enabled"].asBool(true);
        if (app.pool()) app.pool()->setAccountEnabled(id, enabled);
        std::vector<db::AccountEntry> accounts;
        std::string error;
        if (app.database()->listAccounts(accounts, error)) {
            for (auto& a : accounts) {
                if (a.id != id) continue;
                a.enabled = enabled;
                std::string updateError;
                app.database()->updateAccount(a, updateError);
            }
        }
        sendOk(res);
    });

    server.route("POST", "/api/accounts/connect", [&app](Request& req, BodyReader&,
                                                         Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        if (!app.pool()) {
            sendError(res, 400, "Đang ở chế độ thử nghiệm.");
            return;
        }
        app.pool()->connectAll();
        sendOk(res);
    });

    // ---- Siêu nhóm lưu trữ ------------------------------------------------
    server.route("GET", "/api/telegram/groups", [&app](Request& req, BodyReader&,
                                                       Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        if (!app.pool()) {
            sendError(res, 400, "Đang ở chế độ thử nghiệm.");
            return;
        }
        tg::TgAccount* account = nullptr;
        for (const auto& s : app.pool()->statuses()) {
            if (!s.enabled || !s.authorized) continue;
            account = app.pool()->findAccount(s.id);
            if (account) break;
        }
        if (!account) {
            sendError(res, 400, "Chưa có tài khoản Telegram nào đăng nhập.");
            return;
        }
        std::vector<tg::SupergroupRef> groups;
        std::string error;
        if (!account->listSupergroups(groups, error)) {
            sendError(res, 500, error);
            return;
        }
        Json arr = Json::array();
        for (const auto& g : groups) {
            Json j = Json::object();
            j.set("id", g.channelId);
            j.set("title", g.title);
            arr.push(j);
        }
        Json j = okJson();
        j.set("groups", arr);
        res.setJson(j);
    });

    server.route("POST", "/api/telegram/group", [&app](Request& req, BodyReader& body,
                                                       Response& res) {
        AuthContext ctx;
        if (!requireAdmin(app, req, res, ctx)) return;
        body.readAll(req.body, 64 * 1024);
        Json in = req.json();
        std::string error, title;
        if (!app.setSupergroup(in["group"].asString(), error, title)) {
            sendError(res, 400, error);
            return;
        }
        Json j = okJson();
        j.set("title", title);
        j.set("message", "Đã chọn siêu nhóm '" + title + "' làm nơi lưu trữ.");
        res.setJson(j);
    });
}

// ---------------------------------------------------------------------------
//  Giao diện web tĩnh
// ---------------------------------------------------------------------------
void registerStaticRoutes(HttpServer& server, app::App& app) {
    (void)app;
    server.setFallback([](Request& req, BodyReader&, Response& res) {
        if (req.method != "GET" && req.method != "HEAD") {
            res.setError(405, "Phương thức không được hỗ trợ cho đường dẫn này.");
            return;
        }
        std::string path = req.path;
        if (path == "/" || path.empty()) path = "index.html";
        else if (path[0] == '/') path = path.substr(1);

        // Ứng dụng một trang: mọi đường dẫn không phải tài nguyên đều trả về index.html.
        std::string content;
        if (!assets::find(path, content)) {
            if (path.find('.') == std::string::npos) {
                if (!assets::find("index.html", content)) {
                    res.setError(404, "Không tìm thấy giao diện web.");
                    return;
                }
                path = "index.html";
            } else {
                res.setError(404, "Không tìm thấy: /" + path);
                return;
            }
        }
        res.status = 200;
        res.body = std::move(content);
        res.setHeader("Content-Type", guessMimeType(path));
        if (path == "index.html") {
            res.setHeader("Cache-Control", "no-cache");
        } else {
            res.setHeader("Cache-Control", "public, max-age=3600");
        }
        res.setHeader("X-Content-Type-Options", "nosniff");
    });
}

}  // namespace http
}  // namespace ttd
