// Các thao tác trên cây thư mục ảo: tạo, đổi tên, di chuyển, sao chép,
// xoá vào thùng rác, khôi phục, chia sẻ.
#pragma once

#include <string>
#include <vector>

#include "common/config.h"
#include "db/database.h"
#include "storage/storage_engine.h"

namespace ttd {
namespace app {

struct VfsResult {
    bool ok = false;
    std::string error;
    db::FileEntry entry;
};

class Vfs {
public:
    Vfs(db::Database& database, storage::StorageEngine& engine, const Config& config);

    // Tìm mục theo đường dẫn ảo. Trả về false nếu không có.
    bool resolve(const std::string& path, db::FileEntry& out, std::string& error);
    // Bảo đảm chuỗi thư mục tồn tại, trả về mục thư mục cuối cùng.
    VfsResult ensureFolder(const std::string& path, int ownerId);
    VfsResult createFolder(const std::string& parentPath, const std::string& name, int ownerId);

    VfsResult rename(int64_t id, const std::string& newName);
    VfsResult move(int64_t id, const std::string& newParentPath);
    VfsResult copy(int64_t id, const std::string& newParentPath, int ownerId);

    // Chuyển vào thùng rác (có thể khôi phục).
    VfsResult trash(int64_t id);
    VfsResult restore(int64_t id);
    // Xoá vĩnh viễn kèm dữ liệu trên Telegram.
    VfsResult purge(int64_t id);
    // Dọn thùng rác quá hạn.
    int emptyExpiredTrash();
    int emptyTrash(int ownerId);

    VfsResult setStarred(int64_t id, bool starred);
    VfsResult setNote(int64_t id, const std::string& note);
    // Tạo liên kết chia sẻ; expiresInSeconds = 0 nghĩa là không hết hạn.
    VfsResult createShare(int64_t id, int64_t expiresInSeconds);
    VfsResult revokeShare(int64_t id);
    bool resolveShare(const std::string& token, db::FileEntry& out, std::string& error);

    // Tổng dung lượng của một thư mục (tính đệ quy).
    uint64_t folderSize(int64_t folderId);
    // Danh sách đường dẫn từ gốc tới mục (breadcrumb).
    std::vector<db::FileEntry> breadcrumb(int64_t id);

private:
    // Đổi đường dẫn của mục và toàn bộ con cháu.
    bool repath(db::FileEntry& entry, const std::string& newPath, std::string& error);
    std::string uniqueNameIn(int64_t parentId, const std::string& desired);

    db::Database& db_;
    storage::StorageEngine& engine_;
    const Config& config_;
};

}  // namespace app
}  // namespace ttd
