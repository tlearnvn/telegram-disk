// Tài nguyên tĩnh được nhúng thẳng vào tệp thực thi (giao diện web + schema TL).
// Có thể ghi đè bằng tệp thật trên đĩa để tiện phát triển hoặc nâng cấp schema.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ttd {
namespace assets {

struct Asset {
    std::string path;
    const unsigned char* data = nullptr;
    size_t size = 0;
};

// Bật chế độ đọc tài nguyên từ thư mục trên đĩa (ưu tiên hơn bản nhúng).
void setOverrideDirectory(const std::string& dir);
const std::string& overrideDirectory();

// Tìm tài nguyên theo đường dẫn tương đối ("index.html", "schema/api.tl").
// Trả về true nếu tìm thấy; nội dung được đặt vào `out`.
bool find(const std::string& path, std::string& out);
// Trả về con trỏ tới chuỗi kết thúc bằng NUL của tài nguyên nhúng (không đọc đĩa).
const char* findTextAsset(const std::string& path);

std::vector<std::string> listPaths();

}  // namespace assets
}  // namespace ttd
