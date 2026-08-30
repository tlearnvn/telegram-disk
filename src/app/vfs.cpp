#include "app/vfs.h"

#include <algorithm>
#include <map>

#include "common/fsutil.h"
#include "common/logging.h"
#include "common/strutil.h"
#include "common/timeutil.h"
#include "crypto/random.h"
#include "http/mime.h"

namespace ttd {
namespace app {

namespace {
constexpr const char* kTag = "vfs";

VfsResult fail(const std::string& message) {
    VfsResult r;
    r.ok = false;
    r.error = message;
    return r;
}

VfsResult success(const db::FileEntry& entry) {
    VfsResult r;
    r.ok = true;
    r.entry = entry;
    return r;
}

}  // namespace

Vfs::Vfs(db::Database& database, storage::StorageEngine& engine, const Config& config)
    : db_(database), engine_(engine), config_(config) {}

bool Vfs::resolve(const std::string& path, db::FileEntry& out, std::string& error) {
    std::string normalized = normalizeVirtualPath(path);
    if (normalized == "/") {
        out = db::FileEntry();
        out.id = 0;
        out.parentId = -1;
        out.name = "";
        out.path = "/";
        out.isFolder = true;
        return true;
    }
    return db_.getEntryByPath(normalized, out, error);
}

std::string Vfs::uniqueNameIn(int64_t parentId, const std::string& desired) {
    std::string candidate = desired;
    int counter = 1;
    db::FileEntry probe;
    std::string error;
    while (db_.findByNameInFolder(parentId, candidate, probe, error)) {
        ++counter;
        candidate = makeUniqueName(desired, counter);
        if (counter > 999) break;
    }
    return candidate;
}

VfsResult Vfs::ensureFolder(const std::string& path, int ownerId) {
    std::string normalized = normalizeVirtualPath(path);
    if (normalized == "/") {
        db::FileEntry root;
        root.id = 0;
        root.path = "/";
        root.isFolder = true;
        return success(root);
    }
    db::FileEntry existing;
    std::string error;
    if (db_.getEntryByPath(normalized, existing, error)) {
        if (!existing.isFolder) return fail("'" + normalized + "' đã là một tệp.");
        return success(existing);
    }

    int64_t parent = 0;
    std::string current;
    db::FileEntry last;
    for (const auto& part : split(normalized, '/', false)) {
        current += "/" + part;
        db::FileEntry found;
        std::string findError;
        if (db_.getEntryByPath(current, found, findError)) {
            if (!found.isFolder) return fail("'" + current + "' đã là một tệp.");
            parent = found.id;
            last = found;
            continue;
        }
        db::FileEntry folder;
        folder.parentId = parent;
        folder.name = part;
        folder.path = current;
        folder.isFolder = true;
        folder.ownerId = ownerId;
        folder.createdAt = nowUnix();
        folder.modifiedAt = folder.createdAt;
        std::string createError;
        if (!db_.createEntry(folder, createError)) return fail(createError);
        parent = folder.id;
        last = folder;
    }
    return success(last);
}

VfsResult Vfs::createFolder(const std::string& parentPath, const std::string& name,
                            int ownerId) {
    std::string cleanName = sanitizeFileName(name);
    if (cleanName.empty()) return fail("Tên thư mục không hợp lệ.");

    VfsResult parent = ensureFolder(parentPath, ownerId);
    if (!parent.ok) return parent;

    db::FileEntry probe;
    std::string probeError;
    if (db_.findByNameInFolder(parent.entry.id, cleanName, probe, probeError))
        return fail("Trong thư mục này đã có mục tên '" + cleanName + "'.");

    db::FileEntry folder;
    folder.parentId = parent.entry.id;
    folder.name = cleanName;
    folder.path = normalizeVirtualPath(joinPath(parent.entry.path, cleanName));
    folder.isFolder = true;
    folder.ownerId = ownerId;
    folder.createdAt = nowUnix();
    folder.modifiedAt = folder.createdAt;
    std::string error;
    if (!db_.createEntry(folder, error)) return fail(error);
    LOG_INFO(kTag, "Đã tạo thư mục %s", folder.path.c_str());
    return success(folder);
}

bool Vfs::repath(db::FileEntry& entry, const std::string& newPath, std::string& error) {
    std::string oldPath = entry.path;
    entry.path = newPath;
    entry.modifiedAt = nowUnix();
    if (!db_.updateEntry(entry, error)) return false;
    if (entry.isFolder && oldPath != newPath) {
        if (!db_.updatePathsUnder(oldPath, newPath, error)) return false;
    }
    return true;
}

VfsResult Vfs::rename(int64_t id, const std::string& newName) {
    if (id <= 0) return fail("Không thể đổi tên thư mục gốc.");
    std::string cleanName = sanitizeFileName(newName);
    if (cleanName.empty()) return fail("Tên mới không hợp lệ.");

    db::FileEntry entry;
    std::string error;
    if (!db_.getEntry(id, entry, error)) return fail(error);
    if (entry.name == cleanName) return success(entry);

    db::FileEntry probe;
    std::string probeError;
    if (db_.findByNameInFolder(entry.parentId, cleanName, probe, probeError) && probe.id != id)
        return fail("Trong thư mục này đã có mục tên '" + cleanName + "'.");

    std::string parentDir = ttd::parentPath(entry.path);
    std::string newPath = normalizeVirtualPath(joinPath(parentDir, cleanName));
    entry.name = cleanName;
    if (!entry.isFolder) entry.mimeType = http::guessMimeType(cleanName);
    if (!repath(entry, newPath, error)) return fail(error);
    LOG_INFO(kTag, "Đã đổi tên thành %s", newPath.c_str());
    return success(entry);
}

VfsResult Vfs::move(int64_t id, const std::string& newParentPath) {
    if (id <= 0) return fail("Không thể di chuyển thư mục gốc.");
    db::FileEntry entry;
    std::string error;
    if (!db_.getEntry(id, entry, error)) return fail(error);

    VfsResult parent = ensureFolder(newParentPath, entry.ownerId);
    if (!parent.ok) return parent;
    if (parent.entry.id == entry.parentId) return success(entry);

    // Không cho di chuyển thư mục vào chính con cháu của nó.
    if (entry.isFolder) {
        std::string targetPath = normalizeVirtualPath(parent.entry.path);
        if (targetPath == entry.path || startsWith(targetPath, entry.path + "/"))
            return fail("Không thể di chuyển thư mục vào bên trong chính nó.");
    }

    std::string finalName = uniqueNameIn(parent.entry.id, entry.name);
    entry.parentId = parent.entry.id;
    entry.name = finalName;
    std::string newPath = normalizeVirtualPath(joinPath(parent.entry.path, finalName));
    if (!repath(entry, newPath, error)) return fail(error);
    LOG_INFO(kTag, "Đã chuyển tới %s", newPath.c_str());
    return success(entry);
}

VfsResult Vfs::copy(int64_t id, const std::string& newParentPath, int ownerId) {
    db::FileEntry source;
    std::string error;
    if (!db_.getEntry(id, source, error)) return fail(error);

    VfsResult parent = ensureFolder(newParentPath, ownerId);
    if (!parent.ok) return parent;

    if (source.isFolder) {
        std::string targetPath = normalizeVirtualPath(parent.entry.path);
        if (targetPath == source.path || startsWith(targetPath, source.path + "/"))
            return fail("Không thể sao chép thư mục vào bên trong chính nó.");
    }

    std::string finalName = uniqueNameIn(parent.entry.id, source.name);
    db::FileEntry copyEntry = source;
    copyEntry.id = 0;
    copyEntry.parentId = parent.entry.id;
    copyEntry.name = finalName;
    copyEntry.path = normalizeVirtualPath(joinPath(parent.entry.path, finalName));
    copyEntry.createdAt = nowUnix();
    copyEntry.modifiedAt = copyEntry.createdAt;
    copyEntry.ownerId = ownerId;
    copyEntry.shareToken.clear();
    copyEntry.shareExpiresAt = 0;
    copyEntry.trashed = false;
    copyEntry.trashedAt = 0;
    if (!db_.createEntry(copyEntry, error)) return fail(error);

    if (!source.isFolder) {
        // Bản sao dùng chung mảnh dữ liệu — không tốn thêm dung lượng trên Telegram.
        std::vector<db::ChunkEntry> chunks;
        if (db_.listChunks(source.id, chunks, error)) {
            for (auto& c : chunks) {
                c.id = 0;
                c.fileId = copyEntry.id;
                std::string chunkError;
                db_.addChunk(c, chunkError);
            }
        }
    } else {
        // Sao chép đệ quy toàn bộ cây con.
        std::vector<db::FileEntry> children;
        db_.listChildrenRecursive(source.id, children, error);
        // Sắp xếp theo độ sâu để cha luôn được tạo trước con.
        std::sort(children.begin(), children.end(),
                  [](const db::FileEntry& a, const db::FileEntry& b) {
                      return a.path.size() < b.path.size();
                  });
        std::map<int64_t, int64_t> idMap;
        idMap[source.id] = copyEntry.id;
        std::map<int64_t, std::string> pathMap;
        pathMap[source.id] = copyEntry.path;
        for (const auto& child : children) {
            auto itParent = idMap.find(child.parentId);
            if (itParent == idMap.end()) continue;
            db::FileEntry c = child;
            c.id = 0;
            c.parentId = itParent->second;
            c.path = normalizeVirtualPath(joinPath(pathMap[child.parentId], child.name));
            c.createdAt = nowUnix();
            c.modifiedAt = c.createdAt;
            c.ownerId = ownerId;
            c.shareToken.clear();
            c.shareExpiresAt = 0;
            std::string createError;
            if (!db_.createEntry(c, createError)) continue;
            idMap[child.id] = c.id;
            pathMap[child.id] = c.path;
            if (!child.isFolder) {
                std::vector<db::ChunkEntry> chunks;
                std::string chunkError;
                if (db_.listChunks(child.id, chunks, chunkError)) {
                    for (auto& ch : chunks) {
                        ch.id = 0;
                        ch.fileId = c.id;
                        db_.addChunk(ch, chunkError);
                    }
                }
            }
        }
    }
    LOG_INFO(kTag, "Đã sao chép tới %s", copyEntry.path.c_str());
    return success(copyEntry);
}

VfsResult Vfs::trash(int64_t id) {
    if (id <= 0) return fail("Không thể xoá thư mục gốc.");
    db::FileEntry entry;
    std::string error;
    if (!db_.getEntry(id, entry, error)) return fail(error);
    if (entry.trashed) return success(entry);

    entry.trashed = true;
    entry.trashedAt = nowUnix();
    if (!db_.updateEntry(entry, error)) return fail(error);

    if (entry.isFolder) {
        std::vector<db::FileEntry> children;
        db_.listChildrenRecursive(entry.id, children, error);
        for (auto& c : children) {
            if (c.trashed) continue;
            c.trashed = true;
            c.trashedAt = entry.trashedAt;
            std::string updateError;
            db_.updateEntry(c, updateError);
        }
    }
    LOG_INFO(kTag, "Đã chuyển '%s' vào thùng rác", entry.path.c_str());
    return success(entry);
}

VfsResult Vfs::restore(int64_t id) {
    db::FileEntry entry;
    std::string error;
    if (!db_.getEntry(id, entry, error)) return fail(error);
    if (!entry.trashed) return success(entry);

    // Nếu thư mục cha cũng đang ở thùng rác thì khôi phục về gốc.
    if (entry.parentId != 0) {
        db::FileEntry parent;
        std::string parentError;
        if (!db_.getEntry(entry.parentId, parent, parentError) || parent.trashed) {
            entry.parentId = 0;
            entry.name = uniqueNameIn(0, entry.name);
            entry.path = "/" + entry.name;
        }
    }
    entry.trashed = false;
    entry.trashedAt = 0;
    entry.modifiedAt = nowUnix();
    if (!db_.updateEntry(entry, error)) return fail(error);
    if (entry.isFolder) {
        std::vector<db::FileEntry> children;
        db_.listChildrenRecursive(entry.id, children, error);
        for (auto& c : children) {
            c.trashed = false;
            c.trashedAt = 0;
            std::string updateError;
            db_.updateEntry(c, updateError);
        }
    }
    LOG_INFO(kTag, "Đã khôi phục '%s'", entry.path.c_str());
    return success(entry);
}

VfsResult Vfs::purge(int64_t id) {
    db::FileEntry entry;
    std::string error;
    if (!db_.getEntry(id, entry, error)) return fail(error);

    std::vector<db::FileEntry> toDelete;
    if (entry.isFolder) {
        db_.listChildrenRecursive(entry.id, toDelete, error);
        // Xoá từ dưới lên.
        std::sort(toDelete.begin(), toDelete.end(),
                  [](const db::FileEntry& a, const db::FileEntry& b) {
                      return a.path.size() > b.path.size();
                  });
    }
    toDelete.push_back(entry);

    for (const auto& item : toDelete) {
        if (!item.isFolder) {
            std::string purgeError;
            if (!engine_.purgeFileData(item, purgeError))
                LOG_WARN(kTag, "Không xoá hết dữ liệu của '%s': %s", item.path.c_str(),
                         purgeError.c_str());
        }
        std::string deleteError;
        db_.deleteEntry(item.id, deleteError);
    }
    LOG_INFO(kTag, "Đã xoá vĩnh viễn '%s' (%zu mục)", entry.path.c_str(), toDelete.size());
    return success(entry);
}

int Vfs::emptyExpiredTrash() {
    int days = config_.storage.trashRetentionDays;
    if (days <= 0) return 0;
    int64_t cutoff = nowUnix() - static_cast<int64_t>(days) * 86400;

    db::ListOptions opts;
    opts.onlyTrashed = true;
    opts.limit = 500;
    std::vector<db::FileEntry> trashed;
    std::string error;
    if (!db_.listEntries(opts, trashed, error)) return 0;

    int removed = 0;
    for (const auto& item : trashed) {
        if (item.trashedAt == 0 || item.trashedAt > cutoff) continue;
        // Chỉ xử lý mục gốc của cây bị xoá để tránh làm hai lần.
        db::FileEntry parent;
        std::string parentError;
        if (item.parentId != 0 && db_.getEntry(item.parentId, parent, parentError) &&
            parent.trashed)
            continue;
        purge(item.id);
        ++removed;
    }
    if (removed) LOG_INFO(kTag, "Đã dọn %d mục quá hạn khỏi thùng rác", removed);
    return removed;
}

int Vfs::emptyTrash(int ownerId) {
    db::ListOptions opts;
    opts.onlyTrashed = true;
    opts.limit = 2000;
    opts.ownerId = ownerId;
    std::vector<db::FileEntry> trashed;
    std::string error;
    if (!db_.listEntries(opts, trashed, error)) return 0;

    int removed = 0;
    for (const auto& item : trashed) {
        db::FileEntry parent;
        std::string parentError;
        if (item.parentId != 0 && db_.getEntry(item.parentId, parent, parentError) &&
            parent.trashed)
            continue;
        purge(item.id);
        ++removed;
    }
    return removed;
}

VfsResult Vfs::setStarred(int64_t id, bool starred) {
    db::FileEntry entry;
    std::string error;
    if (!db_.getEntry(id, entry, error)) return fail(error);
    entry.starred = starred;
    if (!db_.updateEntry(entry, error)) return fail(error);
    return success(entry);
}

VfsResult Vfs::setNote(int64_t id, const std::string& note) {
    db::FileEntry entry;
    std::string error;
    if (!db_.getEntry(id, entry, error)) return fail(error);
    entry.note = utf8TruncateBytes(note, 4000);
    entry.modifiedAt = nowUnix();
    if (!db_.updateEntry(entry, error)) return fail(error);
    return success(entry);
}

VfsResult Vfs::createShare(int64_t id, int64_t expiresInSeconds) {
    db::FileEntry entry;
    std::string error;
    if (!db_.getEntry(id, entry, error)) return fail(error);
    if (entry.shareToken.empty()) entry.shareToken = crypto::randomToken(18);
    entry.shareExpiresAt = expiresInSeconds > 0 ? nowUnix() + expiresInSeconds : 0;
    if (!db_.updateEntry(entry, error)) return fail(error);
    LOG_INFO(kTag, "Đã tạo liên kết chia sẻ cho '%s'", entry.path.c_str());
    return success(entry);
}

VfsResult Vfs::revokeShare(int64_t id) {
    db::FileEntry entry;
    std::string error;
    if (!db_.getEntry(id, entry, error)) return fail(error);
    entry.shareToken.clear();
    entry.shareExpiresAt = 0;
    if (!db_.updateEntry(entry, error)) return fail(error);
    return success(entry);
}

bool Vfs::resolveShare(const std::string& token, db::FileEntry& out, std::string& error) {
    if (!db_.getEntryByShareToken(token, out, error)) return false;
    if (out.shareExpiresAt != 0 && out.shareExpiresAt < nowUnix()) {
        error = "Liên kết chia sẻ đã hết hạn.";
        return false;
    }
    return true;
}

uint64_t Vfs::folderSize(int64_t folderId) {
    std::vector<db::FileEntry> children;
    std::string error;
    if (!db_.listChildrenRecursive(folderId, children, error)) return 0;
    uint64_t total = 0;
    for (const auto& c : children)
        if (!c.isFolder && !c.trashed) total += c.size;
    return total;
}

std::vector<db::FileEntry> Vfs::breadcrumb(int64_t id) {
    std::vector<db::FileEntry> chain;
    int64_t current = id;
    int guard = 0;
    while (current > 0 && ++guard < 64) {
        db::FileEntry entry;
        std::string error;
        if (!db_.getEntry(current, entry, error)) break;
        chain.push_back(entry);
        current = entry.parentId;
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

}  // namespace app
}  // namespace ttd
