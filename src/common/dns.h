// Phân giải tên miền. Ưu tiên getaddrinfo của hệ điều hành; nếu không dùng được
// (thường gặp với bản liên kết tĩnh hoàn toàn trên glibc) thì rơi về bộ phân giải
// DNS nội bộ tự cài đặt qua UDP/TCP.
#pragma once

#include <string>
#include <vector>

namespace ttd {
namespace dns {

// Trả về danh sách địa chỉ IPv4/IPv6 dạng chuỗi. Rỗng nếu thất bại.
// Kết quả được nhớ tạm trong bộ đệm để tránh tra cứu lặp lại.
std::vector<std::string> resolve(const std::string& host, int timeoutMs = 8000);

// Xoá bộ đệm phân giải.
void clearCache();

// Bộ phân giải nội bộ (dùng trực tiếp khi cần bỏ qua hệ điều hành).
std::vector<std::string> resolveBuiltin(const std::string& host, int timeoutMs);

// Danh sách máy chủ DNS đang dùng (đọc từ /etc/resolv.conf hoặc mặc định).
std::vector<std::string> nameServers();

}  // namespace dns
}  // namespace ttd
