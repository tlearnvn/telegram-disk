// Đoán kiểu MIME theo phần mở rộng tệp.
#pragma once

#include <string>

namespace ttd {
namespace http {

std::string guessMimeType(const std::string& fileName);
// Kiểu tệp rút gọn để hiển thị biểu tượng trên giao diện.
// Trả về một trong: image, video, audio, document, spreadsheet, presentation,
// archive, code, text, pdf, font, other
std::string fileCategory(const std::string& fileName, const std::string& mimeType);
// Kiểu này có nên phát trực tuyến ngay trong trình duyệt không.
bool isStreamable(const std::string& mimeType);
// Kiểu này có an toàn để hiển thị inline không (tránh XSS từ tệp người dùng tải lên).
bool isSafeInline(const std::string& mimeType);

}  // namespace http
}  // namespace ttd
