// Thông tin phiên bản. Giá trị thực được sinh tự động lúc biên dịch
// (xem cmake/GenerateVersion.cmake) — số build tự tăng sau mỗi lần biên dịch.
#pragma once

namespace ttd {
namespace version {

extern const char* kAppName;
extern const char* kAppFooter;
extern const char* kVersion;
extern const char* kGitCommit;
extern const char* kGitBranch;
extern const char* kBuildTimeUtc;
extern long long kBuildEpoch;
extern int kBuildNumber;

}  // namespace version
}  // namespace ttd
